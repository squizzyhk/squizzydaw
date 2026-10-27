// IIndexScanner: incremental folder scan on a private worker thread.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sq/cpp/attributes.hpp"
#include "sq/cpp/object.hpp"
#include "sq/platform.hpp"
#include "sqlite_index.hpp"

namespace sq::sqlite_index {
namespace {

std::string normalizePath(const std::string& in) {
  std::string p = in;
  Buffer abs;
  if (sq_fs_absolute(in.c_str(), abs.out()) == SQ_OK && abs.size()) p = abs.str();
  for (char& c : p)
    if (c == '\\') c = '/';
  while (p.size() > 1 && p.back() == '/' && !(p.size() == 3 && p[1] == ':')) p.pop_back();
  return p;
}

std::string lowerExt(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || dot == 0) return {};
  std::string e = name.substr(dot + 1);
  for (char& c : e)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return e;
}

// Identity of a directory with links / junctions resolved ("" if it cannot be resolved).
std::string canonicalDir(const std::string& utf8) {
  std::error_code ec;
  const std::filesystem::path p(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
  const std::filesystem::path c = std::filesystem::canonical(p, ec);
  if (ec) return {};
  const std::u8string u = c.u8string();
  std::string out(reinterpret_cast<const char*>(u.data()), u.size());
#ifdef _WIN32
  for (char& ch : out)
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
#endif
  return out;
}

// Deeper than any sane library; stops pathological link chains that canonical() cannot see.
constexpr size_t kMaxScanDepth = 64;

class IndexScanner final : public Object<IndexScanner, IIndexScanner> {
 public:
  IndexScanner() { sq_event_create(SQ_TRUE, SQ_TRUE, &done_); }
  ~IndexScanner() override {
    cancel_.store(true);
    joinThread();
    if (done_) sq_event_destroy(done_);
  }

  sq_result SQ_CALL start(IContentIndex* index, const sq_index_scan_options* o) override {
    if (!index || !o || o->struct_size < sizeof(sq_index_scan_options) || !o->root || !*o->root) return SQ_E_INVALIDARG;
    if (!done_) return SQ_E_FAIL;
    std::lock_guard<std::mutex> lock(m_);
    if (running_.load()) return SQ_E_BUSY;
    joinThreadLocked();
    try {
      if (!sq_fs_is_dir(o->root)) return SQ_E_NOTFOUND;
      root_ = normalizePath(o->root);
      extensions_.clear();
      if (o->extensions && *o->extensions) {
        for (const std::string& e : splitTags(o->extensions)) extensions_.insert(e[0] == '.' ? e.substr(1) : e);
      }
      extraTags_ = splitTags(o->extra_tags);
      flags_ = o->flags;
      batchSize_ = o->batch_size ? o->batch_size : 256;
      index_ = Ptr<IContentIndex>(index);
    } catch (...) {
      return SQ_E_OUTOFMEMORY;
    }
    seen_ = indexed_ = unchanged_ = removed_ = errors_ = 0;
    result_ = SQ_OK;
    cancel_.store(false);
    startedNs_ = sq_time_monotonic_ns();
    endedNs_ = 0;
    state_.store(SQ_INDEX_SCAN_RUNNING);
    running_.store(true);
    sq_event_reset(done_);
    if (sq_thread_create(&IndexScanner::threadMain, this, "sq-index-scan", &thread_) != SQ_OK) {
      thread_ = nullptr;
      running_.store(false);
      state_.store(SQ_INDEX_SCAN_FAILED);
      result_ = SQ_E_FAIL;
      index_.reset();
      sq_event_set(done_);
      return SQ_E_FAIL;
    }
    return SQ_OK;
  }

  sq_result SQ_CALL cancel() override {
    if (!running_.load()) return SQ_S_FALSE;
    cancel_.store(true);
    return SQ_OK;
  }

  sq_result SQ_CALL wait(int32_t timeoutMs) override {
    if (!done_) return SQ_E_FAIL;
    if (sq_event_wait(done_, timeoutMs) != SQ_OK) return SQ_E_TIMEOUT;
    joinThread();
    return SQ_OK;
  }

  sq_result SQ_CALL getStatus(sq_index_scan_status* out) override {
    if (!out || out->struct_size < sizeof(sq_index_scan_status)) return SQ_E_INVALIDARG;
    out->state = state_.load();
    out->result = result_.load();
    out->files_seen = seen_.load();
    out->files_indexed = indexed_.load();
    out->files_unchanged = unchanged_.load();
    out->files_removed = removed_.load();
    out->errors = errors_.load();
    const uint64_t s = startedNs_.load(), e = endedNs_.load();
    out->elapsed_ms = s ? static_cast<int64_t>(((e ? e : sq_time_monotonic_ns()) - s) / 1000000u) : 0;
    return SQ_OK;
  }

 private:
  static void threadMain(void* arg) {
    auto* self = static_cast<IndexScanner*>(arg);
    sq_thread_set_current_priority(SQ_PRIORITY_LOW);
    sq_result r = SQ_E_FAIL;
    try {
      r = self->run();
    } catch (...) {
      r = SQ_E_FAIL;
    }
    self->finish(r);
  }

  void finish(sq_result r) {
    // index_ is only touched by the worker while running_ is set.
    index_.reset();
    result_.store(r);
    endedNs_.store(sq_time_monotonic_ns());
    state_.store(cancel_.load() ? SQ_INDEX_SCAN_CANCELLED : (SQ_FAILED(r) ? SQ_INDEX_SCAN_FAILED : SQ_INDEX_SCAN_DONE));
    running_.store(false);
    sq_event_set(done_);
  }

  void joinThread() {
    std::lock_guard<std::mutex> lock(m_);
    joinThreadLocked();
  }
  void joinThreadLocked() {
    if (thread_) {
      sq_thread_join(thread_);
      thread_ = nullptr;
    }
  }

  bool wanted(const std::string& ext) const {
    if (ext.empty() || !kindForExtension(ext)) return false;
    return extensions_.empty() || extensions_.count(ext) != 0;
  }

  struct Known {
    int64_t mtime;
    int64_t size;
  };

  sq_result run() {
    IContentIndex* ix = index_.get();
    const std::string prefix = root_ + (root_.back() == '/' ? "" : "/");

    // What the index already knows below the root (for incremental skip + prune).
    std::unordered_map<std::string, Known> known;
    {
      sq_index_query q{};
      q.struct_size = sizeof q;
      q.url_prefix = prefix.c_str();
      Ptr<IIndexResult> res;
      sq_result r = ix->query(&q, res.put());
      if (SQ_FAILED(r)) return r;
      const uint32_t n = res->getCount();
      known.reserve(n);
      for (uint32_t k = 0; k < n; ++k) {
        sq_index_row row{};
        row.struct_size = sizeof row;
        if (res->getRow(k, &row) == SQ_OK) known.emplace(row.url, Known{row.mtime, row.size});
      }
    }
    std::unordered_set<std::string> present;

    uint32_t inBatch = 0;
    bool batchOpen = false;
    auto closeBatch = [&] {
      if (batchOpen) {
        if (SQ_FAILED(ix->commitBatch())) errors_.fetch_add(1);
        batchOpen = false;
        inBatch = 0;
      }
    };
    // Never leave the shared index inside an open batch, even on an exception.
    struct BatchGuard {
      IContentIndex* ix;
      bool* open;
      ~BatchGuard() {
        if (*open) ix->commitBatch();
      }
    } guard{ix, &batchOpen};

    // Directories whose listing failed: their (unseen) items must not be pruned.
    std::vector<std::string> failedDirs;
    // Directories already walked, by resolved identity (junction / symlink loops, aliases).
    std::unordered_set<std::string> visited;
    struct Pending {
      std::string path;
      size_t depth;
    };
    std::vector<Pending> dirs{{root_, 0}};
    while (!dirs.empty() && !cancel_.load()) {
      const std::string dir = std::move(dirs.back().path);
      const size_t depth = dirs.back().depth;
      dirs.pop_back();
      const std::string id = canonicalDir(dir);
      if (!id.empty() && !visited.insert(id).second) continue;  // already walked via another path
      struct Entry {
        std::string name;
        bool isDir;
      };
      std::vector<Entry> entries;
      const sq_result listed = sq_fs_list_dir(
          dir.c_str(),
          [](void* user, const char* name, sq_bool isDir) -> sq_bool {
            static_cast<std::vector<Entry>*>(user)->push_back({name, isDir != SQ_FALSE});
            return SQ_TRUE;
          },
          &entries);
      if (SQ_FAILED(listed)) {
        errors_.fetch_add(1);
        failedDirs.push_back(dir + (dir.back() == '/' ? "" : "/"));
      }
      std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
      std::vector<std::string> subdirs;
      for (const Entry& e : entries) {
        if (cancel_.load()) break;
        if (e.name.empty() || e.name[0] == '.') continue;  // hidden files / folders
        const std::string path = dir + (dir.back() == '/' ? "" : "/") + e.name;
        if (e.isDir) {
          if ((flags_ & SQ_INDEX_SCAN_RECURSIVE) && depth < kMaxScanDepth) subdirs.push_back(path);
          continue;
        }
        const std::string ext = lowerExt(e.name);
        if (!wanted(ext)) continue;
        seen_.fetch_add(1);
        present.insert(path);

        uint64_t size = 0;
        int64_t mtimeMs = 0;
        sq_fs_file_size(path.c_str(), &size);
        sq_fs_mtime(path.c_str(), &mtimeMs);
        const int64_t mtime = mtimeMs / 1000;
        if (!(flags_ & SQ_INDEX_SCAN_FORCE)) {
          auto it = known.find(path);
          if (it != known.end() && it->second.mtime == mtime && it->second.size == static_cast<int64_t>(size)) {
            unchanged_.fetch_add(1);
            continue;
          }
        }

        FileInfo info;
        if (!extractFileInfo(path, info)) {
          errors_.fetch_add(1);
          continue;
        }
        for (const auto& t : extraTags_)
          if (std::find(info.tags.begin(), info.tags.end(), t) == info.tags.end()) info.tags.push_back(t);
        Ptr<IAttributes> meta = makeAttributes();
        for (const MetaValue& mv : info.meta) {
          switch (mv.type) {
            case SQ_VALUE_INT: meta->setInt(mv.key.c_str(), mv.i); break;
            case SQ_VALUE_DOUBLE: meta->setDouble(mv.key.c_str(), mv.d); break;
            case SQ_VALUE_BOOL: meta->setBool(mv.key.c_str(), mv.i ? SQ_TRUE : SQ_FALSE); break;
            default: meta->setString(mv.key.c_str(), mv.s.c_str()); break;
          }
        }
        const std::string tags = joinTags(info.tags);
        sq_index_item item{};
        item.struct_size = sizeof item;
        item.url = path.c_str();
        item.kind = info.kind.c_str();
        item.title = info.title.c_str();
        item.tags = tags.c_str();
        item.mtime = mtime;
        item.size = static_cast<int64_t>(size);

        if (!batchOpen) {
          sq_result r = ix->beginBatch();
          if (SQ_FAILED(r)) return r;  // store closed / read-only: nothing more can be done
          batchOpen = true;
        }
        if (SQ_SUCCEEDED(ix->putItem(&item, meta.get(), nullptr))) indexed_.fetch_add(1);
        else errors_.fetch_add(1);
        if (++inBatch >= batchSize_) closeBatch();
      }
      // depth-first, in name order
      for (auto it = subdirs.rbegin(); it != subdirs.rend(); ++it) dirs.push_back({std::move(*it), depth + 1});
    }

    if (!cancel_.load() && (flags_ & SQ_INDEX_SCAN_PRUNE)) {
      for (const auto& kv : known) {
        if (cancel_.load()) break;
        if (present.count(kv.first)) continue;
        // Outside a recursive scan, only prune direct children of the root.
        if (!(flags_ & SQ_INDEX_SCAN_RECURSIVE) && kv.first.find('/', prefix.size()) != std::string::npos) continue;
        // Only types this scan looked for (an extension-filtered scan must not drop the rest).
        const size_t slash = kv.first.find_last_of('/');
        if (!wanted(lowerExt(slash == std::string::npos ? kv.first : kv.first.substr(slash + 1)))) continue;
        // Never prune below a folder that could not be listed (permissions, offline share).
        bool underFailed = false;
        for (const auto& f : failedDirs)
          if (kv.first.compare(0, f.size(), f) == 0) underFailed = true;
        if (underFailed) continue;
        if (!batchOpen) {
          if (SQ_FAILED(ix->beginBatch())) break;
          batchOpen = true;
        }
        if (ix->removeItem(kv.first.c_str()) == SQ_OK) removed_.fetch_add(1);
        if (++inBatch >= batchSize_) closeBatch();
      }
    }
    closeBatch();
    return cancel_.load() ? SQ_S_FALSE : SQ_OK;
  }

  std::mutex m_;
  sq_thread* thread_ = nullptr;
  sq_event* done_ = nullptr;
  std::atomic<bool> running_{false};
  std::atomic<bool> cancel_{false};
  std::atomic<int32_t> state_{SQ_INDEX_SCAN_IDLE};
  std::atomic<sq_result> result_{SQ_OK};
  std::atomic<uint64_t> seen_{0}, indexed_{0}, unchanged_{0}, removed_{0}, errors_{0};
  std::atomic<uint64_t> startedNs_{0}, endedNs_{0};

  // Written by start() before the thread exists, read by the worker only.
  std::string root_;
  std::unordered_set<std::string> extensions_;
  std::vector<std::string> extraTags_;
  uint32_t flags_ = 0;
  uint32_t batchSize_ = 256;
  Ptr<IContentIndex> index_;
};

}  // namespace

Ptr<IIndexScanner> createIndexScanner() { return Ptr<IIndexScanner>::adopt(new IndexScanner()); }

}  // namespace sq::sqlite_index
