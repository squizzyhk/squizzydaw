// services/pluginscanner: IPluginRegistry service (host->queryService) over a JSON manifest, filled
// by the out-of-process scanner sq-pluginscan (one child process per plugin file).
//
// Settings (all optional):
//   Services.pluginscanner.Manifest        manifest path (default <user config>/plugins.json)
//   Services.pluginscanner.Paths           extra search folders, ';' separated (scanned first)
//   Services.pluginscanner.UseDefaultPaths bool, default true: also the formats' standard folders
//   Services.pluginscanner.TimeoutMs       per-file scan timeout (default 15000)
//   Services.pluginscanner.ScannerExe      scanner executable (default <exe dir>/sq-pluginscan)
//   Services.pluginscanner.ServicesDir     where format modules live (default <exe dir>/services)
// Quarantine: a file whose scan crashed or timed out is recorded with status CRASHED / TIMEOUT and
// is not scanned again until it changes (mtime/size) or a full rescan is requested. Such entries
// are never instantiated. setBlocked() entries persist in the manifest ("blocked").
// Threading: rescan() collects the format adapters on the calling (main) thread and runs the
// scanner processes on a background thread; results are merged under a mutex and the manifest is
// written when the scan ends. Observers ("plugins.changed") are notified from the main thread by
// the first isScanning()/getPluginCount() call after the scan finished.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "manifest.hpp"
#include "spawn.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/sdk.hpp"

namespace sq::pscan {
namespace {

struct FormatEntry {
  std::string format;     // "clap"
  std::string extension;  // "clap"
  sq_guid classId{};
  std::vector<std::string> paths;
};

std::string settingStr(ISettings* s, const char* key, const std::string& def = {}) { return sq::getString(s, key, def); }

std::vector<std::string> splitList(const std::string& s) {
  std::vector<std::string> out;
  size_t p = 0;
  while (p <= s.size()) {
    size_t e = s.find(';', p);
    std::string part = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
    if (!part.empty()) out.push_back(part);
    if (e == std::string::npos) break;
    p = e + 1;
  }
  return out;
}

std::string lowerExt(const std::string& path) {
  size_t slash = path.find_last_of("/\\");
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
  std::string e = path.substr(dot + 1);
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e;
}

// Plugin files (or bundles) with extension `ext` below dir, depth-limited.
void collect(const std::string& dir, const std::string& ext, int depth, std::vector<std::string>& out) {
  if (depth > 8 || out.size() > 10000) return;
  struct Ctx {
    std::vector<std::pair<std::string, bool>> entries;
  } ctx;
  sq_fs_list_dir(
      dir.c_str(),
      [](void* user, const char* name, sq_bool isDir) -> sq_bool {
        static_cast<Ctx*>(user)->entries.emplace_back(name, isDir != 0);
        return SQ_TRUE;
      },
      &ctx);
  std::sort(ctx.entries.begin(), ctx.entries.end());
  for (const auto& [name, isDir] : ctx.entries) {
    std::string full = sq::joinPath(dir, name);
    if (lowerExt(name) == ext) out.push_back(full);  // bundles (macOS .clap dirs) count as files
    else if (isDir) collect(full, ext, depth + 1, out);
  }
}

class PluginRegistry final : public sq::Object<PluginRegistry, IPluginRegistry, ISubject> {
 public:
  explicit PluginRegistry(IHostContext* host) : host_(host) {
    ISettings* s = host_->getSettings();
    std::string exeDir = sq::specialDir(SQ_DIR_EXE);
    manifestPath_ = settingStr(s, "Services.pluginscanner.Manifest", sq::joinPath(sq::specialDir(SQ_DIR_USER_CONFIG), "plugins.json"));
    if (auto txt = sq::readFile(manifestPath_)) {
      std::string err;
      if (!readManifest(*txt, plugins_, blocked_, &err)) sq::logf(SQ_LOG_WARN, "manifest %s ignored: %s", manifestPath_.c_str(), err.c_str());
    }
    applyBlocked();
  }
  ~PluginRegistry() override { joinWorker(); }

  void shutdown() {
    cancel_ = true;
    joinWorker();
    observers_.clear();
  }

  // ---- IPluginRegistry ----
  uint32_t SQ_CALL getPluginCount() override {
    pumpNotifications();
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<uint32_t>(plugins_.size());
  }
  sq_result SQ_CALL getPlugin(uint32_t index, sq_plugin_desc* out) override {
    if (!out) return SQ_E_INVALIDARG;
    std::lock_guard<std::mutex> lk(mu_);
    if (index >= plugins_.size()) return SQ_E_NOTFOUND;
    *out = plugins_[index];
    return SQ_OK;
  }
  sq_result SQ_CALL rescan(sq_bool full) override {
    if (scanning_.load()) return SQ_E_BUSY;
    joinWorker();
    ISettings* s = host_->getSettings();
    std::vector<FormatEntry> formats = collectFormats(s);
    std::string exe = settingStr(s, "Services.pluginscanner.ScannerExe",
                                 sq::joinPath(sq::specialDir(SQ_DIR_EXE), std::string("sq-pluginscan") + exeSuffix()));
    std::string svcDir = settingStr(s, "Services.pluginscanner.ServicesDir", sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"));
    uint32_t timeout = static_cast<uint32_t>(std::clamp<int64_t>(s->getInt("Services.pluginscanner.TimeoutMs", 15000), 100, 600000));
    if (!sq_fs_exists(exe.c_str())) {
      sq::logf(SQ_LOG_ERROR, "scanner executable missing: %s", exe.c_str());
      return SQ_E_NOTFOUND;
    }
    scanning_ = true;
    cancel_ = false;
    worker_ = std::thread([this, formats, exe, svcDir, timeout, full] { scanAll(formats, exe, svcDir, timeout, full != 0); });
    return SQ_OK;
  }
  sq_bool SQ_CALL isScanning() override {
    pumpNotifications();
    return scanning_.load() ? SQ_TRUE : SQ_FALSE;
  }
  sq_result SQ_CALL setBlocked(const char* format, const char* id, sq_bool blocked) override {
    if (!format || !id || !*format || !*id) return SQ_E_INVALIDARG;
    {
      std::lock_guard<std::mutex> lk(mu_);
      std::string key = std::string(format) + ":" + id;
      auto it = std::find(blocked_.begin(), blocked_.end(), key);
      if (blocked && it == blocked_.end()) blocked_.push_back(key);
      if (!blocked && it != blocked_.end()) blocked_.erase(it);
      for (auto& d : plugins_) {
        if (std::string(d.format) + ":" + d.id != key) continue;
        if (blocked) d.status = SQ_PLUGIN_STATUS_BLOCKED;
        else if (d.status == SQ_PLUGIN_STATUS_BLOCKED) d.status = SQ_PLUGIN_STATUS_OK;
      }
    }
    saveManifest();
    notify();
    return SQ_OK;
  }
  sq_result SQ_CALL instantiate(const sq_plugin_desc* desc, IPluginHostCallback* cb, IPluginInstance** out) override {
    if (!desc || !out) return SQ_E_INVALIDARG;
    *out = nullptr;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (std::find(blocked_.begin(), blocked_.end(), std::string(desc->format) + ":" + desc->id) != blocked_.end()) return SQ_E_ACCESS;
      for (const auto& d : plugins_)
        if (std::strcmp(d.path, desc->path) == 0 && d.status != SQ_PLUGIN_STATUS_OK) return SQ_E_ACCESS;  // quarantined file
    }
    if (desc->status != SQ_PLUGIN_STATUS_OK) return SQ_E_ACCESS;
    for (const auto& f : collectFormats(host_->getSettings(), false)) {
      if (f.format != desc->format) continue;
      auto fmt = sq::createInstance<IPluginFormat>(host_->getClassRegistry(), f.classId);
      if (!fmt) return SQ_E_NOTFOUND;
      return fmt->instantiate(desc, cb, out);
    }
    return SQ_E_NOTFOUND;
  }

  // ---- ISubject ----
  sq_result SQ_CALL addObserver(IObserver* o) override {
    if (!o) return SQ_E_INVALIDARG;
    observers_.push_back(sq::Ptr<IObserver>(o));
    return SQ_OK;
  }
  sq_result SQ_CALL removeObserver(IObserver* o) override {
    for (auto it = observers_.begin(); it != observers_.end(); ++it)
      if (it->get() == o) {
        observers_.erase(it);
        return SQ_OK;
      }
    return SQ_E_NOTFOUND;
  }

 private:
  static const char* exeSuffix() {
#ifdef _WIN32
    return ".exe";
#else
    return "";
#endif
  }

  std::vector<FormatEntry> collectFormats(ISettings* s, bool withPaths = true) {
    std::vector<FormatEntry> out;
    IClassRegistry* reg = host_->getClassRegistry();
    std::vector<std::string> extra = splitList(settingStr(s, "Services.pluginscanner.Paths"));
    bool defaults = s->getBool("Services.pluginscanner.UseDefaultPaths", SQ_TRUE) != 0;
    for (uint32_t i = 0; i < reg->getClassCount(); ++i) {
      sq_class_info ci{};
      ci.struct_size = sizeof ci;
      if (reg->getClassInfo(i, &ci) != SQ_OK || !ci.category || std::strcmp(ci.category, pluginhost::kCategory) != 0) continue;
      auto fmt = sq::createInstance<IPluginFormat>(reg, ci.class_id);
      if (!fmt) continue;
      sq_plugin_format_info fi{};
      fi.struct_size = sizeof fi;
      if (fmt->getFormatInfo(&fi) != SQ_OK) continue;
      FormatEntry e;
      e.format = fi.format;
      e.extension = fi.file_extension;
      e.classId = ci.class_id;
      if (withPaths) {
        e.paths = extra;
        if (defaults)
          for (uint32_t k = 0; k < fmt->getSearchPathCount(); ++k) {
            char buf[2048];
            uint32_t need = 0;
            if (fmt->getSearchPath(k, buf, sizeof buf, &need) == SQ_OK) e.paths.emplace_back(buf);
          }
      }
      out.push_back(std::move(e));
    }
    return out;
  }

  // Background thread.
  void scanAll(std::vector<FormatEntry> formats, std::string exe, std::string svcDir, uint32_t timeout, bool full) {
    std::string tmpDir = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "sq-pluginscan-" + std::to_string(sq_process_id()));
    sq_fs_create_dirs(tmpDir.c_str());
    std::set<std::string> seen;
    int n = 0;
    for (const auto& f : formats) {
      std::string module = sq::joinPath(svcDir, "pluginhost-" + f.format + sq_dylib_suffix());
      std::vector<std::string> files;
      for (const auto& p : f.paths) collect(p, f.extension, 0, files);
      for (const auto& file : files) {
        if (cancel_.load()) break;
        if (!seen.insert(file).second) continue;
        int64_t mtimeMs = 0;
        uint64_t size = 0;
        sq_fs_mtime(file.c_str(), &mtimeMs);
        sq_fs_file_size(file.c_str(), &size);
        if (!full && upToDate(f.format, file, mtimeMs / 1000, static_cast<int64_t>(size))) continue;
        std::string out = sq::joinPath(tmpDir, "scan-" + std::to_string(++n) + ".json");
        sq_fs_remove(out.c_str());
        SpawnResult r = runProcess({exe, "--module", module, "--class", sq::toString(f.classId), "--out", out, file}, timeout);
        std::vector<sq_plugin_desc> found;
        std::vector<std::string> unused;
        int32_t status = SQ_PLUGIN_STATUS_OK;
        auto txt = sq::readFile(out);
        if (r.timedOut) status = SQ_PLUGIN_STATUS_TIMEOUT;
        else if (!r.started || r.crashed || !txt) status = SQ_PLUGIN_STATUS_CRASHED;
        else if (r.exitCode != 0 || !readManifest(*txt, found, unused)) status = SQ_PLUGIN_STATUS_INVALID;
        sq_fs_remove(out.c_str());
        if (status != SQ_PLUGIN_STATUS_OK) {
          sq_plugin_desc q{};
          q.struct_size = sizeof q;
          copyField(q.format, sizeof q.format, f.format);
          copyField(q.id, sizeof q.id, "file:" + file);
          copyField(q.name, sizeof q.name, file.substr(file.find_last_of("/\\") + 1));
          copyField(q.path, sizeof q.path, file);
          q.status = status;
          found.assign(1, q);
          sq_log_printf(SQ_LOG_WARN, "pluginscanner", "%s: %s", file.c_str(),
                        status == SQ_PLUGIN_STATUS_TIMEOUT ? "scan timed out (quarantined)"
                        : status == SQ_PLUGIN_STATUS_CRASHED ? "scanner crashed (quarantined)" : "invalid plugin");
        }
        for (auto& d : found) {
          d.file_mtime = mtimeMs / 1000;
          d.file_size = static_cast<int64_t>(size);
          copyField(d.path, sizeof d.path, file);
        }
        replaceFile(file, found);
      }
    }
    sq_fs_remove_all(tmpDir.c_str());
    {
      std::lock_guard<std::mutex> lk(mu_);
      applyBlockedLocked();
    }
    saveManifest();
    pendingNotify_ = true;
    scanning_ = false;
  }

  bool upToDate(const std::string& format, const std::string& file, int64_t mtime, int64_t size) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& d : plugins_)
      if (format == d.format && file == d.path) return d.file_mtime == mtime && d.file_size == size;
    return false;
  }
  void replaceFile(const std::string& file, const std::vector<sq_plugin_desc>& found) {
    std::lock_guard<std::mutex> lk(mu_);
    plugins_.erase(std::remove_if(plugins_.begin(), plugins_.end(), [&](const sq_plugin_desc& d) { return file == d.path; }), plugins_.end());
    plugins_.insert(plugins_.end(), found.begin(), found.end());
  }
  void applyBlocked() {
    std::lock_guard<std::mutex> lk(mu_);
    applyBlockedLocked();
  }
  void applyBlockedLocked() {
    for (auto& d : plugins_)
      if (std::find(blocked_.begin(), blocked_.end(), std::string(d.format) + ":" + d.id) != blocked_.end()) d.status = SQ_PLUGIN_STATUS_BLOCKED;
  }
  void saveManifest() {
    std::string txt;
    {
      std::lock_guard<std::mutex> lk(mu_);
      txt = writeManifest(plugins_, blocked_);
    }
    if (sq::writeFile(manifestPath_, txt) != SQ_OK) sq_log_printf(SQ_LOG_WARN, "pluginscanner", "cannot write %s", manifestPath_.c_str());
  }
  void pumpNotifications() {
    if (pendingNotify_.exchange(false)) {
      joinWorker();
      notify();
    }
  }
  void notify() {
    auto args = sq::makeAttributes();
    auto copy = observers_;
    for (auto& o : copy) o->onNotify(static_cast<IPluginRegistry*>(this), "plugins.changed", args.get());
  }
  void joinWorker() {
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
  }

  IHostContext* host_;
  std::string manifestPath_;
  std::mutex mu_;
  std::vector<sq_plugin_desc> plugins_;
  std::vector<std::string> blocked_;
  std::vector<sq::Ptr<IObserver>> observers_;
  std::thread worker_;
  std::atomic<bool> scanning_{false}, cancel_{false}, pendingNotify_{false};
};

PluginRegistry* g_registry = nullptr;

sq_result onInit(IHostContext* host) {
  g_registry = new PluginRegistry(host);
  sq_result r = host->registerService(&IPluginRegistry::iid, static_cast<IPluginRegistry*>(g_registry));
  if (SQ_FAILED(r)) {
    g_registry->release();
    g_registry = nullptr;
  }
  return r;
}

void onShutdown() {
  if (!g_registry) return;
  g_registry->shutdown();
  sq::host()->unregisterService(&IPluginRegistry::iid, static_cast<IPluginRegistry*>(g_registry));
  g_registry->release();
  g_registry = nullptr;
}

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('P', 'S', 'C', 'N'),
    .name = "pluginscanner",
    .description = "Plugin registry + out-of-process scanner (quarantine, blocklist)",
    .classes = {},
    .onInit = &onInit,
    .onShutdown = &onShutdown,
};

}  // namespace
}  // namespace sq::pscan

SQ_MODULE_EXPORTS(sq::pscan::kModule)
