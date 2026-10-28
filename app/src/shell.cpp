// Squizzy Studio app shell - see sq/app/shell.hpp.
#include "sq/app/shell.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <tuple>

#include "sq/cpp/attributes.hpp"
#include "sq/interfaces/codec.h"
#include "sq/interfaces/index.h"
#include "sq/interfaces/midi.h"
#include "sq/interfaces/asio.h"
#include "sq/interfaces/audiodevice.h"
#include "sq/interfaces/remote.h"
#include "sq/interfaces/surface.h"
#include "sq/interfaces/video.h"
#include "sq/platform.hpp"
#include "sq/ui/remote_pages.hpp"
#include "sq/ui/theme.hpp"

#if SQ_SHELL_ENGINE
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "sq/engine/audio/automation.hpp"
#include "sq/engine/audio/builtin.hpp"
#include "sq/engine/audio/devices.hpp"
#include "sq/engine/audio/nodes.hpp"
#include "sq/engine/doc/actions.hpp"
#include "sq/engine/audio/engine.hpp"
#include "sq/engine/audio/plugin_fx.hpp"
#include "sq/engine/doc/manager.hpp"
#include "sq/engine/edit/edit.hpp"
#endif
#if SQ_SHELL_DAWPROJECT
#include "sq/formats/dawproject/dawproject.hpp"
#endif

namespace sq::app {

namespace {
const char* kTag = "shell";

[[maybe_unused]] std::string str(IAttributes* a, const char* key, const std::string& def = {}) {
  if (!a || a->getType(key) != SQ_VALUE_STRING) return def;
  return sq::getAttrString(a, key);
}
[[maybe_unused]] double num(IAttributes* a, const char* key, double def) { return a && a->getType(key) != SQ_VALUE_NONE ? a->getDouble(key, def) : def; }

[[maybe_unused]] std::string nodeName(INode* n) {
  char buf[512];
  uint32_t need = 0;
  return n && n->getName(buf, sizeof buf, &need) == SQ_OK ? std::string(buf) : std::string();
}

[[maybe_unused]] std::string lowerExt(const std::string& path) {
  size_t slash = path.find_last_of("/\\");
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
  std::string e = path.substr(dot + 1);
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e;
}

[[maybe_unused]] std::string dirOf(const std::string& p) {
  size_t s = p.find_last_of("/\\");
  return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

// Observer adapter.
class [[maybe_unused]] FnObserver final : public sq::Object<FnObserver, IObserver> {
 public:
  explicit FnObserver(std::function<void(const char*, IAttributes*)> fn) : fn_(std::move(fn)) {}
  void SQ_CALL onNotify(IUnknown*, const char* topic, IAttributes* args) override {
    if (fn_ && topic) fn_(topic, args);
  }

 private:
  std::function<void(const char*, IAttributes*)> fn_;
};

// Inbound OSC: the service calls onOscMessage on its network thread; messages are queued (bounded)
// and executed as commands from Shell::idle on the main thread.
class OscInbox final : public sq::Object<OscInbox, IOscListener> {
 public:
  struct Msg {
    std::string address;
    bool hasValue = false;
    double value = 0;
    std::string text;
  };
  static constexpr size_t kMaxQueued = 256;

  void SQ_CALL onOscMessage(const char* address, const sq_osc_arg* args, uint32_t count, const char*, uint16_t) override {
    if (!address) return;
    try {
      Msg m;
      m.address = address;
      for (uint32_t i = 0; args && i < count && i < 8; ++i) {
        const sq_osc_arg& a = args[i];
        if (!m.hasValue && (a.type == 'i' || a.type == 'h')) m.value = static_cast<double>(a.i), m.hasValue = true;
        else if (!m.hasValue && (a.type == 'f' || a.type == 'd') && std::isfinite(a.d)) m.value = a.d, m.hasValue = true;
        else if (!m.hasValue && (a.type == 'T' || a.type == 'F')) m.value = a.type == 'T' ? 1.0 : 0.0, m.hasValue = true;
        else if (a.type == 's' && a.s && m.text.empty()) m.text.assign(a.s, ::strnlen(a.s, 1024));
      }
      std::lock_guard<std::mutex> lock(mu_);
      if (queue_.size() >= kMaxQueued) {
        ++dropped_;
        return;
      }
      queue_.push_back(std::move(m));
    } catch (...) {
      // bad_alloc on the network thread: drop the message
    }
  }
  std::deque<Msg> take() {
    std::lock_guard<std::mutex> lock(mu_);
    std::deque<Msg> out;
    out.swap(queue_);
    return out;
  }

 private:
  std::mutex mu_;
  std::deque<Msg> queue_;
  uint64_t dropped_ = 0;
};

// Commands an OSC peer may trigger: playback/navigation/undo only. Anything that touches files,
// plugins, scripts or settings stays local (OSC is unauthenticated UDP).
bool oscCommandAllowed(const std::string& category, const std::string& name) {
  if (category == "Transport" || category == "Navigate") return true;
  return category == "Edit" && (name == "Undo" || name == "Redo");
}

}  // namespace

// =================================================================================================
struct Shell::Impl {
  Shell& sh;
  explicit Impl(Shell& s) : sh(s) {}

#if SQ_SHELL_ENGINE
  edoc::DocumentManagerImpl* docs = nullptr;  // borrowed from the host service table
  sq::Ptr<engine::audio::AudioEngine> audio;
  sq::Ptr<IObserver> docObserver;
  bool nullDriver = false;
#endif
  sq::Ptr<IContentIndex> index;
  std::string indexPath;
  // UI services (theme:// resolver + remote pages), see startUi().
  sq::Ptr<ui::ThemeResolver> themes;
  std::unique_ptr<ui::UiServices> uiServices;
  uint32_t themeObserver = 0;
  // OSC command input (Osc.Listen).
  sq::Ptr<IOscService> osc;
  sq::Ptr<OscInbox> oscInbox;
  uint32_t oscCookie = 0;
  // Script command handlers instantiated on demand (released before modules unload).
  std::map<std::string, sq::Ptr<ICommandHandler>> handlerCache;  // classId -> handler
  int64_t lastTickMs = 0;

  using Fn = std::function<sq_result(IAttributes* args, IAttributes* result)>;
  struct Builtin {
    std::string category, name, title;
    Fn run;
    std::function<bool()> enabled;
  };
  std::vector<Builtin> builtins;

  // ---- helpers -------------------------------------------------------------------------------
  sq::Ptr<IDocument> active() const {
    sq::Ptr<IDocument> d;
#if SQ_SHELL_ENGINE
    if (docs) docs->getActiveDocument(d.put());
#endif
    return d;
  }
  sq::Ptr<IEditor> editor() const {
    sq::Ptr<IEditor> e;
    if (auto d = active()) d->getEditor(e.put());
    return e;
  }
  sq_result exec(const sq_guid& action, IAttributes* p, IAttributes* res = nullptr) {
    auto ed = editor();
    if (!ed) return SQ_E_NOTFOUND;
    return ed->execute(&action, p, res);
  }
  sq_result setMessage(sq_result r, const std::string& msg) {
    sh.message_ = msg;
    if (SQ_FAILED(r)) sq_log_printf(SQ_LOG_WARN, kTag, "%s", msg.c_str());
    return r;
  }
  static sq_guid nodeId(INode* n) {
    sq_guid g{};
    if (n) n->getId(&g);
    return g;
  }
  void bindEngine() {
#if SQ_SHELL_ENGINE
    if (audio) audio->setDocument(active().get());
    applyLoopFromDocument();
#endif
  }

  // ---- built-in commands ---------------------------------------------------------------------
  void add(const char* cat, const char* name, const char* title, Fn fn, std::function<bool()> en = {}) {
    builtins.push_back(Builtin{cat, name, title, std::move(fn), std::move(en)});
  }
  void registerBuiltins() {
    auto haveDoc = [this] { return static_cast<bool>(active()); };
    (void)haveDoc;  // unused when the engine units are filtered out
#if SQ_SHELL_ENGINE
    add("File", "New", "New Song", [this](IAttributes*, IAttributes* res) {
      if (!docs) return SQ_E_UNSUPPORTED;
      sq::Ptr<IDocument> d;
      sq_result r = docs->createDocument(SQ_DOC_SONG, d.put());
      if (SQ_FAILED(r)) return r;
      docs->setActiveDocument(d.get());
      bindEngine();
      if (res) {
        sq_guid g = nodeId(d.get());
        res->setGuid("document", &g);
      }
      return setMessage(SQ_OK, "new song");
    });
    add("File", "Open", "Open...", [this](IAttributes* a, IAttributes* res) {
      std::string path = str(a, "path");
      if (path.empty() || !docs) return SQ_E_INVALIDARG;
      sq::Ptr<IDocument> d;
      sq_result r = docs->openDocument(path.c_str(), d.put());
      if (SQ_FAILED(r)) return setMessage(r, "cannot open " + path + ": " + sq_result_string(r));
      docs->setActiveDocument(d.get());
      bindEngine();
      const std::vector<std::string> missing = missingMedia();
      if (res) res->setInt("missing", static_cast<int64_t>(missing.size()));
      if (!missing.empty()) sq_log_printf(SQ_LOG_WARN, kTag, "%s: %zu media file(s) missing", path.c_str(), missing.size());
      return setMessage(SQ_OK, "opened " + path + missingNote(missing, ""));
    });
    add("File", "Save", "Save", [this](IAttributes* a, IAttributes* res) {
      auto d = active();
      if (!d || !docs) return SQ_E_NOTFOUND;
      std::string path = str(a, "path");
      if (path.empty()) {
        char buf[4096];
        uint32_t need = 0;
        if (d->getPath(buf, sizeof buf, &need) == SQ_OK) path = buf;
      }
      if (path.empty()) return setMessage(SQ_E_INVALIDARG, "save failed: no path (path=<file>)");
      std::string note;
      const size_t moved = consolidateMedia(path, &note);
      if (res) res->setInt("media_copied", static_cast<int64_t>(moved));
      sq_result r = docs->saveDocument(d.get(), path.c_str());
      std::string msg = SQ_SUCCEEDED(r) ? std::string("saved") : std::string("save failed: ") + sq_result_string(r);
      if (moved) msg += " (" + std::to_string(moved) + " media file(s) copied to '" + mediaFolderFor(path) + "')";
      return setMessage(r, msg + note);
    }, haveDoc);
    add("File", "Close", "Close", [this](IAttributes*, IAttributes*) {
      auto d = active();
      if (!d || !docs) return SQ_E_NOTFOUND;
      if (audio) audio->setDocument(nullptr);
      sq_result r = docs->closeDocument(d.get());
      bindEngine();
      return r;
    }, haveDoc);
    add("File", "ExportAudio", "Export Mixdown...", [this](IAttributes* a, IAttributes* res) { return render(a, res, SQ_RENDER_MIXDOWN); }, haveDoc);
    add("File", "ExportStems", "Export Stems...", [this](IAttributes* a, IAttributes* res) { return render(a, res, SQ_RENDER_STEMS); }, haveDoc);
    add("File", "ExportMidi", "Export MIDI File...", [this](IAttributes* a, IAttributes* res) { return exportMidi(a, res); }, haveDoc);
    add("File", "ImportAudio", "Import Audio...", [this](IAttributes* a, IAttributes* res) { return importAudio(a, res); }, haveDoc);
    add("File", "ImportMidi", "Import MIDI File...", [this](IAttributes* a, IAttributes* res) { return importMidi(a, res); }, haveDoc);
#if SQ_SHELL_DAWPROJECT
    add("File", "ExportDawProject", "Export DAWproject...", [this](IAttributes* a, IAttributes*) {
      auto d = active();
      std::string path = str(a, "path");
      if (!d || path.empty()) return SQ_E_INVALIDARG;
      formats::dawproject::ExportReport rep;
      std::string err;
      sq_result r = formats::dawproject::exportProject(d.get(), path, &rep, &err);
      return setMessage(r, SQ_SUCCEEDED(r) ? "exported " + dawSummary(rep) + " to " + path : err);
    }, haveDoc);
    add("File", "ImportDawProject", "Import DAWproject...", [this](IAttributes* a, IAttributes* res) {
      std::string path = str(a, "path");
      if (path.empty() || !docs) return SQ_E_INVALIDARG;
      sq::Ptr<IDocument> d;
      sq_result r = docs->createDocument(SQ_DOC_SONG, d.put());
      if (SQ_FAILED(r)) return r;
      std::string media = str(a, "media", sq::joinPath(dirOf(path), "Imported Media"));
      formats::dawproject::ExportReport rep;
      std::string err;
      r = formats::dawproject::importProject(path, d.get(), media, &rep, &err);
      if (SQ_FAILED(r)) {
        docs->closeDocument(d.get());
        return setMessage(r, err);
      }
      docs->setActiveDocument(d.get());
      bindEngine();
      if (res) res->setInt("tracks", rep.tracks);
      return setMessage(SQ_OK, "imported " + dawSummary(rep) + " from " + path);
    });
#endif
    add("Transport", "Play", "Play", [this](IAttributes*, IAttributes*) { return audio ? audio->play() : SQ_E_UNSUPPORTED; });
    add("Transport", "Stop", "Stop", [this](IAttributes*, IAttributes*) {
      if (!audio) return SQ_E_UNSUPPORTED;
      sq_transport_state ts{};
      ts.struct_size = sizeof ts;
      audio->getTransportState(&ts);
      if (ts.flags & SQ_TRANSPORT_RECORDING) audio->record(SQ_FALSE);
      return audio->stop();
    });
    add("Transport", "Record", "Record", [this](IAttributes*, IAttributes*) {
      if (!audio) return SQ_E_UNSUPPORTED;
      sq_transport_state ts{};
      ts.struct_size = sizeof ts;
      audio->getTransportState(&ts);
      if (ts.flags & SQ_TRANSPORT_RECORDING) return audio->record(SQ_FALSE);
      if (!(ts.flags & SQ_TRANSPORT_PLAYING)) {
        sq_result r = audio->play();
        if (SQ_FAILED(r)) return r;
      }
      return audio->record(SQ_TRUE);
    }, haveDoc);
    add("Transport", "Rewind", "Go to Start", [this](IAttributes*, IAttributes*) { return audio ? audio->locate(0) : SQ_E_UNSUPPORTED; });
    add("Transport", "ToggleLoop", "Loop", [this](IAttributes* a, IAttributes*) {
      if (!audio) return SQ_E_UNSUPPORTED;
      sq_transport_state ts{};
      ts.struct_size = sizeof ts;
      audio->getTransportState(&ts);
      // The song's loop is the main-thread truth (the transport snapshot lags one audio block).
      double ls = ts.loop_start, le = ts.loop_end;
      bool on = (ts.flags & SQ_TRANSPORT_LOOPING) != 0;
      readLoop(activeImpl(), &ls, &le, &on);
      double s = num(a, "start", ls), e = num(a, "end", le > ls ? le : s + 16);
      return audio->setLoop(s, e, on ? SQ_FALSE : SQ_TRUE);
    });
    add("Transport", "ToggleMetronome", "Metronome", [this](IAttributes*, IAttributes*) {
      if (!audio) return SQ_E_UNSUPPORTED;
      sq_transport_state ts{};
      ts.struct_size = sizeof ts;
      audio->getTransportState(&ts);
      return audio->setMetronome((ts.flags & SQ_TRANSPORT_METRONOME) ? SQ_FALSE : SQ_TRUE);
    });
    add("Edit", "Undo", "Undo", [this](IAttributes*, IAttributes*) {
      auto ed = editor();
      return ed ? ed->undo() : SQ_E_NOTFOUND;
    }, [this] {
      auto ed = editor();
      return ed && ed->canUndo();
    });
    add("Edit", "Redo", "Redo", [this](IAttributes*, IAttributes*) {
      auto ed = editor();
      return ed ? ed->redo() : SQ_E_NOTFOUND;
    }, [this] {
      auto ed = editor();
      return ed && ed->canRedo();
    });
    add("Track", "AddAudio", "Add Audio Track", [this](IAttributes* a, IAttributes* res) { return addTrackKind(a, res, SQ_TRACK_AUDIO, "Audio", {}); }, haveDoc);
    add("Track", "AddInstrument", "Add Instrument Track", [this](IAttributes* a, IAttributes* res) {
      std::string spec = str(a, "instrument", engine::audio::kDefaultInstrument);
      if (spec.empty()) spec = engine::audio::kDefaultInstrument;  // instrument="" = the default
      if (!validInstrument(spec)) return setMessage(SQ_E_INVALIDARG, "malformed instrument spec (<format>|<id>|<path>[|i=v]...)");
      return addTrackKind(a, res, SQ_TRACK_INSTRUMENT, "Instrument", spec);
    }, haveDoc);
    add("Track", "ToggleArm", "Arm Track for Recording", [this](IAttributes* a, IAttributes*) {
      auto t = track(a);
      if (!t) return SQ_E_NOTFOUND;
      sq_track_info ti{};
      ti.struct_size = sizeof ti;
      t->getTrackInfo(&ti);
      return setTrackProp(t.get(), "armed", [&](IAttributes* p) { p->setBool("value", (ti.flags & SQ_TRACK_ARMED) ? SQ_FALSE : SQ_TRUE); });
    }, haveDoc);
    add("Track", "SetInput", "Set Track Input", [this](IAttributes* a, IAttributes*) {
      auto t = track(a);
      if (!t || !a) return SQ_E_INVALIDARG;
      int64_t ch = a->getInt("channel", 0);
      if (ch < 0 || ch > 255) return SQ_E_INVALIDARG;
      return setTrackProp(t.get(), "user.input_channel", [&](IAttributes* p) { p->setInt("value", ch); });
    }, haveDoc);
    add("Track", "InsertPlugin", "Insert Plugin...", [this](IAttributes* a, IAttributes*) { return insertPlugin(a); }, haveDoc);
    registerMixerCommands(haveDoc);
#endif
    add("Plugins", "Rescan", "Rescan Plugins", [this](IAttributes* a, IAttributes*) {
      auto reg = sh.plugins();
      if (!reg) return SQ_E_UNSUPPORTED;
      return reg->rescan(a && a->getBool("full", SQ_FALSE) ? SQ_TRUE : SQ_FALSE);
    });
    add("Browser", "IndexFolder", "Index Folder...", [this](IAttributes* a, IAttributes* res) { return indexFolder(a, res); });
    add("Browser", "Search", "Search Browser", [this](IAttributes* a, IAttributes* res) { return search(a, res); });
    add("Script", "Evaluate", "Run Script", [this](IAttributes* a, IAttributes* res) {
      auto se = sh.scripts();
      if (!se) return SQ_E_UNSUPPORTED;
      std::string src = str(a, "source");
      sq_result r = se->evaluate(src.c_str(), str(a, "name", "user-script").c_str(), res);
      if (SQ_FAILED(r)) {
        char buf[1024];
        uint32_t need = 0;
        se->getLastError(buf, sizeof buf, &need);
        return setMessage(r, buf);
      }
      return r;
    });
    add("Script", "LoadPackage", "Load Script Package...", [this](IAttributes* a, IAttributes*) {
      auto se = sh.scripts();
      if (!se) return SQ_E_UNSUPPORTED;
      sq::Ptr<IScriptPackage> pkg;
      sq_result r = se->loadPackage(str(a, "path").c_str(), pkg.put());
      if (SQ_FAILED(r)) {
        char buf[1024];
        uint32_t need = 0;
        se->getLastError(buf, sizeof buf, &need);
        return setMessage(r, std::string("package: ") + buf);
      }
      handlerCache.clear();
      return setMessage(r, "package loaded");
    });
    registerConnectivity();
  }

  // ---- devices, themes, surfaces, OSC, sync, account, video (optional services) ---------------
  template <class T>
  sq::Ptr<T> service() {
    sq::Ptr<T> p;
    sh.host.queryService(&T::iid, p.putVoid());
    return p;
  }
  static std::string fixed(const char* s, size_t cap) { return std::string(s, ::strnlen(s, cap)); }
  static std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  }

  void registerConnectivity() {
    add("Audio", "ListDevices", "List Audio Devices", [this](IAttributes* a, IAttributes* res) { return listAudioDevices(a, res); });
#if SQ_SHELL_ENGINE
    add("Audio", "SelectDevice", "Select Audio Device...", [this](IAttributes* a, IAttributes*) {
      std::string drv = lower(str(a, "driver"));
      static const std::set<std::string> known = {"none", "null", "offline", "wasapi", "asio", "alsa", "pulse"};
      if (!known.count(drv)) return setMessage(SQ_E_INVALIDARG, "driver must be none|null|offline|wasapi|asio|alsa|pulse");
      std::string dev = str(a, "device");
      const std::string oldDrv = sh.settings.get("Devices.Audio.Driver"), oldDev = sh.settings.get("Devices.Audio.Device");
      sh.settings.setString("Devices.Audio.Driver", drv.c_str());
      sh.settings.setString("Devices.Audio.Device", dev.c_str());
      if (drv == "none") {
        if (audio) audio->closeDevice();
        return setMessage(SQ_OK, "audio device closed");
      }
      sq_result r = sh.openAudioDevice(drv);
      if (SQ_FAILED(r)) {  // keep the previous choice persisted
        std::string msg = sh.message_;
        sh.settings.setString("Devices.Audio.Driver", oldDrv.c_str());
        sh.settings.setString("Devices.Audio.Device", oldDev.c_str());
        return setMessage(r, msg);
      }
      return r;
    });
#endif
    add("UI", "SetTheme", "Set Theme", [this](IAttributes* a, IAttributes*) {
      std::string name = str(a, "name");
      if (!themes) return setMessage(SQ_E_UNSUPPORTED, "no themes loaded");
      if (!themes->hasTheme(name)) return setMessage(SQ_E_NOTFOUND, "unknown theme '" + name + "'");
      sh.settings.setString("UI.Theme", name.c_str());  // the UI.Theme observer applies it
      return setMessage(themes->activeTheme() == name ? SQ_OK : SQ_E_FAIL, "theme: " + themes->activeTheme());
    });
    add("Surface", "List", "List Control Surfaces", [this](IAttributes*, IAttributes* res) {
      auto svc = service<IControlSurfaceService>();
      if (!svc) return setMessage(SQ_E_UNSUPPORTED, "no control-surface service (hardware-surface not loaded)");
      std::string lines;
      uint32_t n = svc->getSurfaceCount();
      for (uint32_t i = 0; i < n; ++i) {
        sq_surface_info si{};
        si.struct_size = sizeof si;
        if (svc->getSurfaceInfo(i, &si) != SQ_OK) continue;
        lines += fixed(si.id, sizeof si.id) + "\t" + fixed(si.name, sizeof si.name) + "\t" +
                 ((si.flags & SQ_SURFACE_CONNECTED) ? "connected" : "offline") + "\n";
      }
      if (res) {
        res->setInt("count", n);
        res->setString("surfaces", lines.c_str());
      }
      return setMessage(SQ_OK, std::to_string(n) + " control surface(s)");
    });
    add("Surface", "LoadMap", "Load Control Surface Map...", [this](IAttributes* a, IAttributes* res) {
      auto svc = service<IControlSurfaceService>();
      if (!svc) return setMessage(SQ_E_UNSUPPORTED, "no control-surface service (hardware-surface not loaded)");
      std::string path = str(a, "path"), conn = str(a, "connection");
      if (path.empty()) return SQ_E_INVALIDARG;
      char id[128] = {};
      uint32_t need = 0;
      sq_result r = svc->loadSurfaceMap(path.c_str(), conn.empty() ? nullptr : conn.c_str(), id, sizeof id, &need);
      if (SQ_FAILED(r)) {
        std::string why;
        if (auto pump = svc.as<ISurfacePump>()) {
          char buf[2048];
          uint32_t n2 = 0;
          if (pump->getLastError(buf, sizeof buf, &n2) == SQ_OK) why = buf;
        }
        return setMessage(r, "surface map " + path + ": " + (why.empty() ? std::string(sq_result_string(r)) : why));
      }
      if (res) res->setString("surface", id);
      return setMessage(r, std::string("surface ") + id + " loaded");
    });
    add("Surface", "LoadRemotePages", "Load Remote Pages", [this](IAttributes* a, IAttributes* res) {
      auto svc = service<IControlSurfaceService>();
      auto provider = service<IRemotePageProvider>();
      if (!svc || !provider) return setMessage(SQ_E_UNSUPPORTED, "no control-surface service or remote-page provider");
      std::string sid = str(a, "surface"), skin = str(a, "skin", "theme://current/remote/mixer.skin.xml"),
                  ctx = str(a, "context", "object://doc/active");
      if (sid.empty()) return SQ_E_INVALIDARG;
      sq::Ptr<IRemotePageSet> pages;
      sq_result r = provider->loadRemotePages(skin.c_str(), ctx.c_str(), pages.put());
      if (SQ_FAILED(r) || !pages) return setMessage(SQ_FAILED(r) ? r : SQ_E_FAIL, "remote pages " + skin + ": " + sq_result_string(r));
      r = svc->setRemotePages(sid.c_str(), pages.get());
      if (res && SQ_SUCCEEDED(r)) res->setInt("pages", pages->getPageCount());
      return setMessage(r, SQ_SUCCEEDED(r) ? std::to_string(pages->getPageCount()) + " remote page(s) on " + sid
                                           : "set remote pages: " + std::string(sq_result_string(r)));
    });
    add("Surface", "SelectPage", "Select Remote Page", [this](IAttributes* a, IAttributes*) {
      auto svc = service<IControlSurfaceService>();
      if (!svc) return setMessage(SQ_E_UNSUPPORTED, "no control-surface service (hardware-surface not loaded)");
      std::string sid = str(a, "surface");
      int64_t page = a ? a->getInt("page", -1) : -1;
      if (sid.empty() || page < 0 || page > 0xFFFF) return SQ_E_INVALIDARG;
      return svc->selectPage(sid.c_str(), static_cast<uint32_t>(page));
    });
    add("Osc", "Listen", "Listen for OSC Commands", [this](IAttributes* a, IAttributes* res) {
      auto svc = service<IOscService>();
      if (!svc) return setMessage(SQ_E_UNSUPPORTED, "no OSC service (hardware-remote not loaded)");
      int64_t port = a && a->getType("port") != SQ_VALUE_NONE ? a->getInt("port", 9000) : 9000;
      if (port < 0 || port > 65535) return SQ_E_INVALIDARG;
      stopOsc();
      auto inbox = sq::Ptr<OscInbox>::adopt(new OscInbox());
      uint16_t bound = 0;
      uint32_t cookie = 0;
      sq_result r = svc->listen(static_cast<uint16_t>(port), inbox.get(), &bound, &cookie);
      if (SQ_FAILED(r)) return setMessage(r, "OSC listen on port " + std::to_string(port) + ": " + sq_result_string(r));
      osc = svc;
      oscInbox = inbox;
      oscCookie = cookie;
      if (res) res->setInt("port", bound);
      return setMessage(SQ_OK, "OSC: listening on port " + std::to_string(bound) + " (/sq/<Category>/<Name>)");
    });
    add("Osc", "Stop", "Stop OSC Commands", [this](IAttributes*, IAttributes*) {
      if (!osc) return SQ_S_FALSE;
      stopOsc();
      return setMessage(SQ_OK, "OSC: stopped");
    });
    add("Osc", "Send", "Send OSC Message", [this](IAttributes* a, IAttributes*) {
      auto svc = service<IOscService>();
      if (!svc) return setMessage(SQ_E_UNSUPPORTED, "no OSC service (hardware-remote not loaded)");
      std::string host = str(a, "host", "127.0.0.1"), address = str(a, "address"), text;
      int64_t port = a ? a->getInt("port", -1) : -1;
      if (address.empty() || address[0] != '/' || port <= 0 || port > 65535) return SQ_E_INVALIDARG;
      sq_osc_arg arg{};
      uint32_t count = 0;
      if (a && a->getType("value") == SQ_VALUE_STRING) {
        text = str(a, "value");
        arg.type = 's';
        arg.s = text.c_str();
        count = 1;
      } else if (a && a->getType("value") != SQ_VALUE_NONE) {
        arg.type = 'f';
        arg.d = a->getDouble("value", 0);
        count = 1;
      }
      return svc->send(host.c_str(), static_cast<uint16_t>(port), address.c_str(), count ? &arg : nullptr, count);
    });
    add("Sync", "Link", "Network Tempo Sync", [this](IAttributes* a, IAttributes* res) {
      auto ts = service<ITempoSync>();
      if (!ts) return setMessage(SQ_E_UNSUPPORTED, "no tempo-sync service (link-ableton not loaded)");
      bool on = a && a->getType("enabled") != SQ_VALUE_NONE ? a->getBool("enabled", SQ_TRUE) != SQ_FALSE : !ts->isEnabled();
      sq_result r = ts->enable(on ? SQ_TRUE : SQ_FALSE);
      if (SQ_FAILED(r)) return setMessage(r, std::string("tempo sync: ") + sq_result_string(r));
      sq_tempo_sync_state st{};
      st.struct_size = sizeof st;
      if (ts->captureState(sq_time_monotonic_ns(), &st) == SQ_OK && res) {
        res->setDouble("bpm", st.bpm);
        res->setInt("peers", st.peers);
      }
      if (res) res->setBool("enabled", ts->isEnabled());
      return setMessage(SQ_OK, std::string("tempo sync ") + (ts->isEnabled() ? "on" : "off"));
    });
    add("Account", "Status", "Account Status", [this](IAttributes*, IAttributes* res) {
      auto acc = service<ICloudAccount>();
      if (!acc) return setMessage(SQ_E_UNSUPPORTED, "no account service (cloud-client not loaded); working offline");
      sq_account_state st{};
      st.struct_size = sizeof st;
      sq_result r = acc->getAccountState(&st);
      if (SQ_FAILED(r)) return r;
      static const char* const names[] = {"signed out", "signed in", "offline", "expired"};
      const char* s = st.state >= 0 && st.state <= 3 ? names[st.state] : "unknown";
      if (res) {
        res->setInt("state", st.state);
        res->setString("name", fixed(st.display_name, sizeof st.display_name).c_str());
      }
      return setMessage(SQ_OK, std::string("account: ") + s);
    });
    add("Account", "SignIn", "Sign In...", [this](IAttributes*, IAttributes*) {
      auto acc = service<ICloudAccount>();
      return acc ? acc->beginSignIn() : setMessage(SQ_E_UNSUPPORTED, "no account service (cloud-client not loaded)");
    });
    add("Account", "SignOut", "Sign Out", [this](IAttributes*, IAttributes*) {
      auto acc = service<ICloudAccount>();
      return acc ? acc->signOut() : setMessage(SQ_E_UNSUPPORTED, "no account service (cloud-client not loaded)");
    });
    add("Video", "Probe", "Probe Video File", [this](IAttributes* a, IAttributes* res) {
      auto ve = service<IVideoEngine>();
      if (!ve) return setMessage(SQ_E_UNSUPPORTED, "no video engine (video-engine not loaded)");
      std::string path = str(a, "path");
      if (path.empty()) return SQ_E_INVALIDARG;
      sq::Ptr<IVideoReader> rd;
      sq_result r = ve->openFile(path.c_str(), nullptr, rd.put());
      if (SQ_FAILED(r) || !rd) return setMessage(SQ_FAILED(r) ? r : SQ_E_FAIL, "video " + path + ": " + sq_result_string(r));
      sq_video_info vi{};
      vi.struct_size = sizeof vi;
      r = rd->getInfo(&vi);
      if (SQ_FAILED(r)) return r;
      if (res) {
        res->setInt("width", vi.width);
        res->setInt("height", vi.height);
        res->setInt("duration_ns", vi.duration_ns);
        res->setString("backend", fixed(vi.backend, sizeof vi.backend).c_str());
      }
      return setMessage(SQ_OK, std::to_string(vi.width) + "x" + std::to_string(vi.height) + " " + fixed(vi.codec, sizeof vi.codec));
    });
  }

  sq_result listAudioDevices(IAttributes* a, IAttributes* res) {
    const std::string want = lower(str(a, "driver"));
    std::string lines, notes;
    int64_t count = 0;
    for (uint32_t i = 0; i < sh.registry.getClassCount(); ++i) {
      sq_class_info ci{};
      ci.struct_size = sizeof ci;
      if (sh.registry.getClassInfo(i, &ci) != SQ_OK || !ci.category || std::strcmp(ci.category, audiodevice::kCategory) != 0) continue;
      auto drv = sq::createInstance<IAudioDriver>(&sh.registry, ci.class_id);
      if (!drv) continue;
      sq_audio_driver_info di{};
      di.struct_size = sizeof di;
      if (drv->getDriverInfo(&di) != SQ_OK) continue;
      const std::string dname = fixed(di.name, sizeof di.name);
      if (!want.empty() && lower(dname) != want) continue;
      drv->refreshDevices();
      for (uint32_t k = 0; k < drv->getDeviceCount(); ++k) {
        sq_audio_device_info dev{};
        dev.struct_size = sizeof dev;
        if (drv->getDeviceInfo(k, &dev) != SQ_OK) continue;
        lines += dname + "\t" + fixed(dev.id, sizeof dev.id) + "\t" + fixed(dev.name, sizeof dev.name) + "\n";
        ++count;
      }
      if (auto st = drv.as<IAsioDriverStatus>()) {
        char buf[1024];
        uint32_t need = 0;
        if (st->getStatusText(buf, sizeof buf, &need) == SQ_OK) notes += (notes.empty() ? "" : "; ") + dname + ": " + buf;
      }
    }
    if (res) {
      res->setInt("count", count);
      res->setString("devices", lines.c_str());
      res->setString("notes", notes.c_str());
    }
    return setMessage(SQ_OK, std::to_string(count) + " audio device(s)" + (notes.empty() ? std::string() : " | " + notes));
  }

  void stopOsc() {
    if (osc && oscCookie) osc->stopListening(oscCookie);
    osc = nullptr;
    oscCookie = 0;
    oscInbox = nullptr;
  }
  // Main thread: run queued "/sq/<Category>/<Name>" messages.
  void drainOsc() {
    if (!oscInbox) return;
    for (auto& m : oscInbox->take()) {
      static const std::string pre = "/sq/";
      if (m.address.rfind(pre, 0) != 0) continue;
      std::string rest = m.address.substr(pre.size());
      size_t s = rest.find('/');
      if (s == std::string::npos) continue;
      std::string cat = rest.substr(0, s), nm = rest.substr(s + 1);
      if (nm.empty() || nm.find('/') != std::string::npos || !oscCommandAllowed(cat, nm)) {
        sq_log_printf(SQ_LOG_DEBUG, kTag, "OSC: ignored %s", m.address.c_str());
        continue;
      }
      auto args = sq::makeAttributes();
      if (m.hasValue) args->setDouble("value", m.value);
      if (!m.text.empty()) args->setString("text", m.text.c_str());
      sq_result r = sh.execute(cat, nm, args.get(), nullptr);
      sq_log_printf(SQ_FAILED(r) ? SQ_LOG_WARN : SQ_LOG_DEBUG, kTag, "OSC %s: %s", m.address.c_str(), sq_result_string(r));
    }
  }
  void pumpSurfaces() {
    if (auto svc = service<IControlSurfaceService>())
      if (auto pump = svc.as<ISurfacePump>()) pump->pump();
  }

  // Stock themes: the pack embedded in the EXE (sq-skinc RCDATA), else <exe>/themes/sqthemes.sqtheme,
  // else the theme folders in <exe>/themes. UI.Theme selects the active one (observed).
  void startUi() {
    themes = ui::ThemeResolver::create();
    std::string err;
    bool loaded = false;
    if (auto pack = ui::ThemePack::fromResource(ui::kThemePackResourceName, nullptr, &err)) loaded = themes->addPack(pack, &err);
    const std::string dir = sq::joinPath(sq::specialDir(SQ_DIR_EXE), "themes");
    const std::string packFile = sq::joinPath(dir, "sqthemes.sqtheme");
    if (!loaded && sq_fs_exists(packFile.c_str()))
      if (auto pack = ui::ThemePack::fromFile(packFile, &err)) loaded = themes->addPack(pack, &err);
    if (!loaded && sq_fs_exists(dir.c_str())) loaded = themes->addDirectory(dir, &err);
    if (!loaded) sq_log_printf(SQ_LOG_WARN, kTag, "no stock themes (%s); built-in skin colors only", err.c_str());
    applyTheme();
    themeObserver = sh.settings.observe("UI.Theme", [this](const char*) { applyTheme(); });
    uiServices = std::make_unique<ui::UiServices>(&sh.host, themes);
  }
  void applyTheme() {
    if (!themes) return;
    const std::string want = sh.settings.get("UI.Theme", "dark");
    if (themes->hasTheme(want)) themes->setActiveTheme(want.c_str());
    else if (themes->activeTheme().empty() && themes->hasTheme("dark")) themes->setActiveTheme("dark");
  }
  void stopUi() {
    if (themeObserver) sh.settings.removeObserver(themeObserver);
    themeObserver = 0;
    uiServices.reset();
    themes = nullptr;
  }

#if SQ_SHELL_ENGINE
#if SQ_SHELL_DAWPROJECT
  static std::string dawSummary(const formats::dawproject::ExportReport& rep) {
    size_t warnings = 0;
    for (char ch : rep.warnings) warnings += ch == '\n' ? 1u : 0u;
    if (!rep.warnings.empty()) sq_log_printf(SQ_LOG_WARN, kTag, "DAWproject:\n%s", rep.warnings.c_str());
    return std::to_string(rep.tracks) + " track(s)" + (rep.master ? " + master" : "") + ", " + std::to_string(rep.buses) + " bus(es), " +
           std::to_string(rep.devices) + " device(s), " + std::to_string(rep.sends) + " send(s), " + std::to_string(rep.automation) +
           " automation lane(s), " + std::to_string(rep.clips) + " clip(s)" + (warnings ? " (" + std::to_string(warnings) + " warning(s), see log)" : "");
  }
#endif
  // ---- song persistence: loop, media ------------------------------------------------------------
  // The transport loop is saved in the song as document user properties (not undo steps).
  static constexpr const char* kLoopStartKey = "user.loop.start";
  static constexpr const char* kLoopEndKey = "user.loop.end";
  static constexpr const char* kLoopEnabledKey = "user.loop.enabled";
  bool applyingLoop = false;  // document -> transport in progress: do not write it back
  sq::Ptr<IObserver> engineObserver;

  static bool readLoop(const edoc::DocumentImpl* impl, double* start, double* end, bool* enabled) {
    if (!impl) return false;
    const edoc::PropertyBag& u = impl->store().props.user;
    auto s = u.find(kLoopStartKey), e = u.find(kLoopEndKey), on = u.find(kLoopEnabledKey);
    if (s == u.end() || e == u.end() || s->second.type != SQ_VALUE_DOUBLE || e->second.type != SQ_VALUE_DOUBLE) return false;
    if (!std::isfinite(s->second.d) || !std::isfinite(e->second.d) || s->second.d < 0) return false;
    *start = s->second.d;
    *end = e->second.d;
    *enabled = on != u.end() && on->second.type == SQ_VALUE_BOOL && on->second.i != 0 && e->second.d > s->second.d;
    return true;
  }
  // Document -> transport (open, new, activation). A song without a saved loop has looping off.
  void applyLoopFromDocument() {
    if (!audio) return;
    edoc::DocumentImpl* impl = activeImpl();
    if (!impl) return;
    double s = 0, e = 0;
    bool on = false;
    applyingLoop = true;
    if (readLoop(impl, &s, &e, &on)) audio->setLoop(s, e, on ? SQ_TRUE : SQ_FALSE);
    else audio->setLoop(0.0, 0.0, SQ_FALSE);
    applyingLoop = false;
  }
  // Transport -> document: every loop change (commands, ruler drags, scripts) ends up in the song.
  void onLoopChanged(IAttributes* args) {
    if (applyingLoop || !args) return;
    edoc::DocumentImpl* impl = activeImpl();
    if (!impl) return;
    const double s = args->getDouble("start", 0.0), e = args->getDouble("end", 0.0);
    if (!std::isfinite(s) || !std::isfinite(e)) return;
    edoc::Value v;
    v.type = SQ_VALUE_DOUBLE;
    v.d = s;
    impl->setUntrackedDocProperty(kLoopStartKey, v);
    v.d = e;
    impl->setUntrackedDocProperty(kLoopEndKey, v);
    edoc::Value b;
    b.type = SQ_VALUE_BOOL;
    b.i = args->getBool("enabled", SQ_FALSE) ? 1 : 0;
    impl->setUntrackedDocProperty(kLoopEnabledKey, b);
  }

  // Audio clip sources of the active song whose file does not exist (sorted, unique).
  std::vector<std::string> missingMedia() const {
    std::vector<std::string> out;
    const edoc::DocumentImpl* impl = activeImpl();
    if (!impl) return out;
    std::set<std::string> seen;
    for (const auto& [id, c] : impl->store().clips)
      if (c.kind == SQ_CLIP_AUDIO && !c.source.empty() && seen.insert(c.source).second && !sq_fs_is_file(c.source.c_str()))
        out.push_back(c.source);
    std::sort(out.begin(), out.end());
    return out;
  }
  static std::string fileNameOf(const std::string& p) {
    const size_t sl = p.find_last_of("/\\");
    return sl == std::string::npos ? p : p.substr(sl + 1);
  }
  static std::string missingNote(const std::vector<std::string>& missing, const char* consequence) {
    if (missing.empty()) return {};
    std::string out = " - WARNING: " + std::to_string(missing.size()) + " missing media file(s)" + consequence + ": ";
    for (size_t i = 0; i < missing.size() && i < 8; ++i) out += (i ? ", " : "") + fileNameOf(missing[i]);
    if (missing.size() > 8) out += ", ...";
    return out;
  }

  static std::string absolutePath(const std::string& p) {
    sq::Buffer b;
    std::string out = sq_fs_absolute(p.c_str(), b.out()) == SQ_OK ? b.str() : p;
    for (char& ch : out)
      if (ch == '\\') ch = '/';
    return out;
  }
  // "<song folder>/<song name> Media": where takes of an untitled song move to on the first save.
  static std::string mediaFolderFor(const std::string& songPath) {
    std::string name = fileNameOf(songPath);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
    if (name.empty()) name = "Song";
    return sq::joinPath(dirOf(absolutePath(songPath)), name + " Media");
  }
  // Media recorded or bounced while the song had no file lives in the temp media folder: copy it
  // into "<song name> Media" next to the song and point the clips at the copies (one untracked
  // step, not an undo step; the temp files stay for other songs that may still use them).
  // Returns the number of files copied; `note` gets a warning for files that could not be copied.
  size_t consolidateMedia(const std::string& songPath, std::string* note) {
    edoc::DocumentImpl* impl = activeImpl();
    if (!impl) return 0;
    std::string temp = absolutePath(sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "SquizzyStudio"), "Media"));
    if (temp.empty()) return 0;
    if (temp.back() != '/') temp += '/';
    const std::string dest = mediaFolderFor(songPath);
    std::map<std::string, std::string> relink;
    std::vector<std::string> failed;
    std::set<std::string> used;
    for (const auto& [id, c] : impl->store().clips) {
      if (c.kind != SQ_CLIP_AUDIO || c.source.empty() || relink.count(c.source)) continue;
      const std::string abs = absolutePath(c.source);
      if (abs.compare(0, temp.size(), temp) != 0 || !sq_fs_is_file(abs.c_str())) continue;
      sq_fs_create_dirs(dest.c_str());
      const std::string name = fileNameOf(abs);
      const size_t dot = name.find_last_of('.');
      const std::string stem = dot == std::string::npos ? name : name.substr(0, dot), ext = dot == std::string::npos ? "" : name.substr(dot);
      std::string target = sq::joinPath(dest, name);
      for (int k = 2; sq_fs_exists(target.c_str()) || used.count(target); ++k) target = sq::joinPath(dest, stem + "-" + std::to_string(k) + ext);
      if (SQ_FAILED(sq_fs_copy_file(abs.c_str(), target.c_str(), SQ_FALSE))) {
        failed.push_back(name);
        continue;
      }
      used.insert(target);
      relink[c.source] = target;
    }
    if (!failed.empty() && note) {
      *note = " - WARNING: could not copy " + std::to_string(failed.size()) + " media file(s) next to the song:";
      for (const std::string& f : failed) *note += " " + f;
    }
    if (!relink.empty()) impl->relinkClipSources(relink);
    return relink.size();
  }

  template <class F>
  sq_result setTrackProp(ITrack* t, const char* key, F&& setter, bool merge = false) {
    auto p = sq::makeAttributes();
    sq_guid g = nodeId(t);
    p->setGuid("node", &g);
    p->setString("key", key);
    setter(p.get());
    if (merge) p->setBool("merge", SQ_TRUE);
    return exec(sq::doc::kSetProperty, p.get());
  }

  // Mixdown / stems. tail=<seconds> renders that much past the end (tail=0: hard cut at the end);
  // without tail= the export renders until the output has decayed below -90 dBFS (at most
  // kAutoTailMaxSeconds): a probe render with the maximum tail into a temp WAV measures it.
  static constexpr double kAutoTailMaxSeconds = 10.0;
  static constexpr double kTailSilence = 3.1622776601683795e-5;  // -90 dBFS

  sq_result runRender(const sq_render_request& rq, std::vector<std::string>* outputs) {
    sq::Ptr<IRenderTask> task;
    sq_result r = audio->startRender(&rq, nullptr, task.put());
    if (SQ_FAILED(r)) return r;
    r = task->wait(600000);
    audio->idle();  // main thread: release the finished render's graph (plugin instances) now
    if (SQ_SUCCEEDED(r)) r = task->getResult();
    if (SQ_SUCCEEDED(r) && outputs) {
      for (uint32_t i = 0; i < task->getOutputCount(); ++i) {
        char buf[4096];
        uint32_t need = 0;
        if (task->getOutputPath(i, buf, sizeof buf, &need) == SQ_OK) outputs->push_back(buf);
      }
    }
    return r;
  }
  // Frames at the end of the WAV `path` (whose last `tailSeconds` are the rendered tail) that are
  // still above -90 dBFS after the end of the range; -1 if the file cannot be read.
  int64_t audibleTailFrames(const std::string& path, double tailSeconds, double* rate) {
    auto codecSvc = sq::Ptr<ICodecService>::adopt(codec::findCodec(&sh.registry, "wav", nullptr, SQ_CODEC_READ));
    if (!codecSvc) return -1;
    sq::Ptr<IAudioFileReader> reader;
    if (SQ_FAILED(codecSvc->createReader(reader.put())) || !reader || SQ_FAILED(reader->open(path.c_str()))) return -1;
    sq_audio_file_format fmt{};
    fmt.struct_size = sizeof fmt;
    if (SQ_FAILED(reader->getFormat(&fmt)) || fmt.channels == 0 || fmt.channels > 64 || !(fmt.sample_rate > 0)) return -1;
    constexpr uint32_t kChunk = 8192;
    std::vector<float> buf(static_cast<size_t>(kChunk) * fmt.channels);
    std::vector<float*> ptrs(fmt.channels);
    for (uint32_t c = 0; c < fmt.channels; ++c) ptrs[c] = &buf[static_cast<size_t>(c) * kChunk];
    int64_t pos = 0, lastLoud = -1;
    for (;;) {
      sq_audio_block blk{ptrs.data(), fmt.channels, kChunk, pos};
      uint32_t got = 0;
      if (SQ_FAILED(reader->read(&blk, &got)) || got == 0) break;
      for (uint32_t c = 0; c < fmt.channels; ++c)
        for (uint32_t f = 0; f < got; ++f)
          if (std::fabs(static_cast<double>(ptrs[c][f])) > kTailSilence) lastLoud = std::max<int64_t>(lastLoud, pos + f);
      pos += got;
      if (got < kChunk) break;
    }
    *rate = fmt.sample_rate;
    const int64_t body = pos - static_cast<int64_t>(std::llround(tailSeconds * fmt.sample_rate));
    return std::max<int64_t>(0, lastLoud + 1 - body);
  }
  // Tail (seconds) the request needs until its output(s) decayed below -90 dBFS.
  sq_result measureAutoTail(const sq_render_request& base, double* tail) {
    *tail = 0;
    const std::string probeDir = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "SquizzyStudio");
    sq_fs_create_dirs(probeDir.c_str());  // mixdown renders do not create their output folder
    const std::string probe = sq::joinPath(probeDir, "export-probe-" + std::to_string(sq_process_id()));
    const bool stems = base.kind == SQ_RENDER_STEMS;
    const std::string probeOut = stems ? probe : probe + ".wav";
    sq_render_request rq = base;
    rq.flags |= SQ_RENDER_TAIL;
    rq.tail_seconds = kAutoTailMaxSeconds;
    rq.output = probeOut.c_str();
    rq.file_extension = "wav";
    rq.codec_options = nullptr;
    std::vector<std::string> outputs;
    sq_result r = runRender(rq, &outputs);
    if (SQ_SUCCEEDED(r)) {
      for (const std::string& f : outputs) {
        double rate = 0;
        const int64_t frames = audibleTailFrames(f, kAutoTailMaxSeconds, &rate);
        if (frames < 0) {
          r = SQ_E_IO;
          break;
        }
        *tail = std::max(*tail, std::ceil(static_cast<double>(frames) / rate * 1000.0) / 1000.0);  // whole ms
      }
    }
    for (const std::string& f : outputs) sq_fs_remove(f.c_str());
    if (stems) sq_fs_remove_all(probe.c_str());
    *tail = std::min(*tail, kAutoTailMaxSeconds);
    return r;
  }

  sq_result render(IAttributes* a, IAttributes* res, int32_t kind) {
    auto d = active();
    if (!d || !audio) return SQ_E_UNSUPPORTED;
    std::string out = str(a, kind == SQ_RENDER_STEMS ? "dir" : "path");
    if (out.empty()) return SQ_E_INVALIDARG;
    sq_document_info di{};
    di.struct_size = sizeof di;
    d->getDocumentInfo(&di);
    std::string ext = kind == SQ_RENDER_STEMS ? str(a, "ext", "wav") : lowerExt(out);
    sq_render_request rq{};
    rq.struct_size = sizeof rq;
    rq.kind = kind;
    rq.start = num(a, "start", 0.0);
    rq.end = num(a, "end", di.length);
    if (!(rq.end > rq.start)) return setMessage(SQ_E_INVALIDARG, "nothing to export (empty range)");
    const bool autoTail = !a || a->getType("tail") == SQ_VALUE_NONE;
    if (autoTail) {
      const sq_result pr = measureAutoTail(rq, &rq.tail_seconds);
      if (SQ_FAILED(pr)) return setMessage(pr, std::string("export failed (tail probe): ") + sq_result_string(pr));
    } else {
      rq.tail_seconds = num(a, "tail", 0.0);
      if (!(rq.tail_seconds >= 0.0 && rq.tail_seconds <= 3600.0)) return setMessage(SQ_E_INVALIDARG, "tail=<0..3600 seconds>");
    }
    if (rq.tail_seconds > 0) rq.flags |= SQ_RENDER_TAIL;
    rq.output = out.c_str();
    rq.file_extension = ext.c_str();
    std::vector<std::string> outputs;
    const sq_result r = runRender(rq, &outputs);
    if (SQ_FAILED(r)) return setMessage(r, std::string("export failed: ") + sq_result_string(r));
    const std::vector<std::string> missing = missingMedia();
    if (res) {
      res->setInt("outputs", static_cast<int64_t>(outputs.size()));
      res->setDouble("tail", rq.tail_seconds);
      res->setInt("missing", static_cast<int64_t>(missing.size()));
    }
    return setMessage(r, "exported " + out + " (tail " + fmtNum(rq.tail_seconds) + " s" + (autoTail ? ", auto" : "") + ")" +
                             missingNote(missing, " rendered as silence"));
  }

  static std::string fileNameOf(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
  }
  static std::string stemOf(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
  }

  // File.ImportAudio path=<audio> [track=<guid>|track_index=<i>] [start=ppq] [length=ppq]
  //                [source_offset=seconds] [name=<clip name>] [copy=true]
  // One undo step. References the file by default; copy=true puts it next to a saved song
  // (<song>/Media/<name>) or in Media.Folder. Media at any sample rate is resampled by the graph.
  sq_result importAudio(IAttributes* a, IAttributes* res) {
    auto d = active();
    const std::string path = str(a, "path");
    if (!d) return SQ_E_NOTFOUND;
    if (path.empty()) return setMessage(SQ_E_INVALIDARG, "import audio: path is required");
    if (!sq_fs_is_file(path.c_str())) return setMessage(SQ_E_NOTFOUND, "import audio: no such file: " + path);
    const std::string ext = lowerExt(path);
    if (ext.empty()) return setMessage(SQ_E_INVALIDARG, "import audio: file has no extension");
    auto codec = sq::Ptr<ICodecService>::adopt(sq::codec::findCodec(&sh.registry, ext.c_str(), nullptr, SQ_CODEC_READ));
    if (!codec) return setMessage(SQ_E_UNSUPPORTED, "import audio: no codec for ." + ext);
    sq::Ptr<IAudioFileReader> reader;
    sq_result r = codec->createReader(reader.put());
    if (SQ_FAILED(r) || !reader) return setMessage(r, "import audio: cannot create reader");
    r = reader->open(path.c_str());
    if (SQ_FAILED(r)) return setMessage(r, "import audio: cannot open " + path);
    sq_audio_file_format fmt{};
    fmt.struct_size = sizeof fmt;
    r = reader->getFormat(&fmt);
    if (SQ_FAILED(r)) return setMessage(r, "import audio: cannot read format");
    if (!(fmt.sample_rate > 0) || fmt.channels == 0) return setMessage(SQ_E_UNSUPPORTED, "import audio: invalid format");
    if (fmt.frame_count <= 0) return setMessage(SQ_E_UNSUPPORTED, "import audio: stream length is unknown");
    const double seconds = static_cast<double>(fmt.frame_count) / fmt.sample_rate;

    sq::Ptr<ITempoMap> tm;
    r = d->getTempoMap(tm.put());
    if (SQ_FAILED(r) || !tm) return setMessage(r, "import audio: no tempo map");
    const double start = num(a, "start", 0.0);
    if (start < 0 || !std::isfinite(start)) return setMessage(SQ_E_INVALIDARG, "import audio: invalid start");
    const double offset = num(a, "source_offset", 0.0);
    if (offset < 0 || !std::isfinite(offset)) return setMessage(SQ_E_INVALIDARG, "import audio: invalid source_offset");
    if (offset >= seconds) return setMessage(SQ_E_INVALIDARG, "import audio: source_offset is past the end of the file");
    double length = num(a, "length", 0.0);
    if (length <= 0) length = tm->secondsToPpq(seconds) - tm->secondsToPpq(offset);
    if (!(length > 0) || !std::isfinite(length)) return setMessage(SQ_E_INVALIDARG, "import audio: invalid clip length");

    std::string source = path;
    if (a && a->getBool("copy", SQ_FALSE)) {
      std::string dir;
      char docPath[1024] = {};
      uint32_t need = 0;
      if (d->getPath(docPath, sizeof docPath, &need) == SQ_OK && need > 1) dir = sq::joinPath(dirOf(docPath), "Media");
      if (dir.empty()) {
        const std::string media = sh.settings.get("Media.Folder");
        if (!media.empty() && media.rfind("mem://", 0) != 0) dir = media;
      }
      if (dir.empty()) return setMessage(SQ_E_INVALIDARG, "import audio: copy requested, but the song is unsaved and Media.Folder is empty");
      r = sq_fs_create_dirs(dir.c_str());
      if (SQ_FAILED(r)) return setMessage(r, "import audio: cannot create " + dir);
      const std::string base = fileNameOf(path);
      std::string dest = sq::joinPath(dir, base);
      for (int n = 1; sq_fs_exists(dest.c_str()) && dest != path && n < 1000; ++n)
        dest = sq::joinPath(dir, stemOf(base) + "-" + std::to_string(n) + (base.size() > stemOf(base).size() ? base.substr(stemOf(base).size()) : std::string()));
      if (dest != path) {
        r = sq_fs_copy_file(path.c_str(), dest.c_str(), SQ_FALSE);
        if (SQ_FAILED(r)) return setMessage(r, "import audio: cannot copy media to " + dest);
        source = dest;
      }
    }

    const bool explicitTrack = a && (a->getType("track") == SQ_VALUE_GUID || a->getType("track_index") != SQ_VALUE_NONE);
    sq::Ptr<ITrack> t = track(a);
    if (explicitTrack && !t) return setMessage(SQ_E_NOTFOUND, "import audio: target track not found");
    auto usable = [](const sq_track_info& ti) { return ti.kind == SQ_TRACK_AUDIO || ti.kind == SQ_TRACK_INSTRUMENT; };
    sq_track_info ti{};
    ti.struct_size = sizeof ti;
    bool haveTrack = false;
    if (t && t->getTrackInfo(&ti) == SQ_OK) {
      if (!usable(ti)) {
        if (explicitTrack) return setMessage(SQ_E_INVALIDARG, "import audio: the target track cannot hold audio clips");
        t.reset();
      } else {
        haveTrack = true;
      }
    }

    auto ed = editor();
    if (!ed) return SQ_E_NOTFOUND;
    sq_result tx = ed->beginTransaction("Import Audio");
    if (SQ_FAILED(tx)) return tx;
    auto fail = [&](sq_result e, const std::string& msg) {
      ed->cancelTransaction();
      return setMessage(e, msg);
    };

    sq_guid trackId{};
    if (!haveTrack) {
      auto p = sq::makeAttributes();
      p->setInt("kind", SQ_TRACK_AUDIO);
      p->setString("name", stemOf(fileNameOf(source)).c_str());
      auto tr = sq::makeAttributes();
      r = exec(sq::doc::kAddTrack, p.get(), tr.get());
      if (SQ_FAILED(r)) return fail(r, "import audio: cannot add a track");
      tr->getGuid("node", &trackId);
    } else {
      trackId = nodeId(t.get());
    }
    if (sq_guid_is_null(&trackId)) return fail(SQ_E_FAIL, "import audio: no target track");

    auto cp = sq::makeAttributes();
    cp->setGuid("track", &trackId);
    cp->setInt("kind", SQ_CLIP_AUDIO);
    cp->setDouble("start", start);
    cp->setDouble("length", length);
    cp->setString("source", source.c_str());
    cp->setDouble("source_offset", offset);
    const std::string clipName = str(a, "name", stemOf(fileNameOf(source)));
    if (!clipName.empty()) cp->setString("name", clipName.c_str());
    auto cr = sq::makeAttributes();
    r = exec(sq::doc::kAddClip, cp.get(), cr.get());
    if (SQ_FAILED(r)) return fail(r, "import audio: cannot add the clip");

    r = ed->commitTransaction();
    if (SQ_FAILED(r)) return setMessage(r, "import audio: commit failed");
    sq_guid clipId{};
    cr->getGuid("node", &clipId);
    if (res) {
      res->setGuid("node", &clipId);
      res->setGuid("track", &trackId);
      res->setDouble("length", length);
      res->setDouble("seconds", seconds);
      res->setInt("sample_rate", static_cast<int64_t>(std::llround(fmt.sample_rate)));
      res->setInt("channels", fmt.channels);
      res->setInt("frames", fmt.frame_count);
    }
    return setMessage(SQ_OK, "imported " + fileNameOf(source) + " (" + std::to_string(seconds) + " s)");
  }

  // File.ImportMidi path=<file.mid> [name=<prefix>] -> one instrument track + clip per SMF track.
  sq_result importMidi(IAttributes* a, IAttributes* res) {
    auto d = active();
    const std::string path = str(a, "path");
    if (!d) return SQ_E_NOTFOUND;
    if (path.empty()) return setMessage(SQ_E_INVALIDARG, "import MIDI: path is required");
    if (!sq_fs_is_file(path.c_str())) return setMessage(SQ_E_NOTFOUND, "import MIDI: no such file: " + path);
    auto rd = sq::createInstance<IMidiFileReader>(&sh.registry, sq::midi::kMidiFileReaderClassId);
    if (!rd) return setMessage(SQ_E_UNSUPPORTED, "import MIDI: no MIDI file reader (codec-midifile not loaded)");
    sq_result r = rd->open(path.c_str());
    if (SQ_FAILED(r)) return setMessage(r, "import MIDI: cannot open " + path);
    sq_midi_file_info info{};
    info.struct_size = sizeof info;
    r = rd->getInfo(&info);
    if (SQ_FAILED(r)) return setMessage(r, "import MIDI: cannot read file info");
    if (info.ticks_per_quarter == 0) return setMessage(SQ_E_UNSUPPORTED, "import MIDI: SMPTE-timed files are not supported");
    const double tpq = info.ticks_per_quarter;

    struct Note {
      double start = 0, length = 0;
      uint8_t bytes[3] = {0x90, 60, 100};
    };
    struct Part {
      std::string name;
      std::vector<Note> notes;
      double length = 0;
    };
    std::vector<Part> parts;
    for (uint32_t ti = 0; ti < info.track_count; ++ti) {
      Part part;
      char nb[256] = {};
      uint32_t need = 0;
      if (rd->getTrackName(ti, nb, sizeof nb, &need) == SQ_OK) part.name = nb;
      struct Open { uint64_t tick; uint8_t velocity; };
      std::map<std::pair<uint8_t, uint8_t>, std::vector<Open>> open;
      uint64_t lastTick = 0;
      auto close = [&](uint8_t ch, uint8_t note, uint64_t tick, uint8_t velocity) {
        auto it = open.find({ch, note});
        if (it == open.end() || it->second.empty()) return;
        const Open o = it->second.back();
        it->second.pop_back();
        if (tick <= o.tick) tick = o.tick + 1;  // dangling note-on: keep a minimal event
        Note n;
        n.start = static_cast<double>(o.tick) / tpq;
        n.length = static_cast<double>(tick - o.tick) / tpq;
        n.bytes[0] = static_cast<uint8_t>(0x90u | (ch & 0x0Fu));
        n.bytes[1] = note;
        n.bytes[2] = o.velocity ? o.velocity : velocity;
        part.length = std::max(part.length, n.start + n.length);
        part.notes.push_back(n);
      };
      const uint32_t evCount = rd->getEventCount(ti);
      for (uint32_t ei = 0; ei < evCount; ++ei) {
        sq_midi_file_event ev{};
        ev.struct_size = sizeof ev;
        if (rd->getEvent(ti, ei, &ev) != SQ_OK) continue;
        lastTick = std::max(lastTick, ev.tick);
        if (ev.status >= 0x80 && ev.status < 0xF0) {
          const uint8_t ch = ev.status & 0x0Fu;
          const uint8_t type = ev.status & 0xF0u;
          if (type == 0x90u && ev.data2 > 0) open[{ch, ev.data1}].push_back(Open{ev.tick, ev.data2});
          else if (type == 0x80u || (type == 0x90u && ev.data2 == 0)) close(ch, ev.data1, ev.tick, ev.data2);
        }
      }
      for (auto& [key, stack] : open)
        while (!stack.empty()) {
          close(key.first, key.second, std::max(lastTick, stack.back().tick), 64);
        }
      if (!part.notes.empty()) parts.push_back(std::move(part));
    }
    if (parts.empty()) return setMessage(SQ_E_NOTFOUND, "import MIDI: no notes found");

    auto ed = editor();
    if (!ed) return SQ_E_NOTFOUND;
    sq_result tx = ed->beginTransaction("Import MIDI");
    if (SQ_FAILED(tx)) return tx;
    auto fail = [&](sq_result e, const std::string& msg) {
      ed->cancelTransaction();
      return setMessage(e, msg);
    };
    const std::string prefix = str(a, "name", stemOf(fileNameOf(path)));
    int64_t madeClips = 0;
    for (size_t pi = 0; pi < parts.size(); ++pi) {
      Part& part = parts[pi];
      auto tp = sq::makeAttributes();
      tp->setInt("kind", SQ_TRACK_INSTRUMENT);
      const std::string trackName =
          part.name.empty() ? prefix + (parts.size() > 1 ? " " + std::to_string(pi + 1) : "") : part.name;
      tp->setString("name", trackName.c_str());
      auto tr = sq::makeAttributes();
      r = exec(sq::doc::kAddTrack, tp.get(), tr.get());
      if (SQ_FAILED(r)) return fail(r, "import MIDI: cannot add an instrument track");
      sq_guid trackId{};
      tr->getGuid("node", &trackId);
      auto cp = sq::makeAttributes();
      cp->setGuid("track", &trackId);
      cp->setInt("kind", SQ_CLIP_MIDI);
      cp->setDouble("start", 0);
      cp->setDouble("length", std::max(0.25, part.length));
      cp->setString("name", (part.name.empty() ? trackName : part.name).c_str());
      auto cr = sq::makeAttributes();
      r = exec(sq::doc::kAddClip, cp.get(), cr.get());
      if (SQ_FAILED(r)) return fail(r, "import MIDI: cannot add a clip");
      sq_guid clipId{};
      cr->getGuid("node", &clipId);
      std::vector<sq_clip_event> events(part.notes.size());
      for (size_t i = 0; i < part.notes.size(); ++i) {
        events[i].struct_size = sizeof(sq_clip_event);
        events[i].position = part.notes[i].start;
        events[i].length = part.notes[i].length;
        std::memcpy(events[i].bytes, part.notes[i].bytes, 3);
        events[i].size = 3;
      }
      auto ep = sq::makeAttributes();
      ep->setGuid("clip", &clipId);
      ep->setBlob("events", events.data(), static_cast<uint32_t>(events.size() * sizeof(sq_clip_event)));
      r = exec(sq::doc::kSetClipEvents, ep.get());
      if (SQ_FAILED(r)) return fail(r, "import MIDI: cannot add notes");
      ++madeClips;
    }
    r = ed->commitTransaction();
    if (SQ_FAILED(r)) return setMessage(r, "import MIDI: commit failed");
    if (res) {
      res->setInt("tracks", static_cast<int64_t>(parts.size()));
      res->setInt("clips", madeClips);
    }
    return setMessage(SQ_OK, "imported " + std::to_string(parts.size()) + " MIDI track(s) from " + fileNameOf(path));
  }

  sq_result exportMidi(IAttributes* a, IAttributes* res) {
    auto d = active();
    std::string path = str(a, "path");
    if (!d || path.empty()) return SQ_E_INVALIDARG;
    auto w = sq::createInstance<IMidiFileWriter>(&sh.registry, sq::midi::kMidiFileWriterClassId);
    if (!w) return setMessage(SQ_E_UNSUPPORTED, "no MIDI file writer (codec-midifile not loaded)");
    const uint32_t tpq = 480;
    sq_result r = w->create(path.c_str(), 1, tpq);
    if (SQ_FAILED(r)) return setMessage(r, "MIDI export: cannot create " + path);
    sq_result firstErr = SQ_OK;
    std::string where;
    auto note = [&](sq_result x, const char* step) {
      if (SQ_FAILED(x) && SQ_SUCCEEDED(firstErr)) {
        firstErr = x;
        where = step;
      }
    };
    auto meta = [&](uint64_t tick, uint8_t type, const std::string& payload) {
      sq_midi_file_event e{};
      e.struct_size = sizeof e;
      e.tick = tick;
      e.status = 0xFF;
      e.meta_type = type;
      e.size = static_cast<uint32_t>(payload.size());
      e.data = reinterpret_cast<const uint8_t*>(payload.data());
      return w->addEvent(&e);
    };
    auto toTick = [&](double ppq) { return static_cast<uint64_t>(std::llround(std::max(0.0, ppq) * tpq)); };
    // Conductor track: tempo map + signatures, merged in tick order.
    note(w->beginTrack(), "begin conductor");
    std::vector<std::tuple<uint64_t, uint8_t, std::string>> conductor;
    sq::Ptr<ITempoMap> tm;
    if (d->getTempoMap(tm.put()) == SQ_OK && tm) {
      for (uint32_t i = 0; i < tm->getTempoCount(); ++i) {
        sq_tempo_point tp{};
        tp.struct_size = sizeof tp;
        if (tm->getTempo(i, &tp) != SQ_OK || !(tp.bpm > 0)) continue;
        uint32_t us = static_cast<uint32_t>(std::llround(60000000.0 / tp.bpm));
        std::string pl{static_cast<char>((us >> 16) & 0xFF), static_cast<char>((us >> 8) & 0xFF), static_cast<char>(us & 0xFF)};
        conductor.emplace_back(toTick(tp.position), uint8_t{0x51}, pl);
      }
      for (uint32_t i = 0; i < tm->getSignatureCount(); ++i) {
        sq_time_signature ts{};
        ts.struct_size = sizeof ts;
        if (tm->getSignature(i, &ts) != SQ_OK) continue;
        uint8_t dd = 0;
        for (uint32_t v = ts.denominator; v > 1; v >>= 1) ++dd;
        std::string pl{static_cast<char>(ts.numerator), static_cast<char>(dd), 24, 8};
        conductor.emplace_back(toTick(ts.position), uint8_t{0x58}, pl);
      }
    }
    std::stable_sort(conductor.begin(), conductor.end(), [](const auto& x, const auto& y) { return std::get<0>(x) < std::get<0>(y); });
    for (const auto& [tick, type, payload] : conductor) note(meta(tick, type, payload), "conductor meta");
    note(w->endTrack(), "end conductor");
    sq_document_info di{};
    di.struct_size = sizeof di;
    d->getDocumentInfo(&di);
    int tracks = 0, notes = 0;
    for (uint32_t ti = 0; ti < di.track_count; ++ti) {
      sq::Ptr<ITrack> t;
      if (d->getTrack(ti, t.put()) != SQ_OK || !t) continue;
      sq_track_info info{};
      info.struct_size = sizeof info;
      t->getTrackInfo(&info);
      if (info.kind != SQ_TRACK_INSTRUMENT) continue;
      struct Ev {
        uint64_t tick;
        int order;
        uint8_t b[3];
      };
      std::vector<Ev> evs;
      for (uint32_t ci = 0; ci < info.clip_count; ++ci) {
        sq::Ptr<IClip> c;
        if (t->getClip(ci, c.put()) != SQ_OK || !c) continue;
        sq_clip_info cinfo{};
        cinfo.struct_size = sizeof cinfo;
        c->getClipInfo(&cinfo);
        if (cinfo.kind != SQ_CLIP_MIDI || (cinfo.flags & SQ_CLIP_MUTED)) continue;
        for (uint32_t k = 0; k < cinfo.event_count; ++k) {
          sq_clip_event ev{};
          ev.struct_size = sizeof ev;
          if (c->getEvent(k, &ev) != SQ_OK || ev.position < 0 || ev.position >= cinfo.length) continue;
          uint64_t at = toTick(cinfo.start + ev.position);
          if ((ev.bytes[0] & 0xF0u) == 0x90u && ev.bytes[2] > 0) {
            evs.push_back(Ev{at, 1, {ev.bytes[0], ev.bytes[1], ev.bytes[2]}});
            double endPpq = cinfo.start + std::min(ev.position + ev.length, cinfo.length);
            evs.push_back(Ev{toTick(endPpq), 0, {static_cast<uint8_t>(0x80u | (ev.bytes[0] & 0x0Fu)), ev.bytes[1], 0}});
            ++notes;
          } else if (ev.size >= 1 && ev.size <= 3 && ev.bytes[0] >= 0x80 && ev.bytes[0] < 0xF0) {
            evs.push_back(Ev{at, 1, {ev.bytes[0], ev.bytes[1], ev.bytes[2]}});
          }
        }
      }
      std::stable_sort(evs.begin(), evs.end(), [](const Ev& x, const Ev& y) { return x.tick != y.tick ? x.tick < y.tick : x.order < y.order; });
      note(w->beginTrack(), "begin track");
      note(meta(0, 0x03, nodeName(t.get())), "track name");
      for (const Ev& e : evs) {
        sq_midi_file_event fe{};
        fe.struct_size = sizeof fe;
        fe.tick = e.tick;
        fe.status = e.b[0];
        fe.data1 = e.b[1];
        fe.data2 = e.b[2];
        // Channel messages carry their bytes in data1/data2 (size/data are for meta and sysex).
        note(w->addEvent(&fe), "note event");
      }
      note(w->endTrack(), "end of track");
      ++tracks;
    }
    note(w->finish(), "finish");
    r = firstErr;
    if (SQ_FAILED(r)) return setMessage(r, std::string("MIDI export failed at ") + where + ": " + sq_result_string(r));
    if (res) {
      res->setInt("tracks", tracks);
      res->setInt("notes", notes);
    }
    return setMessage(r, SQ_SUCCEEDED(r) ? "exported MIDI to " + path : std::string("MIDI export failed: ") + sq_result_string(r));
  }

  // Plugin inserts: "Track.InsertPlugin" appends "<format>|<id>|<path>[|<param>=<value>...]" to the
  // track's user.inserts (one undo step); the resolver below turns specs into engine processors.
  sq_result insertPlugin(IAttributes* a) {
    auto t = track(a);
    std::string want = str(a, "plugin");
    auto reg = sh.plugins();
    if (!t || want.empty()) return SQ_E_INVALIDARG;
    if (!reg) return setMessage(SQ_E_UNSUPPORTED, "no plugin registry (pluginscanner not loaded)");
    std::string fmt, id = want;
    if (size_t c = want.find(':'); c != std::string::npos) {
      fmt = want.substr(0, c);
      id = want.substr(c + 1);
    }
    sq_plugin_desc found{};
    bool ok = false;
    for (uint32_t i = 0; i < reg->getPluginCount() && !ok; ++i) {
      sq_plugin_desc d{};
      d.struct_size = sizeof d;
      if (reg->getPlugin(i, &d) != SQ_OK || d.status != SQ_PLUGIN_STATUS_OK) continue;
      if ((fmt.empty() || fmt == d.format) && (id == d.id || id == d.name)) {
        found = d;
        ok = true;
      }
    }
    if (!ok) return setMessage(SQ_E_NOTFOUND, "plugin not found or quarantined: " + want);
    std::string spec = std::string(found.format) + "|" + found.id + "|" + found.path;
    std::string params = str(a, "params");
    size_t p = 0;
    while (p < params.size()) {
      size_t e = params.find(';', p);
      std::string kv = params.substr(p, e == std::string::npos ? std::string::npos : e - p);
      if (!kv.empty()) spec += "|" + kv;
      p = e == std::string::npos ? params.size() : e + 1;
    }
    auto props = sq::makeAttributes();
    t->getProperties(props.get());
    std::string cur = sq::getAttrString(props.get(), "user.inserts");
    std::string next = cur.empty() ? spec : cur + "\n" + spec;
    auto ed = editor();
    ed->beginTransaction("Insert Plugin");
    sq_result r = setTrackProp(t.get(), "user.inserts", [&](IAttributes* q) { q->setString("value", next.c_str()); });
    if (SQ_FAILED(r)) {
      ed->cancelTransaction();
      return r;
    }
    r = ed->commitTransaction();
    return setMessage(r, std::string("inserted ") + found.name);
  }

  // ---- insert / instrument resolvers (AudioEngine::setInsertResolver / setInstrumentResolver) ----
  // Specs: "builtin|<id>|<path>[|i=v]..." -> engine/audio built-in devices (makeBuiltin; a non-
  // empty path is decoded with loadMediaFile and handed to makeBuiltinWithMedia, e.g. the sampler);
  // "<clap|vst3>|<plugin id>|<path>[|<paramId>=<value>]..." -> plugin instance (PluginFx inserts,
  // PluginSynth instruments).
  std::map<std::string, engine::audio::MediaPtr> sampleCache;  // decoded media of built-in devices (main thread)

  engine::audio::ProcessorProto resolveBuiltin(const engine::audio::SpecParts& sp, std::string* err) {
    if (sp.path.empty()) return engine::audio::makeBuiltin(sp.id, sp.params, err);
    engine::audio::MediaPtr media;
    if (auto it = sampleCache.find(sp.path); it != sampleCache.end()) {
      media = it->second;
    } else {
      const sq_result r = engine::audio::loadMediaFile(&sh.registry, sp.path.c_str(), &media);
      if (SQ_FAILED(r) || !media) {
        if (err) *err = "cannot decode " + sp.path + ": " + sq_result_string(r);
        return nullptr;
      }
      if (sampleCache.size() > 64) sampleCache.clear();
      sampleCache[sp.path] = media;
    }
    return engine::audio::makeBuiltinWithMedia(sp.id, media, sp.params, err);
  }

  engine::audio::PluginFactory pluginFactory(const engine::audio::SpecParts& sp) {
    sq_plugin_desc desc{};
    desc.struct_size = sizeof desc;
    auto cp = [](char* dst, size_t cap, const std::string& s) {
      size_t n = std::min(cap - 1, s.size());
      std::memcpy(dst, s.data(), n);
      dst[n] = 0;
    };
    cp(desc.format, sizeof desc.format, sp.format);
    cp(desc.id, sizeof desc.id, sp.id);
    cp(desc.path, sizeof desc.path, sp.path);
    cp(desc.name, sizeof desc.name, sp.id);
    desc.status = SQ_PLUGIN_STATUS_OK;
    IHostContext* hostCtx = &sh.host;
    IClassRegistry* creg = &sh.registry;
    const auto params = sp.params;
    return [desc, params, hostCtx, creg](std::string* e) -> sq::Ptr<IPluginInstance> {
      sq::Ptr<IPluginInstance> inst;
      sq::Ptr<IPluginRegistry> reg;
      sq_result r = SQ_E_NOTFOUND;
      if (hostCtx->queryService(&IPluginRegistry::iid, reg.putVoid()) == SQ_OK && reg) r = reg->instantiate(&desc, nullptr, inst.put());
      if (!inst) {  // no registry: ask the format adapters directly
        for (uint32_t i = 0; i < creg->getClassCount() && !inst; ++i) {
          sq_class_info ci{};
          ci.struct_size = sizeof ci;
          if (creg->getClassInfo(i, &ci) != SQ_OK || !ci.category || std::strcmp(ci.category, pluginhost::kCategory) != 0) continue;
          auto f = sq::createInstance<IPluginFormat>(creg, ci.class_id);
          sq_plugin_format_info fi{};
          fi.struct_size = sizeof fi;
          if (f && f->getFormatInfo(&fi) == SQ_OK && std::strcmp(fi.format, desc.format) == 0) r = f->instantiate(&desc, nullptr, inst.put());
        }
      }
      if (!inst) {
        if (e) *e = std::string("instantiate failed: ") + sq_result_string(r);
        return inst;
      }
      for (const auto& [id, v] : params) inst->setParamValue(id, v);
      return inst;
    };
  }

  engine::audio::ProcessorProto resolveInsert(const std::string& spec, std::string* err) {
    engine::audio::SpecParts sp;
    if (!engine::audio::parseSpec(spec, &sp)) {
      if (err) *err = "malformed insert spec";
      return nullptr;
    }
    if (sp.format == "builtin") {
      auto p = resolveBuiltin(sp, err);
      if (p && p->kind() == engine::audio::AudioProcessor::Kind::Synth) {
        if (err) *err = "'" + sp.id + "' is an instrument, not an effect";
        return nullptr;
      }
      return p;
    }
    if (sp.path.empty()) {
      if (err) *err = "malformed insert spec (no plugin path)";
      return nullptr;
    }
    return std::make_shared<engine::audio::PluginFx>(pluginFactory(sp), sp.id);
  }

  engine::audio::ProcessorProto resolveInstrument(const std::string& spec, std::string* err) {
    engine::audio::SpecParts sp;
    if (!engine::audio::parseSpec(spec, &sp)) {
      if (err) *err = "malformed instrument spec";
      return nullptr;
    }
    if (sp.format == "builtin") {
      auto p = resolveBuiltin(sp, err);
      if (p && p->kind() != engine::audio::AudioProcessor::Kind::Synth) {
        if (err) *err = "'" + sp.id + "' is an effect, not an instrument";
        return nullptr;
      }
      return p;
    }
    if (sp.format != "clap" && sp.format != "vst3") {
      if (err) *err = "unknown instrument format '" + sp.format + "'";
      return nullptr;
    }
    if (sp.path.empty()) {
      if (err) *err = "malformed instrument spec (no plugin path)";
      return nullptr;
    }
    return std::make_shared<engine::audio::PluginSynth>(pluginFactory(sp), sp.id);
  }

  // =============================================================================================
  // Mixer / device / automation / MIDI commands (engine). Track arguments: "track" = GUID (or its
  // text), a track name, or an index (number); default = the selected track (edit.track journal),
  // else the last track. Bus arguments ("bus") take the same forms plus "master".
  // =============================================================================================
  static std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p < text.size()) {
      size_t e = text.find('\n', p);
      std::string l = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
      p = e == std::string::npos ? text.size() : e + 1;
      while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
      size_t b = 0;
      while (b < l.size() && (l[b] == ' ' || l[b] == '\t')) ++b;
      if (b < l.size()) out.push_back(l.substr(b));
    }
    return out;
  }
  static std::string joinLines(const std::vector<std::string>& lines) {
    std::string s;
    for (size_t i = 0; i < lines.size(); ++i) s += (i ? "\n" : "") + lines[i];
    return s;
  }
  static std::string fmtNum(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.10g", v);
    return b;
  }
  edoc::DocumentImpl* activeImpl() const {
    auto d = active();
    return d ? edoc::DocumentImpl::from(d.get()) : nullptr;
  }
  sq_guid selectedTrack() const {
    if (auto* impl = activeImpl()) return edit::readEditState(*impl).track;
    return sq_guid{};
  }
  void selectTrack(const sq_guid& id) {
    if (auto* impl = activeImpl()) edit::setFocusTrack(*impl, id);
    syncLiveTarget();
  }
  // Keeps the engine's live MIDI target on the selected track (GUI selection, Navigate.*).
  void syncLiveTarget() {
    if (!audio) return;
    const sq_guid sel = selectedTrack();
    if (!(sel == audio->liveMidiTarget())) audio->setLiveMidiTarget(sel);
  }
  sq::Ptr<ITrack> trackByIndex(int64_t idx) {
    sq::Ptr<ITrack> t;
    auto d = active();
    if (!d) return t;
    sq_document_info di{};
    di.struct_size = sizeof di;
    d->getDocumentInfo(&di);
    if (idx >= 0 && idx < static_cast<int64_t>(di.track_count)) d->getTrack(static_cast<uint32_t>(idx), t.put());
    return t;
  }
  sq::Ptr<ITrack> trackById(const sq_guid& g) {
    sq::Ptr<ITrack> t;
    auto d = active();
    sq::Ptr<INode> n;
    if (d && !sq_guid_is_null(&g) && d->findNode(&g, n.put()) == SQ_OK && n) t = n.as<ITrack>();
    return t;
  }
  // Explicit track argument (GUID, GUID text, name, index); null when absent or not found.
  sq::Ptr<ITrack> trackFromArg(IAttributes* a, const char* key, bool* given = nullptr) {
    if (given) *given = a && a->getType(key) != SQ_VALUE_NONE;
    if (!a) return {};
    const int32_t type = a->getType(key);
    if (type == SQ_VALUE_GUID) {
      sq_guid g{};
      a->getGuid(key, &g);
      return trackById(g);
    }
    if (type == SQ_VALUE_INT || type == SQ_VALUE_DOUBLE) return trackByIndex(static_cast<int64_t>(std::llround(a->getDouble(key, -1))));
    if (type == SQ_VALUE_STRING) {
      const std::string s = sq::getAttrString(a, key);
      sq_guid g{};
      if (sq::parseGuid(s, g) || sq::parseGuid("{" + s + "}", g)) return trackById(g);
      auto d = active();
      if (!d) return {};
      sq_document_info di{};
      di.struct_size = sizeof di;
      d->getDocumentInfo(&di);
      for (uint32_t i = 0; i < di.track_count; ++i) {
        sq::Ptr<ITrack> t;
        if (d->getTrack(i, t.put()) == SQ_OK && t && nodeName(t.get()) == s) return t;
      }
    }
    return {};
  }
  // Track by args "track" / "track_index"; default: the selected track, else the last track.
  sq::Ptr<ITrack> track(IAttributes* args) {
    bool given = false;
    if (auto t = trackFromArg(args, "track", &given); t || given) return t;
    if (args && args->getType("track_index") != SQ_VALUE_NONE) return trackByIndex(args->getInt("track_index", -1));
    if (auto t = trackById(selectedTrack())) return t;
    auto d = active();
    if (!d) return {};
    sq_document_info di{};
    di.struct_size = sizeof di;
    d->getDocumentInfo(&di);
    return trackByIndex(static_cast<int64_t>(di.track_count) - 1);
  }
  static sq_track_info trackInfo(ITrack* t) {
    sq_track_info ti{};
    ti.struct_size = sizeof ti;
    if (t) t->getTrackInfo(&ti);
    return ti;
  }
  static std::string trackStr(ITrack* t, const char* key) {
    auto props = sq::makeAttributes();
    if (t) t->getProperties(props.get());
    return sq::getAttrString(props.get(), key);
  }
  sq_result setTrackString(ITrack* t, const char* key, const std::string& value, bool merge = false) {
    return setTrackProp(t, key, [&](IAttributes* p) { p->setString("value", value.c_str()); }, merge);
  }
  // One undo step around fn (named `title`).
  template <class F>
  sq_result step(const char* title, F&& fn) {
    auto ed = editor();
    if (!ed) return SQ_E_NOTFOUND;
    ed->beginTransaction(title);
    const sq_result r = fn();
    if (SQ_FAILED(r)) {
      ed->cancelTransaction();
      return r;
    }
    const sq_result c = ed->commitTransaction();
    return SQ_FAILED(c) ? c : r;
  }
  static bool argNum(IAttributes* a, const char* key, double* out) {
    if (!a) return false;
    const int32_t t = a->getType(key);
    if (t == SQ_VALUE_INT || t == SQ_VALUE_DOUBLE) {
      *out = a->getDouble(key, 0);
      return std::isfinite(*out);
    }
    if (t == SQ_VALUE_STRING) {
      const std::string s = sq::getAttrString(a, key);
      char* end = nullptr;
      const double v = std::strtod(s.c_str(), &end);
      if (end && end != s.c_str() && *end == '\0' && std::isfinite(v)) {
        *out = v;
        return true;
      }
    }
    return false;
  }
  static bool argInt(IAttributes* a, const char* key, int64_t* out) {
    double v = 0;
    if (!argNum(a, key, &v) || v != std::floor(v) || std::fabs(v) > 9.0e15) return false;
    *out = static_cast<int64_t>(v);
    return true;
  }
  // A parameter / slot index: a whole number in 0..2^32-1 (larger values would wrap on the cast).
  static bool argIndex(IAttributes* a, const char* key, uint32_t* out) {
    int64_t v = 0;
    if (!argInt(a, key, &v) || v < 0 || v > static_cast<int64_t>(UINT32_MAX)) return false;
    *out = static_cast<uint32_t>(v);
    return true;
  }
  // dB range accepted for send levels and volume / send automation (dbToGain: <= -144 dB is silence).
  static constexpr double kMinDb = -144.0;
  static constexpr double kMaxDb = 24.0;
  static bool validDb(double v) { return std::isfinite(v) && v >= kMinDb && v <= kMaxDb; }
  // Device spec text: one line (a newline would split one insert into several) and well-formed.
  static bool validSpec(const std::string& spec) {
    if (spec.find_first_of("\r\n") != std::string::npos) return false;
    engine::audio::SpecParts sp;
    return engine::audio::parseSpec(engine::audio::specWithoutBypass(spec), &sp);
  }
  // Length (status included) of a MIDI message; sysex (F0/F7) is not accepted by Midi.Inject.
  static size_t midiMessageLength(uint8_t st) {
    if (st >= 0xF0) return st == 0xF1 || st == 0xF3 ? size_t{2} : st == 0xF2 ? size_t{3} : size_t{1};
    return ((st & 0xF0u) == 0xC0 || (st & 0xF0u) == 0xD0) ? size_t{2} : size_t{3};
  }
  static bool validInstrument(const std::string& spec) {
    if (spec.find_first_of("\r\n") != std::string::npos) return false;
    engine::audio::SpecParts sp;
    return engine::audio::parseSpec(spec, &sp);
  }
  static bool argBool(IAttributes* a, const char* key, bool def) {
    if (!a || a->getType(key) == SQ_VALUE_NONE) return def;
    if (a->getType(key) == SQ_VALUE_STRING) {
      const std::string s = sq::getAttrString(a, key);
      return s == "true" || s == "1" || s == "yes" || s == "on";
    }
    return a->getDouble(key, def ? 1 : 0) != 0 || a->getBool(key, def ? SQ_TRUE : SQ_FALSE) != SQ_FALSE;
  }
  static bool argColor(IAttributes* a, uint32_t* out) {
    if (!a || a->getType("color") == SQ_VALUE_NONE) return false;
    if (a->getType("color") == SQ_VALUE_STRING) {
      std::string s = sq::getAttrString(a, "color");
      if (!s.empty() && s[0] == '#') s = "0x" + s.substr(1);
      char* end = nullptr;
      const unsigned long long v = std::strtoull(s.c_str(), &end, 0);
      if (!end || *end != '\0' || v > 0xFFFFFFFFull) return false;
      *out = static_cast<uint32_t>(v);
      return true;
    }
    const double v = a->getDouble("color", -1);
    if (v < 0 || v > 4294967295.0) return false;
    *out = static_cast<uint32_t>(v);
    return true;
  }
  sq_result needTrack(const sq::Ptr<ITrack>& t) { return t ? SQ_OK : setMessage(SQ_E_NOTFOUND, "no such track"); }
  std::string trackLabel(ITrack* t) { return "'" + nodeName(t) + "'"; }

  // Resolves the "bus" argument: master -> null guid. false = not a bus/FX/master track.
  bool busArg(IAttributes* a, sq_guid* out, std::string* label) {
    if (a && a->getType("bus") == SQ_VALUE_STRING) {
      std::string s = lower(sq::getAttrString(a, "bus"));
      if (s == "master" || s.empty()) {
        *out = sq_guid{};
        *label = "master";
        return true;
      }
    }
    auto b = trackFromArg(a, "bus");
    if (!b) return false;
    const sq_track_info bi = trackInfo(b.get());
    if (bi.kind == SQ_TRACK_MASTER) {
      *out = sq_guid{};
      *label = "master";
      return true;
    }
    if (bi.kind != SQ_TRACK_BUS && bi.kind != SQ_TRACK_FX) return false;
    *out = nodeId(b.get());
    *label = trackLabel(b.get());
    return true;
  }

  // True when audio leaving track `from` can arrive at track `target` through outputs and sends
  // (a null output is the master, which routes nowhere). Used to refuse feedback loops.
  bool reaches(const sq_guid& from, const sq_guid& target) {
    std::vector<sq_guid> todo{from};
    std::set<std::string> seen;
    while (!todo.empty()) {
      const sq_guid g = todo.back();
      todo.pop_back();
      if (sq_guid_is_null(&g)) continue;
      if (g == target) return true;
      if (!seen.insert(sq::toString(g)).second) continue;
      auto t = trackById(g);
      if (!t) continue;
      auto props = sq::makeAttributes();
      t->getProperties(props.get());
      sq_guid out{};
      if (props->getGuid("output", &out) == SQ_OK) todo.push_back(out);
      for (const std::string& line : splitLines(sq::getAttrString(props.get(), "user.sends"))) {
        sq_guid dest{};
        if (sq::parseGuid(line.substr(0, line.find(' ')), dest)) todo.push_back(dest);
      }
    }
    return false;
  }

  // ---- track structure ----
  sq_result addTrackKind(IAttributes* a, IAttributes* res, int32_t kind, const std::string& defName, const std::string& instrument) {
    auto p = sq::makeAttributes();
    p->setInt("kind", kind);
    p->setString("name", str(a, "name", defName).c_str());
    if (a && a->getType("channels") != SQ_VALUE_NONE) p->setInt("channels", static_cast<int64_t>(a->getDouble("channels", 2)));
    auto out = sq::makeAttributes();
    sq_guid id{};
    const sq_result r = step(kind == SQ_TRACK_INSTRUMENT ? "Add Instrument Track" : kind == SQ_TRACK_AUDIO ? "Add Audio Track" : "Add Bus", [&] {
      sq_result x = exec(sq::doc::kAddTrack, p.get(), out.get());
      if (SQ_FAILED(x)) return x;
      out->getGuid("node", &id);
      if (!instrument.empty()) {
        auto t = trackById(id);
        x = t ? setTrackString(t.get(), "user.instrument", instrument) : SQ_E_FAIL;
      }
      return x;
    });
    if (SQ_FAILED(r)) return setMessage(r, std::string("add track failed: ") + sq_result_string(r));
    if (res) res->setGuid("node", &id);
    selectTrack(id);
    return setMessage(SQ_OK, "added track " + sq::toString(id) + (instrument.empty() ? std::string() : " (" + instrument + ")"));
  }

  // An editable line list ("user.inserts" / "user.sends") that remembers where each line came
  // from, so automation lanes addressed by line index ("insert.<slot>.*", "send.<n>") can follow.
  struct LineList {
    std::vector<std::string> lines;
    std::vector<int64_t> origin;  // original index of each line; -1 = new line
    explicit LineList(std::vector<std::string> l) : lines(std::move(l)) {
      for (size_t i = 0; i < lines.size(); ++i) origin.push_back(static_cast<int64_t>(i));
    }
    size_t size() const { return lines.size(); }
    std::string& operator[](size_t i) { return lines[i]; }
    void insert(size_t at, const std::string& line) {
      lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), line);
      origin.insert(origin.begin() + static_cast<std::ptrdiff_t>(at), -1);
    }
    void push_back(const std::string& line) { insert(lines.size(), line); }
    void erase(size_t at) {
      lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(at));
      origin.erase(origin.begin() + static_cast<std::ptrdiff_t>(at));
    }
    void move(size_t from, size_t to) {
      const std::string l = lines[from];
      const int64_t o = origin[from];
      erase(from);
      lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(to), l);
      origin.insert(origin.begin() + static_cast<std::ptrdiff_t>(to), o);
    }
    // old index -> new index (-1 = removed); empty when nothing moved.
    std::vector<int64_t> remap(size_t oldCount) const {
      std::vector<int64_t> m(oldCount, -1);
      bool moved = false;
      for (size_t i = 0; i < origin.size(); ++i)
        if (origin[i] >= 0) m[static_cast<size_t>(origin[i])] = static_cast<int64_t>(i);
      for (size_t i = 0; i < oldCount; ++i) moved = moved || m[i] != static_cast<int64_t>(i);
      return moved ? m : std::vector<int64_t>{};
    }
  };
  // Renumbers the track's automation lanes "<prefix><index>[.<rest>]" by map (old -> new, -1 =
  // delete the lane). Runs inside the caller's step, so the list edit and the lanes are one undo.
  sq_result remapLanes(ITrack* t, const std::string& prefix, const std::vector<int64_t>& map) {
    if (map.empty()) return SQ_OK;
    std::vector<std::pair<std::string, std::vector<std::pair<double, double>>>> moved;
    std::vector<std::string> removed;
    for (const auto& l : engine::audio::parseAutomation(trackStr(t, "automation"))) {
      if (l.target.compare(0, prefix.size(), prefix) != 0) continue;
      const size_t dot = l.target.find('.', prefix.size());
      const std::string idx = l.target.substr(prefix.size(), dot == std::string::npos ? std::string::npos : dot - prefix.size());
      uint32_t old = 0;
      if (!parseIndex(idx, &old) || old >= map.size()) continue;
      const int64_t now = map[old];
      if (now == static_cast<int64_t>(old)) continue;
      removed.push_back(l.target);
      if (now >= 0)
        moved.emplace_back(prefix + std::to_string(now) + (dot == std::string::npos ? std::string() : l.target.substr(dot)), l.points);
    }
    for (const auto& target : removed) {
      const sq_result r = setLane(t, target, {});
      if (SQ_FAILED(r)) return r;
    }
    for (const auto& [target, pts] : moved) {
      const sq_result r = setLane(t, target, pts);
      if (SQ_FAILED(r)) return r;
    }
    return SQ_OK;
  }
  // Editing the insert list ("user.inserts") of a track/bus: fn edits the lines.
  template <class F>
  sq_result editInserts(IAttributes* a, const char* title, F&& fn) {
    auto t = track(a);
    if (SQ_FAILED(needTrack(t))) return SQ_E_NOTFOUND;
    LineList lines(splitLines(trackStr(t.get(), "user.inserts")));
    const size_t before = lines.size();
    std::string msg;
    const sq_result r = fn(lines, msg);
    if (SQ_FAILED(r)) return setMessage(r, msg.empty() ? std::string(title) + ": " + sq_result_string(r) : msg);
    const bool merge = argBool(a, "merge", false);
    const sq_result w = step(title, [&] {
      const sq_result x = setTrackString(t.get(), "user.inserts", joinLines(lines.lines), merge);
      return SQ_FAILED(x) ? x : remapLanes(t.get(), "insert.", lines.remap(before));
    });
    return setMessage(w, SQ_SUCCEEDED(w) ? msg : std::string(title) + " failed: " + sq_result_string(w));
  }
  bool slotArg(IAttributes* a, const char* key, size_t count, size_t* out) {
    int64_t v = 0;
    if (!argInt(a, key, &v) || v < 0 || static_cast<size_t>(v) >= count) return false;
    *out = static_cast<size_t>(v);
    return true;
  }

  sq_result editSends(IAttributes* a, const char* title, const std::function<sq_result(LineList&, std::string&)>& fn) {
    auto t = track(a);
    if (SQ_FAILED(needTrack(t))) return SQ_E_NOTFOUND;
    LineList lines(splitLines(trackStr(t.get(), "user.sends")));
    const size_t before = lines.size();
    std::string msg;
    const sq_result r = fn(lines, msg);
    if (SQ_FAILED(r)) return setMessage(r, msg.empty() ? std::string(title) + ": " + sq_result_string(r) : msg);
    const bool merge = argBool(a, "merge", false);
    const sq_result w = step(title, [&] {
      const sq_result x = setTrackString(t.get(), "user.sends", joinLines(lines.lines), merge);
      return SQ_FAILED(x) ? x : remapLanes(t.get(), "send.", lines.remap(before));
    });
    return setMessage(w, SQ_SUCCEEDED(w) ? msg : std::string(title) + " failed: " + sq_result_string(w));
  }

  // ---- automation (edoc::kSetAutomation; the track property "automation" is the read view) ----
  sq_result setLane(ITrack* t, const std::string& target, const std::vector<std::pair<double, double>>& pts) {
    std::vector<double> raw;
    for (const auto& [p, v] : pts) {
      raw.push_back(p);
      raw.push_back(v);
    }
    auto p = sq::makeAttributes();
    sq_guid g = nodeId(t);
    p->setGuid("track", &g);
    p->setString("param", target.c_str());
    p->setBlob("points", raw.empty() ? nullptr : raw.data(), static_cast<uint32_t>(raw.size() * sizeof(double)));
    return exec(edoc::kSetAutomation, p.get());
  }
  sq_result writeLane(ITrack* t, const std::string& target, const std::vector<std::pair<double, double>>& pts, const char* title) {
    return step(title, [&] { return setLane(t, target, pts); });
  }
  // A decimal index that fits 32 bits (no sign, no wrap).
  static bool parseIndex(const std::string& s, uint32_t* out) {
    if (s.empty() || s.size() > 10 || s.find_first_not_of("0123456789") != std::string::npos) return false;
    const unsigned long long v = std::strtoull(s.c_str(), nullptr, 10);
    if (v > UINT32_MAX) return false;
    *out = static_cast<uint32_t>(v);
    return true;
  }
  // Volume and send lanes are dB values (see kMinDb / kMaxDb).
  static bool dbTarget(const std::string& t) { return t == "volume" || t.rfind("send.", 0) == 0; }
  // "ppq:value ppq:value ..." with every token fully numeric; false on any malformed token.
  static bool parsePoints(const std::string& text, std::vector<std::pair<double, double>>* out) {
    size_t p = 0;
    while (p < text.size()) {
      const size_t e = text.find_first_of(" \t", p);
      const std::string tok = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
      p = e == std::string::npos ? text.size() : e + 1;
      if (tok.empty()) continue;
      const size_t colon = tok.find(':');
      if (colon == std::string::npos) return false;
      const std::string ps = tok.substr(0, colon), vs = tok.substr(colon + 1);
      char* pe = nullptr;
      char* ve = nullptr;
      const double pos = std::strtod(ps.c_str(), &pe);
      const double val = std::strtod(vs.c_str(), &ve);
      if (ps.empty() || vs.empty() || !pe || *pe != '\0' || !ve || *ve != '\0' || !std::isfinite(pos) || !std::isfinite(val)) return false;
      out->emplace_back(pos, val);
    }
    std::stable_sort(out->begin(), out->end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    return true;
  }
  static bool validTarget(const std::string& t) {
    uint32_t v = 0;
    auto digits = [&v](const std::string& s) { return parseIndex(s, &v); };
    if (t == "volume" || t == "pan" || t == "mute") return true;
    if (t.rfind("send.", 0) == 0) return digits(t.substr(5));
    if (t.rfind("instrument.", 0) == 0) return digits(t.substr(11));
    if (t.rfind("insert.", 0) == 0) {
      const size_t dot = t.find('.', 7);
      return dot != std::string::npos && digits(t.substr(7, dot - 7)) && digits(t.substr(dot + 1));
    }
    return false;
  }

  // ---- clips ----
  sq::Ptr<IClip> clipArg(IAttributes* a) {
    auto d = active();
    if (!d) return {};
    sq_guid g{};
    bool have = false;
    if (a && a->getType("clip") == SQ_VALUE_GUID) have = a->getGuid("clip", &g) == SQ_OK;
    else if (a && a->getType("clip") == SQ_VALUE_STRING) {
      const std::string s = sq::getAttrString(a, "clip");
      have = sq::parseGuid(s, g) || sq::parseGuid("{" + s + "}", g);
    } else if (auto* impl = activeImpl()) {
      const auto st = edit::readEditState(*impl);
      if (!st.selection.empty()) {
        g = st.selection.front();
        have = true;
      }
    }
    sq::Ptr<INode> n;
    if (!have || d->findNode(&g, n.put()) != SQ_OK || !n) return {};
    return n.as<IClip>();
  }

  // ---- live MIDI input ----
  class MidiInbox final : public sq::Object<MidiInbox, IMidiInputCallback, IMidiDeviceListener> {
   public:
    std::atomic<engine::audio::AudioEngine*> target{nullptr};
    std::atomic<bool> portsChanged{false};
    std::atomic<uint64_t> events{0};
    void SQ_CALL onMidi(const sq_midi_port_info*, const sq_midi_timed_event* ev, uint32_t count) override {
      engine::audio::AudioEngine* e = target.load();
      for (uint32_t i = 0; e && i < count; ++i) {
        if (ev[i].data || ev[i].size == 0 || ev[i].size > 4) continue;  // sysex
        e->pushLiveMidi(ev[i].bytes, ev[i].size);
        events.fetch_add(1, std::memory_order_relaxed);
      }
    }
    void SQ_CALL onPortsChanged() override { portsChanged.store(true); }
  };
  sq::Ptr<MidiInbox> midiInbox;
  std::vector<sq::Ptr<IMidiDriver>> midiDrivers;
  std::map<std::string, sq::Ptr<IMidiInputPort>> midiInputs;  // port id -> open port
  bool midiWanted = false;

  void loadMidiDrivers() {
    if (!midiDrivers.empty()) return;
    if (!midiInbox) midiInbox = sq::Ptr<MidiInbox>::adopt(new MidiInbox());
    for (uint32_t i = 0; i < sh.registry.getClassCount(); ++i) {
      sq_class_info ci{};
      ci.struct_size = sizeof ci;
      if (sh.registry.getClassInfo(i, &ci) != SQ_OK || !ci.category || std::strcmp(ci.category, sq::midi::kDriverCategory) != 0) continue;
      auto drv = sq::createInstance<IMidiDriver>(&sh.registry, ci.class_id);
      if (!drv) continue;
      drv->addListener(midiInbox.get());
      midiDrivers.push_back(drv);
    }
  }
  // Opens every MIDI input port not open yet and closes vanished ones. Returns ports open.
  size_t openMidiInputs() {
    loadMidiDrivers();
    midiWanted = true;
    if (midiInbox) midiInbox->target.store(audio.get());
    std::set<std::string> seen;
    for (auto& drv : midiDrivers) {
      drv->refreshPorts();
      for (uint32_t k = 0; k < drv->getPortCount(); ++k) {
        sq_midi_port_info pi{};
        pi.struct_size = sizeof pi;
        if (drv->getPortInfo(k, &pi) != SQ_OK || !(pi.flags & SQ_MIDI_PORT_INPUT)) continue;
        const std::string id = fixed(pi.id, sizeof pi.id);
        seen.insert(id);
        if (midiInputs.count(id)) continue;
        sq::Ptr<IMidiInputPort> port;
        if (drv->openInput(pi.id, midiInbox.get(), port.put()) != SQ_OK || !port) {
          sq_log_printf(SQ_LOG_WARN, kTag, "MIDI input '%s' could not be opened", fixed(pi.name, sizeof pi.name).c_str());
          continue;
        }
        port->start();
        midiInputs[id] = port;
        sq_log_printf(SQ_LOG_INFO, kTag, "MIDI input '%s' open (live MIDI)", fixed(pi.name, sizeof pi.name).c_str());
      }
    }
    for (auto it = midiInputs.begin(); it != midiInputs.end();) {
      if (!seen.count(it->first)) {
        it->second->stop();
        it = midiInputs.erase(it);
      } else {
        ++it;
      }
    }
    return midiInputs.size();
  }
  void closeMidiInputs() {
    for (auto& [id, port] : midiInputs) port->stop();
    midiInputs.clear();
    if (midiInbox) midiInbox->target.store(nullptr);
    for (auto& drv : midiDrivers)
      if (midiInbox) drv->removeListener(midiInbox.get());
    midiDrivers.clear();
    midiWanted = false;
  }
  void pumpMidi() {  // hot-plug (best effort): reopen the port list after a driver notification
    if (midiWanted && midiInbox && midiInbox->portsChanged.exchange(false)) openMidiInputs();
  }

  void registerMixerCommands(const std::function<bool()>& haveDoc) {
    add("Track", "AddBus", "Add Bus", [this](IAttributes* a, IAttributes* res) { return addTrackKind(a, res, SQ_TRACK_BUS, "Bus", {}); }, haveDoc);
    add("Track", "AddFx", "Add FX Track", [this](IAttributes* a, IAttributes* res) { return addTrackKind(a, res, SQ_TRACK_FX, "FX", {}); }, haveDoc);
    add("Track", "AddDrums", "Add Drum Track", [this](IAttributes* a, IAttributes* res) {
      std::string spec = str(a, "instrument", "builtin|drums|");
      if (spec.empty()) spec = "builtin|drums|";  // instrument="" = the default kit
      if (!validInstrument(spec)) return setMessage(SQ_E_INVALIDARG, "malformed instrument spec (<format>|<id>|<path>[|i=v]...)");
      return addTrackKind(a, res, SQ_TRACK_INSTRUMENT, "Drums", spec);
    }, haveDoc);
    add("Track", "Remove", "Remove Track", [this](IAttributes* a, IAttributes*) {
      auto t = track(a);
      if (SQ_FAILED(needTrack(t))) return SQ_E_NOTFOUND;
      const std::string label = trackLabel(t.get());
      auto p = sq::makeAttributes();
      sq_guid g = nodeId(t.get());
      p->setGuid("node", &g);
      const sq_result r = step("Remove Track", [&] { return exec(sq::doc::kRemoveNode, p.get()); });
      return setMessage(r, SQ_SUCCEEDED(r) ? "removed track " + label : std::string("remove failed: ") + sq_result_string(r));
    }, haveDoc);
    add("Track", "Rename", "Rename Track", [this](IAttributes* a, IAttributes*) {
      auto t = track(a);
      if (SQ_FAILED(needTrack(t))) return SQ_E_NOTFOUND;
      if (!a || a->getType("name") != SQ_VALUE_STRING) return setMessage(SQ_E_INVALIDARG, "name= required");
      const std::string name = str(a, "name");
      const sq_result r = step("Rename Track", [&] { return setTrackString(t.get(), "name", name); });
      return setMessage(r, SQ_SUCCEEDED(r) ? "renamed to '" + name + "'" : std::string("rename failed: ") + sq_result_string(r));
    }, haveDoc);
    add("Track", "SetColor", "Set Track Color", [this](IAttributes* a, IAttributes*) {
      auto t = track(a);
      if (SQ_FAILED(needTrack(t))) return SQ_E_NOTFOUND;
      uint32_t color = 0;
      if (!argColor(a, &color)) return setMessage(SQ_E_INVALIDARG, "color=0xRRGGBBAA required");
      const sq_result r = step("Track Color", [&] { return setTrackProp(t.get(), "color", [&](IAttributes* p) { p->setInt("value", color); }); });
      char buf[32];
      std::snprintf(buf, sizeof buf, "0x%08X", color);
      return setMessage(r, SQ_SUCCEEDED(r) ? trackLabel(t.get()) + " color " + buf : std::string("set color failed: ") + sq_result_string(r));
    }, haveDoc);
    add("Track", "SetOutput", "Set Track Output", [this](IAttributes* a, IAttributes*) {
      auto t = track(a);
      if (SQ_FAILED(needTrack(t))) return SQ_E_NOTFOUND;
      sq_guid out{};
      std::string label;
      if (!busArg(a, &out, &label)) return setMessage(SQ_E_INVALIDARG, "bus=<bus/FX track or master> required");
      if (trackInfo(t.get()).kind == SQ_TRACK_MASTER) return setMessage(SQ_E_INVALIDARG, "the master track has no output to route");
      if (!sq_guid_is_null(&out) && reaches(out, nodeId(t.get())))
        return setMessage(SQ_E_INVALIDARG, "routing to " + label + " would create a feedback loop");
      const sq_result r = step("Track Output", [&] { return setTrackProp(t.get(), "output", [&](IAttributes* p) { p->setGuid("value", &out); }); });
      return setMessage(r, SQ_SUCCEEDED(r) ? trackLabel(t.get()) + " -> " + label
                                           : std::string("set output failed (feedback loop or not a bus): ") + sq_result_string(r));
    }, haveDoc);
    add("Track", "Move", "Move Track", [this](IAttributes* a, IAttributes*) {
      auto t = track(a);
      if (SQ_FAILED(needTrack(t))) return SQ_E_NOTFOUND;
      int64_t idx = 0;
      if (!argInt(a, "index", &idx) || idx < 0) return setMessage(SQ_E_INVALIDARG, "index=<0..> required");
      auto p = sq::makeAttributes();
      sq_guid g = nodeId(t.get());
      p->setGuid("node", &g);
      p->setInt("index", idx);
      const sq_result r = step("Move Track", [&] { return exec(edoc::kMoveTrack, p.get()); });
      return setMessage(r, SQ_SUCCEEDED(r) ? trackLabel(t.get()) + " moved to " + std::to_string(idx) : std::string("move failed: ") + sq_result_string(r));
    }, haveDoc);
    add("Track", "Select", "Select Track", [this](IAttributes* a, IAttributes*) {
      bool given = false;
      auto t = trackFromArg(a, "track", &given);
      if (!t) return setMessage(SQ_E_NOTFOUND, "no such track");
      selectTrack(nodeId(t.get()));
      return setMessage(SQ_OK, "selected " + trackLabel(t.get()));
    }, haveDoc);

    add("Track", "Info", "Track Info", [this](IAttributes* a, IAttributes* res) {
      auto t = track(a);
      if (SQ_FAILED(needTrack(t))) return SQ_E_NOTFOUND;
      const sq_track_info ti = trackInfo(t.get());
      static const char* const kinds[] = {"?", "audio", "instrument", "bus", "fx", "folder", "master"};
      const char* kind = ti.kind >= 1 && ti.kind <= 6 ? kinds[ti.kind] : "?";
      auto oneLine = [](const std::string& v) {
        std::string o;
        for (char c : v) o += c == '\n' ? std::string(" / ") : std::string(1, c);
        return o;
      };
      uint32_t notes = 0;
      for (uint32_t i = 0; i < ti.clip_count; ++i) {
        sq::Ptr<IClip> c;
        sq_clip_info ci{};
        ci.struct_size = sizeof ci;
        if (t->getClip(i, c.put()) == SQ_OK && c && c->getClipInfo(&ci) == SQ_OK) notes += ci.event_count;
      }
      std::string lanes;
      for (const auto& l : engine::audio::parseAutomation(trackStr(t.get(), "automation")))
        lanes += (lanes.empty() ? "" : ",") + l.target + "(" + std::to_string(l.points.size()) + ")";
      float peak = 0.0f;
      if (audio) {
        sq_meter_value mv[8];
        uint32_t n = 0;
        sq_guid g = nodeId(t.get());
        if (audio->getMeter(&g, mv, 8, &n) == SQ_OK)
          for (uint32_t c = 0; c < n; ++c) peak = std::max(peak, mv[c].peak);
      }
      char pk[32];
      std::snprintf(pk, sizeof pk, "%.3f", peak);
      const std::string msg = trackLabel(t.get()) + " kind=" + kind + " instrument=" + trackStr(t.get(), "user.instrument") +
                              " inserts=[" + oneLine(trackStr(t.get(), "user.inserts")) + "] sends=[" + oneLine(trackStr(t.get(), "user.sends")) +
                              "] automation=[" + lanes + "] clips=" + std::to_string(ti.clip_count) + " events=" + std::to_string(notes) +
                              " peak=" + pk;
      if (res) {
        sq_guid g = nodeId(t.get());
        res->setGuid("node", &g);
        res->setInt("clips", ti.clip_count);
        res->setDouble("peak", peak);
      }
      return setMessage(SQ_OK, msg);
    }, haveDoc);

    // ---- instrument ----
    add("Track", "SetInstrument", "Set Instrument", [this](IAttributes* a, IAttributes*) {
      auto t = track(a);
      if (SQ_FAILED(needTrack(t))) return SQ_E_NOTFOUND;
      if (trackInfo(t.get()).kind != SQ_TRACK_INSTRUMENT) return setMessage(SQ_E_INVALIDARG, trackLabel(t.get()) + " is not an instrument track");
      const std::string spec = str(a, "spec");
      if (!spec.empty() && !validInstrument(spec)) return setMessage(SQ_E_INVALIDARG, "malformed spec (<format>|<id>|<path>[|i=v]...)");
      const sq_result r = step("Set Instrument", [&] { return setTrackString(t.get(), "user.instrument", spec); });
      return setMessage(r, SQ_SUCCEEDED(r) ? trackLabel(t.get()) + " instrument " + (spec.empty() ? std::string("(default)") : spec)
                                           : std::string("set instrument failed: ") + sq_result_string(r));
    }, haveDoc);
    add("Track", "SetInstrumentParam", "Set Instrument Parameter", [this](IAttributes* a, IAttributes*) {
      auto t = track(a);
      if (SQ_FAILED(needTrack(t))) return SQ_E_NOTFOUND;
      if (trackInfo(t.get()).kind != SQ_TRACK_INSTRUMENT) return setMessage(SQ_E_INVALIDARG, trackLabel(t.get()) + " is not an instrument track");
      uint32_t param = 0;
      double value = 0;
      if (!argIndex(a, "param", &param) || !argNum(a, "value", &value)) return setMessage(SQ_E_INVALIDARG, "param=<0..2^32-1> value=<plain> required");
      std::string spec = trackStr(t.get(), "user.instrument");
      if (spec.empty()) spec = engine::audio::kDefaultInstrument;
      if (!validInstrument(spec)) return setMessage(SQ_E_INVALIDARG, trackLabel(t.get()) + " has a malformed instrument spec");
      const std::string next = engine::audio::specSetParam(spec, param, value);
      const bool merge = argBool(a, "merge", false);
      const sq_result r = step("Instrument Parameter", [&] { return setTrackString(t.get(), "user.instrument", next, merge); });
      return setMessage(r, SQ_SUCCEEDED(r) ? "instrument param " + std::to_string(param) + " = " + fmtNum(value) : std::string("failed: ") + sq_result_string(r));
    }, haveDoc);

    // ---- inserts ----
    add("Track", "AddInsert", "Add Insert", [this](IAttributes* a, IAttributes* res) {
      const std::string spec = str(a, "spec");
      if (!validSpec(spec)) return setMessage(SQ_E_INVALIDARG, "spec=<format>|<id>|<path>[|i=v]... required (one line)");
      int64_t slot = -1;
      return editInserts(a, "Add Insert", [&](LineList& lines, std::string& msg) {
        int64_t idx = static_cast<int64_t>(lines.size());
        if (a && a->getType("index") != SQ_VALUE_NONE && (!argInt(a, "index", &idx) || idx < 0 || idx > static_cast<int64_t>(lines.size())))
          return msg = "index=<0.." + std::to_string(lines.size()) + "> (whole number)", SQ_E_INVALIDARG;
        lines.insert(static_cast<size_t>(idx), spec);
        slot = idx;
        if (res) res->setInt("slot", slot);
        msg = "insert " + spec + " at slot " + std::to_string(idx);
        return SQ_OK;
      });
    }, haveDoc);
    add("Track", "RemoveInsert", "Remove Insert", [this](IAttributes* a, IAttributes*) {
      return editInserts(a, "Remove Insert", [&](LineList& lines, std::string& msg) {
        size_t s = 0;
        if (!slotArg(a, "slot", lines.size(), &s)) return msg = "slot=<0.." + std::to_string(lines.size()) + "> required", SQ_E_INVALIDARG;
        msg = "removed insert " + lines[s];
        lines.erase(s);
        return SQ_OK;
      });
    }, haveDoc);
    add("Track", "MoveInsert", "Move Insert", [this](IAttributes* a, IAttributes*) {
      return editInserts(a, "Move Insert", [&](LineList& lines, std::string& msg) {
        size_t s = 0, to = 0;
        if (!slotArg(a, "slot", lines.size(), &s) || !slotArg(a, "to", lines.size(), &to)) return msg = "slot= to= required", SQ_E_INVALIDARG;
        lines.move(s, to);
        msg = "moved insert " + std::to_string(s) + " -> " + std::to_string(to);
        return SQ_OK;
      });
    }, haveDoc);
    add("Track", "SetInsertParam", "Set Insert Parameter", [this](IAttributes* a, IAttributes*) {
      return editInserts(a, "Insert Parameter", [&](LineList& lines, std::string& msg) {
        size_t s = 0;
        uint32_t param = 0;
        double value = 0;
        if (!slotArg(a, "slot", lines.size(), &s) || !argIndex(a, "param", &param) || !argNum(a, "value", &value))
          return msg = "slot= param=<0..2^32-1> value= required", SQ_E_INVALIDARG;
        if (!validSpec(lines[s])) return msg = "insert " + std::to_string(s) + " has a malformed spec", SQ_E_INVALIDARG;
        const bool bypassed = engine::audio::specBypassed(lines[s]);
        lines[s] = (bypassed ? "!" : "") + engine::audio::specSetParam(engine::audio::specWithoutBypass(lines[s]), param, value);
        msg = "insert " + std::to_string(s) + " param " + std::to_string(param) + " = " + fmtNum(value);
        return SQ_OK;
      });
    }, haveDoc);
    add("Track", "BypassInsert", "Bypass Insert", [this](IAttributes* a, IAttributes*) {
      return editInserts(a, "Bypass Insert", [&](LineList& lines, std::string& msg) {
        size_t s = 0;
        if (!slotArg(a, "slot", lines.size(), &s)) return msg = "slot= required", SQ_E_INVALIDARG;
        const bool now = engine::audio::specBypassed(lines[s]);
        const bool want = argBool(a, "bypass", !now);
        lines[s] = (want ? "!" : "") + engine::audio::specWithoutBypass(lines[s]);
        msg = "insert " + std::to_string(s) + (want ? " bypassed" : " active");
        return SQ_OK;
      });
    }, haveDoc);

    // ---- sends ----
    add("Track", "AddSend", "Add Send", [this](IAttributes* a, IAttributes* res) {
      sq_guid bus{};
      std::string label;
      if (!busArg(a, &bus, &label) || sq_guid_is_null(&bus)) return setMessage(SQ_E_INVALIDARG, "bus=<bus/FX track> required");
      double level = 0;
      if (a && a->getType("level") != SQ_VALUE_NONE && (!argNum(a, "level", &level) || !validDb(level)))
        return setMessage(SQ_E_INVALIDARG, "level=<dB> (-144..24)");
      const bool pre = argBool(a, "pre", false);
      auto t = track(a);
      if (t && nodeId(t.get()) == bus) return setMessage(SQ_E_INVALIDARG, "a track cannot send to itself");
      if (t && trackInfo(t.get()).kind == SQ_TRACK_MASTER) return setMessage(SQ_E_INVALIDARG, "the master track cannot have sends");
      if (t && reaches(bus, nodeId(t.get()))) return setMessage(SQ_E_INVALIDARG, "send to " + label + " would create a feedback loop");
      return editSends(a, "Add Send", [&](LineList& lines, std::string& msg) {
        lines.push_back(sq::toString(bus) + " " + fmtNum(level) + (pre ? " pre" : ""));
        if (res) res->setInt("send", static_cast<int64_t>(lines.size() - 1));
        msg = "send " + std::to_string(lines.size() - 1) + " -> " + label + " " + fmtNum(level) + " dB" + (pre ? " pre" : "");
        return SQ_OK;
      });
    }, haveDoc);
    add("Track", "SetSendLevel", "Set Send Level", [this](IAttributes* a, IAttributes*) {
      return editSends(a, "Send Level", [&](LineList& lines, std::string& msg) {
        size_t s = 0;
        double level = 0;
        if (!slotArg(a, "send", lines.size(), &s) || !argNum(a, "level", &level) || !validDb(level))
          return msg = "send=<index> level=<dB> (-144..24) required", SQ_E_INVALIDARG;
        const size_t sp = lines[s].find(' ');
        const bool pre = lines[s].find(" pre") != std::string::npos;
        lines[s] = lines[s].substr(0, sp) + " " + fmtNum(level) + (pre ? " pre" : "");
        msg = "send " + std::to_string(s) + " level " + fmtNum(level) + " dB";
        return SQ_OK;
      });
    }, haveDoc);
    add("Track", "RemoveSend", "Remove Send", [this](IAttributes* a, IAttributes*) {
      return editSends(a, "Remove Send", [&](LineList& lines, std::string& msg) {
        size_t s = 0;
        if (!slotArg(a, "send", lines.size(), &s)) return msg = "send=<index> required", SQ_E_INVALIDARG;
        lines.erase(s);
        msg = "removed send " + std::to_string(s);
        return SQ_OK;
      });
    }, haveDoc);

    // ---- automation ----
    add("Automation", "SetLane", "Set Automation Lane", [this](IAttributes* a, IAttributes*) {
      auto t = track(a);
      if (SQ_FAILED(needTrack(t))) return SQ_E_NOTFOUND;
      const std::string target = str(a, "target");
      if (!validTarget(target)) return setMessage(SQ_E_INVALIDARG, "target= volume|pan|mute|send.<n>|insert.<slot>.<param>|instrument.<param>");
      std::vector<std::pair<double, double>> pts;
      if (!parsePoints(str(a, "points"), &pts) || pts.empty()) return setMessage(SQ_E_INVALIDARG, "points=\"ppq:value ppq:value ...\" required");
      for (auto& [p, v] : pts) {
        if (p < 0) return setMessage(SQ_E_INVALIDARG, "negative position");
        if (dbTarget(target) && !validDb(v)) return setMessage(SQ_E_INVALIDARG, target + " values are dB in -144..24");
      }
      const sq_result r = writeLane(t.get(), target, pts, "Edit Automation");
      return setMessage(r, SQ_SUCCEEDED(r) ? trackLabel(t.get()) + " " + target + ": " + std::to_string(pts.size()) + " point(s)"
                                           : std::string("automation failed: ") + sq_result_string(r));
    }, haveDoc);
    add("Automation", "AddPoint", "Add Automation Point", [this](IAttributes* a, IAttributes*) {
      auto t = track(a);
      if (SQ_FAILED(needTrack(t))) return SQ_E_NOTFOUND;
      const std::string target = str(a, "target");
      double ppq = 0, value = 0;
      if (!validTarget(target) || !argNum(a, "ppq", &ppq) || ppq < 0 || !argNum(a, "value", &value))
        return setMessage(SQ_E_INVALIDARG, "target= ppq= value= required");
      if (dbTarget(target) && !validDb(value)) return setMessage(SQ_E_INVALIDARG, target + " values are dB in -144..24");
      std::vector<std::pair<double, double>> pts;
      for (const auto& l : engine::audio::parseAutomation(trackStr(t.get(), "automation")))
        if (l.target == target) pts = l.points;
      bool replaced = false;
      for (auto& pv : pts)
        if (std::fabs(pv.first - ppq) < 1e-9) pv.second = value, replaced = true;
      if (!replaced) pts.emplace_back(ppq, value);
      std::stable_sort(pts.begin(), pts.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
      const sq_result r = writeLane(t.get(), target, pts, "Add Automation Point");
      return setMessage(r, SQ_SUCCEEDED(r) ? trackLabel(t.get()) + " " + target + ": " + std::to_string(pts.size()) + " point(s)"
                                           : std::string("automation failed: ") + sq_result_string(r));
    }, haveDoc);
    add("Automation", "RemoveLane", "Remove Automation Lane", [this](IAttributes* a, IAttributes*) {
      auto t = track(a);
      if (SQ_FAILED(needTrack(t))) return SQ_E_NOTFOUND;
      const std::string target = str(a, "target");
      if (target.empty()) return setMessage(SQ_E_INVALIDARG, "target= required");
      const sq_result r = writeLane(t.get(), target, {}, "Remove Automation");
      return setMessage(r, SQ_SUCCEEDED(r) ? "removed lane " + target : std::string("automation failed: ") + sq_result_string(r));
    }, haveDoc);

    // ---- clips ----
    add("Clip", "CreateMidi", "Create MIDI Clip", [this](IAttributes* a, IAttributes* res) {
      auto t = track(a);
      if (SQ_FAILED(needTrack(t))) return SQ_E_NOTFOUND;
      if (trackInfo(t.get()).kind != SQ_TRACK_INSTRUMENT) return setMessage(SQ_E_INVALIDARG, trackLabel(t.get()) + " is not an instrument track");
      double start = 0, length = 4;
      if (a && a->getType("start") != SQ_VALUE_NONE && (!argNum(a, "start", &start) || start < 0)) return setMessage(SQ_E_INVALIDARG, "start=<ppq>");
      if (a && a->getType("length") != SQ_VALUE_NONE && (!argNum(a, "length", &length) || length <= 0)) return setMessage(SQ_E_INVALIDARG, "length=<ppq>");
      auto p = sq::makeAttributes();
      sq_guid tid = nodeId(t.get());
      p->setGuid("track", &tid);
      p->setInt("kind", SQ_CLIP_MIDI);
      p->setDouble("start", start);
      p->setDouble("length", length);
      if (a && a->getType("name") == SQ_VALUE_STRING) p->setString("name", str(a, "name").c_str());
      auto out = sq::makeAttributes();
      sq_guid clip{};
      const sq_result r = step("Create MIDI Clip", [&] {
        sq_result x = exec(sq::doc::kAddClip, p.get(), out.get());
        if (SQ_SUCCEEDED(x)) out->getGuid("node", &clip);
        if (SQ_SUCCEEDED(x))
          if (auto* impl = activeImpl()) edit::setSelection(*impl, {clip});  // rides along with the step
        return x;
      });
      if (SQ_FAILED(r)) return setMessage(r, std::string("create clip failed: ") + sq_result_string(r));
      if (res) res->setGuid("node", &clip);
      return setMessage(SQ_OK, "MIDI clip " + sq::toString(clip) + " at " + fmtNum(start));
    }, haveDoc);
    add("Clip", "SetNotes", "Set Clip Notes", [this](IAttributes* a, IAttributes*) {
      auto c = clipArg(a);
      if (!c) return setMessage(SQ_E_NOTFOUND, "no such clip (clip= or a selected clip)");
      if (!a || a->getType("notes") != SQ_VALUE_STRING) return setMessage(SQ_E_INVALIDARG, "notes=\"pos,len,pitch,vel;...\" required (notes=\"\" clears)");
      std::vector<sq_clip_event> evs;
      const std::string notes = str(a, "notes");
      size_t p = 0;
      while (p < notes.size()) {
        size_t e = notes.find_first_of(";/", p);  // ';' or '/' between notes
        std::string one = notes.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? notes.size() : e + 1;
        if (one.find_first_not_of(" \t") == std::string::npos) continue;
        double f[4] = {0, 0, 60, 100};
        int n = 0;
        size_t q = 0;
        while (q <= one.size() && n < 4) {
          size_t comma = one.find(',', q);
          const std::string part = one.substr(q, comma == std::string::npos ? std::string::npos : comma - q);
          char* end = nullptr;
          const double v = std::strtod(part.c_str(), &end);
          if (!end || end == part.c_str() || !std::isfinite(v)) return setMessage(SQ_E_INVALIDARG, "notes=\"pos,len,pitch,vel;...\": bad '" + one + "'");
          f[n++] = v;
          if (comma == std::string::npos) break;
          q = comma + 1;
        }
        if (n < 3 || f[0] < 0 || f[1] <= 0 || f[2] < 0 || f[2] > 127 || f[3] < 1 || f[3] > 127)
          return setMessage(SQ_E_INVALIDARG, "notes=\"pos,len,pitch,vel;...\": bad '" + one + "'");
        sq_clip_event ev{};
        ev.struct_size = sizeof ev;
        ev.position = f[0];
        ev.length = f[1];
        ev.bytes[0] = 0x90;
        ev.bytes[1] = static_cast<uint8_t>(f[2]);
        ev.bytes[2] = static_cast<uint8_t>(f[3]);
        ev.size = 3;
        evs.push_back(ev);
      }
      auto q = sq::makeAttributes();
      sq_guid cid = nodeId(c.get());
      q->setGuid("clip", &cid);
      q->setBlob("events", evs.empty() ? nullptr : evs.data(), static_cast<uint32_t>(evs.size() * sizeof(sq_clip_event)));
      const sq_result r = step("Edit Notes", [&] { return exec(sq::doc::kSetClipEvents, q.get()); });
      return setMessage(r, SQ_SUCCEEDED(r) ? std::to_string(evs.size()) + " note(s)" : std::string("set notes failed: ") + sq_result_string(r));
    }, haveDoc);
    add("Clip", "Rename", "Rename Clip", [this](IAttributes* a, IAttributes*) {
      auto c = clipArg(a);
      if (!c) return setMessage(SQ_E_NOTFOUND, "no such clip");
      if (!a || a->getType("name") != SQ_VALUE_STRING) return setMessage(SQ_E_INVALIDARG, "name= required");
      const std::string name = str(a, "name");
      auto p = sq::makeAttributes();
      sq_guid g = nodeId(c.get());
      p->setGuid("node", &g);
      p->setString("key", "name");
      p->setString("value", name.c_str());
      const sq_result r = step("Rename Clip", [&] { return exec(sq::doc::kSetProperty, p.get()); });
      return setMessage(r, SQ_SUCCEEDED(r) ? "clip renamed to '" + name + "'" : std::string("rename failed: ") + sq_result_string(r));
    }, haveDoc);
    add("Clip", "SetColor", "Set Clip Color", [this](IAttributes* a, IAttributes*) {
      auto c = clipArg(a);
      if (!c) return setMessage(SQ_E_NOTFOUND, "no such clip");
      uint32_t color = 0;
      if (!argColor(a, &color)) return setMessage(SQ_E_INVALIDARG, "color=0xRRGGBBAA required");
      auto p = sq::makeAttributes();
      sq_guid g = nodeId(c.get());
      p->setGuid("node", &g);
      p->setString("key", "color");
      p->setInt("value", color);
      const sq_result r = step("Clip Color", [&] { return exec(sq::doc::kSetProperty, p.get()); });
      return setMessage(r, SQ_SUCCEEDED(r) ? "clip color set" : std::string("set color failed: ") + sq_result_string(r));
    }, haveDoc);

    // ---- song / transport ----
    add("Song", "SetTempo", "Set Tempo", [this](IAttributes* a, IAttributes*) {
      double bpm = 0, pos = 0;
      if (!argNum(a, "bpm", &bpm) || bpm < 1 || bpm > 999) return setMessage(SQ_E_INVALIDARG, "bpm=<1..999> required");
      if (a && a->getType("position") != SQ_VALUE_NONE && (!argNum(a, "position", &pos) || pos < 0)) return setMessage(SQ_E_INVALIDARG, "position=<ppq>");
      auto p = sq::makeAttributes();
      p->setDouble("position", pos);
      p->setDouble("bpm", bpm);
      const sq_result r = step("Set Tempo", [&] { return exec(sq::doc::kSetTempo, p.get()); });
      return setMessage(r, SQ_SUCCEEDED(r) ? "tempo " + fmtNum(bpm) + " bpm at " + fmtNum(pos) : std::string("set tempo failed: ") + sq_result_string(r));
    }, haveDoc);
    add("Song", "SetSignature", "Set Time Signature", [this](IAttributes* a, IAttributes*) {
      int64_t num = 0, den = 0;
      double pos = 0;
      if (!argInt(a, "num", &num) || !argInt(a, "den", &den)) return setMessage(SQ_E_INVALIDARG, "num= den= required");
      if (a && a->getType("position") != SQ_VALUE_NONE && (!argNum(a, "position", &pos) || pos < 0)) return setMessage(SQ_E_INVALIDARG, "position=<ppq>");
      auto p = sq::makeAttributes();
      p->setDouble("position", pos);
      p->setInt("numerator", num);
      p->setInt("denominator", den);
      const sq_result r = step("Set Time Signature", [&] { return exec(sq::doc::kSetSignature, p.get()); });
      return setMessage(r, SQ_SUCCEEDED(r) ? "signature " + std::to_string(num) + "/" + std::to_string(den) : std::string("set signature failed: ") + sq_result_string(r));
    }, haveDoc);
    add("Transport", "Locate", "Locate", [this](IAttributes* a, IAttributes*) {
      double ppq = 0;
      if (!audio) return SQ_E_UNSUPPORTED;
      if (!argNum(a, "ppq", &ppq) || ppq < 0) return setMessage(SQ_E_INVALIDARG, "ppq=<position> required");
      const sq_result r = audio->locate(ppq);
      return setMessage(r, "position " + fmtNum(ppq));
    });
    add("Transport", "SetLoop", "Set Loop", [this](IAttributes* a, IAttributes*) {
      double s = 0, e = 0;
      if (!audio) return SQ_E_UNSUPPORTED;
      if (!argNum(a, "start", &s) || !argNum(a, "end", &e) || s < 0) return setMessage(SQ_E_INVALIDARG, "start= end= required");
      const bool on = argBool(a, "enabled", true);
      const sq_result r = audio->setLoop(s, e, on ? SQ_TRUE : SQ_FALSE);
      return setMessage(r, SQ_SUCCEEDED(r) ? "loop " + fmtNum(s) + ".." + fmtNum(e) + (on ? " on" : " off") : std::string("set loop failed: ") + sq_result_string(r));
    });
    add("Transport", "Info", "Transport Info", [this](IAttributes*, IAttributes* res) {
      if (!audio) return SQ_E_UNSUPPORTED;
      sq_transport_state ts{};
      ts.struct_size = sizeof ts;
      audio->getTransportState(&ts);
      double s = ts.loop_start, e = ts.loop_end;
      bool on = (ts.flags & SQ_TRANSPORT_LOOPING) != 0;
      const bool saved = readLoop(activeImpl(), &s, &e, &on);  // main-thread truth (the snapshot lags a block)
      sq_engine_status st{};
      st.struct_size = sizeof st;
      audio->getStatus(&st);
      const bool running = (st.flags & SQ_ENGINE_RUNNING) != 0, playing = (ts.flags & SQ_TRANSPORT_PLAYING) != 0;
      if (res) {
        res->setDouble("loop_start", s);
        res->setDouble("loop_end", e);
        res->setBool("looping", on ? SQ_TRUE : SQ_FALSE);
        res->setBool("playing", playing ? SQ_TRUE : SQ_FALSE);
        res->setDouble("position", ts.position);
        res->setDouble("seconds", ts.seconds);
        res->setBool("engine_running", running ? SQ_TRUE : SQ_FALSE);
        res->setInt("xruns", static_cast<int64_t>(st.xruns));
      }
      char rate[64] = "";
      if (running) std::snprintf(rate, sizeof rate, " %.0f Hz/%u", st.sample_rate, st.block_frames);
      return setMessage(SQ_OK, std::string(playing ? "playing" : "stopped") + " at " + fmtNum(ts.seconds) + " s, engine " +
                                   (running ? std::string("running on '") + st.device_id + "'" + rate + ", xruns " + std::to_string(st.xruns)
                                            : std::string("stopped")) +
                                   ", loop " + fmtNum(s) + ".." + fmtNum(e) + (on ? " on" : " off") + (saved ? " (song)" : ""));
    });
    add("Clip", "AddAudio", "Add Audio Clip", [this](IAttributes* a, IAttributes* res) {
      auto t = track(a);
      if (SQ_FAILED(needTrack(t))) return SQ_E_NOTFOUND;
      if (trackInfo(t.get()).kind != SQ_TRACK_AUDIO) return setMessage(SQ_E_INVALIDARG, trackLabel(t.get()) + " is not an audio track");
      const std::string path = str(a, "path");
      if (path.empty()) return setMessage(SQ_E_INVALIDARG, "path=<audio file> required");
      double start = 0, length = 4;
      if (a && a->getType("start") != SQ_VALUE_NONE && (!argNum(a, "start", &start) || start < 0)) return setMessage(SQ_E_INVALIDARG, "start=<ppq>");
      if (a && a->getType("length") != SQ_VALUE_NONE && (!argNum(a, "length", &length) || length <= 0)) return setMessage(SQ_E_INVALIDARG, "length=<ppq>");
      auto p = sq::makeAttributes();
      sq_guid tid = nodeId(t.get());
      p->setGuid("track", &tid);
      p->setInt("kind", SQ_CLIP_AUDIO);
      p->setDouble("start", start);
      p->setDouble("length", length);
      p->setString("source", absolutePath(path).c_str());
      auto out = sq::makeAttributes();
      sq_guid clip{};
      const sq_result r = step("Add Audio Clip", [&] {
        sq_result x = exec(sq::doc::kAddClip, p.get(), out.get());
        if (SQ_SUCCEEDED(x)) out->getGuid("node", &clip);
        return x;
      });
      if (SQ_FAILED(r)) return setMessage(r, std::string("add clip failed: ") + sq_result_string(r));
      if (res) res->setGuid("node", &clip);
      return setMessage(SQ_OK, "audio clip " + sq::toString(clip) + " at " + fmtNum(start) + " <- " + fileNameOf(path) +
                                   (sq_fs_is_file(path.c_str()) ? "" : " (file missing)"));
    }, haveDoc);
    add("Transport", "Wait", "Wait", [this](IAttributes* a, IAttributes*) {
      double ms = 0;
      if (!argNum(a, "ms", &ms) || ms < 0 || ms > 600000) return setMessage(SQ_E_INVALIDARG, "ms=<0..600000> required");
      const uint64_t until = sq_time_monotonic_ns() + static_cast<uint64_t>(ms * 1e6);
      while (sq_time_monotonic_ns() < until) {
        sh.idle();
        sq_thread_sleep_ms(5);
      }
      return SQ_OK;
    });

    // ---- MIDI ----
    add("Midi", "ListInputs", "List MIDI Inputs", [this](IAttributes*, IAttributes* res) {
      loadMidiDrivers();
      std::string lines;
      int64_t n = 0;
      for (auto& drv : midiDrivers) {
        drv->refreshPorts();
        for (uint32_t k = 0; k < drv->getPortCount(); ++k) {
          sq_midi_port_info pi{};
          pi.struct_size = sizeof pi;
          if (drv->getPortInfo(k, &pi) != SQ_OK || !(pi.flags & SQ_MIDI_PORT_INPUT)) continue;
          const std::string id = fixed(pi.id, sizeof pi.id);
          lines += id + "\t" + fixed(pi.name, sizeof pi.name) + "\t" + (midiInputs.count(id) ? "open" : "closed") + "\n";
          ++n;
        }
      }
      if (res) {
        res->setInt("count", n);
        res->setString("inputs", lines.c_str());
      }
      return setMessage(SQ_OK, std::to_string(n) + " MIDI input(s)" + (midiDrivers.empty() ? " (no MIDI driver loaded)" : ""));
    });
    add("Midi", "OpenInputs", "Open MIDI Inputs", [this](IAttributes*, IAttributes* res) {
      const size_t n = openMidiInputs();
      if (res) res->setInt("count", static_cast<int64_t>(n));
      return setMessage(SQ_OK, std::to_string(n) + " MIDI input(s) open");
    });
    add("Midi", "Inject", "Inject MIDI", [this](IAttributes* a, IAttributes*) {
      if (!audio) return SQ_E_UNSUPPORTED;
      // bytes="90,60,100": the status byte is hex (90 = note-on ch 1), data bytes decimal; a
      // "0x" prefix forces hex on any byte. Several messages: "90,60,100,80,60,0" (or ';').
      std::string text;
      if (a && a->getType("bytes") == SQ_VALUE_STRING) text = str(a, "bytes");
      else if (a && a->getType("bytes") != SQ_VALUE_NONE) text = fmtNum(a->getDouble("bytes", 0));
      std::vector<uint8_t> bytes;
      size_t p = 0;
      while (p < text.size()) {
        size_t e = text.find_first_of(",; ", p);
        std::string tok = text.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? text.size() : e + 1;
        if (tok.empty()) continue;
        char* end = nullptr;
        unsigned long v = 0;
        const bool hex = tok.size() > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X');
        // a status position: the first byte, or after a complete message
        bool statusPos = bytes.empty();
        if (!statusPos) {
          size_t i = 0;
          while (i < bytes.size()) {
            const size_t len = midiMessageLength(bytes[i]);
            if (i + len > bytes.size()) break;
            i += len;
          }
          statusPos = i == bytes.size();
        }
        v = std::strtoul(hex ? tok.c_str() + 2 : tok.c_str(), &end, (hex || statusPos) ? 16 : 10);
        // status bytes have the high bit set; data bytes (note, velocity, ...) are 0..127
        if (!end || *end != '\0' || v > 255 || (statusPos ? v < 0x80 || v == 0xF0 || v == 0xF7 : v > 0x7F))
          return setMessage(SQ_E_INVALIDARG, "bytes=\"90,60,100\" (status hex, data decimal 0..127, no sysex): bad '" + tok + "'");
        bytes.push_back(static_cast<uint8_t>(v));
      }
      if (bytes.empty()) return setMessage(SQ_E_INVALIDARG, "bytes= required");
      const bool ok = audio->pushLiveMidi(bytes.data(), static_cast<uint32_t>(bytes.size()));
      const auto targets = audio->liveMidiTracks();
      return setMessage(ok ? SQ_OK : SQ_E_BUSY, std::to_string(bytes.size()) + " byte(s) -> " + std::to_string(targets.size()) + " live MIDI track(s)");
    });
  }
#endif

  // ---- browser (IContentIndex from sqlite-index) ---------------------------------------------
  sq_result ensureIndex() {
    if (index) return SQ_OK;
    index = sq::createInstance<IContentIndex>(&sh.registry, sq::index::kSqliteIndexClassId);
    if (!index) return SQ_E_UNSUPPORTED;
    indexPath = sh.settings.get("Media.Browser.Index");
    if (indexPath.empty()) indexPath = sq::joinPath(sq::specialDir(SQ_DIR_USER_CONFIG), "content.tags.db");
    sq_result r = index->open(indexPath.c_str(), SQ_INDEX_CREATE);
    if (SQ_FAILED(r)) index = nullptr;
    return r;
  }
  static const char* kindOf(const std::string& ext) {
    static const std::map<std::string, const char*> k = {{"wav", "audio"}, {"aif", "audio"}, {"aiff", "audio"}, {"flac", "audio"},
                                                         {"mp3", "audio"}, {"ogg", "audio"}, {"m4a", "audio"}, {"mid", "midi"},
                                                         {"midi", "midi"}, {"preset", "preset"}, {"song", "project"}, {"dawproject", "project"}};
    auto it = k.find(ext);
    return it == k.end() ? nullptr : it->second;
  }
  sq_result indexFolder(IAttributes* a, IAttributes* res) {
    std::string folder = str(a, "folder");
    if (folder.empty()) return SQ_E_INVALIDARG;
    sq_result r = ensureIndex();
    if (SQ_FAILED(r)) return setMessage(r, "no content index (sqlite-index not loaded)");
    uint64_t n = 0;
    index->beginBatch();
    std::function<void(const std::string&, const std::string&, int)> walk = [&](const std::string& dir, const std::string& tags, int depth) {
      if (depth > 16) return;
      std::vector<std::pair<std::string, bool>> entries;
      sq_fs_list_dir(
          dir.c_str(),
          [](void* u, const char* name, sq_bool isDir) -> sq_bool {
            static_cast<std::vector<std::pair<std::string, bool>>*>(u)->emplace_back(name, isDir != 0);
            return SQ_TRUE;
          },
          &entries);
      for (const auto& [name, isDir] : entries) {
        std::string full = sq::joinPath(dir, name);
        if (isDir) {
          walk(full, tags.empty() ? name : tags + ";" + name, depth + 1);
          continue;
        }
        const char* kind = kindOf(lowerExt(name));
        if (!kind) continue;
        std::string title = name.substr(0, name.find_last_of('.'));
        int64_t mt = 0;
        uint64_t sz = 0;
        sq_fs_mtime(full.c_str(), &mt);
        sq_fs_file_size(full.c_str(), &sz);
        sq_index_item it{};
        it.struct_size = sizeof it;
        it.url = full.c_str();
        it.kind = kind;
        it.title = title.c_str();
        it.tags = tags.c_str();
        it.mtime = mt / 1000;
        it.size = static_cast<int64_t>(sz);
        if (SQ_SUCCEEDED(index->putItem(&it, nullptr, nullptr))) ++n;
      }
    };
    walk(folder, "", 0);
    r = index->commitBatch();
    if (res) res->setInt("count", static_cast<int64_t>(n));
    return setMessage(r, "indexed " + std::to_string(n) + " item(s)");
  }
  sq_result search(IAttributes* a, IAttributes* res) {
    sq_result r = ensureIndex();
    if (SQ_FAILED(r)) return setMessage(r, "no content index (sqlite-index not loaded)");
    std::string text = str(a, "text"), tags = str(a, "tags"), kind = str(a, "kind");
    sq_index_query q{};
    q.struct_size = sizeof q;
    q.text = text.c_str();
    q.tags_all = tags.empty() ? nullptr : tags.c_str();
    q.kind = kind.empty() ? nullptr : kind.c_str();
    q.sort = SQ_INDEX_SORT_RELEVANCE;
    q.limit = static_cast<uint32_t>(a ? a->getInt("limit", 50) : 50);
    sq::Ptr<IIndexResult> rs;
    r = index->query(&q, rs.put());
    if (SQ_FAILED(r)) return r;
    if (res) {
      res->setInt("count", static_cast<int64_t>(rs->getTotalCount()));
      sq_index_row row{};
      row.struct_size = sizeof row;
      if (rs->getCount() > 0 && rs->getRow(0, &row) == SQ_OK && row.url) res->setString("first", row.url);
    }
    return SQ_OK;
  }

  // ---- script commands -----------------------------------------------------------------------
  struct ScriptCmd {
    std::string category, name, title, classId;
  };
  std::vector<ScriptCmd> scriptCatalog() {
    std::vector<ScriptCmd> out;
    sq::Ptr<IObjectRegistry> objs;
    if (sh.host.queryService(&IObjectRegistry::iid, objs.putVoid()) != SQ_OK || !objs) return out;
    sq::Ptr<IController> c;
    if (objs->resolve("object://scripting", &IController::iid, c.putVoid()) != SQ_OK || !c) return out;
    auto v = sq::makeAttributes();
    if (c->getProperty("commands", v.get()) != SQ_OK) return out;
    std::string all = sq::getAttrString(v.get(), "value");
    size_t p = 0;
    while (p < all.size()) {
      size_t e = all.find('\n', p);
      std::string line = all.substr(p, e == std::string::npos ? std::string::npos : e - p);
      p = e == std::string::npos ? all.size() : e + 1;
      std::vector<std::string> f;
      size_t q = 0;
      while (q <= line.size()) {
        size_t t = line.find('\t', q);
        f.push_back(line.substr(q, t == std::string::npos ? std::string::npos : t - q));
        if (t == std::string::npos) break;
        q = t + 1;
      }
      if (f.size() >= 4) out.push_back(ScriptCmd{f[0], f[1], f[2], f[3]});
    }
    return out;
  }
  sq::Ptr<ICommandHandler> handler(const std::string& classId) {
    if (auto it = handlerCache.find(classId); it != handlerCache.end()) return it->second;
    sq_guid g{};
    if (!sq::parseGuid(classId, g)) return {};
    auto h = sq::createInstance<ICommandHandler>(&sh.registry, g);
    if (h) handlerCache[classId] = h;
    return h;
  }
};

// =================================================================================================

Shell::Shell() : impl_(std::make_unique<Impl>(*this)) {}

Shell::~Shell() { stop(); }

bool Shell::hasEngine() const {
#if SQ_SHELL_ENGINE
  return true;
#else
  return false;
#endif
}

void Shell::registerDefaults(Settings& s) {
  s.setDefault("Engine.SampleRate", SQ_SETTING_INT, "48000");
  s.setDefault("Engine.BlockSize", SQ_SETTING_INT, "256");
  s.setDefault("Engine.LowLatencyMonitoring", SQ_SETTING_BOOL, "true");
  s.setDefault("Engine.Autosave.Dir", SQ_SETTING_STRING, "");
  s.setDefault("Engine.Autosave.IntervalMs", SQ_SETTING_INT, "60000");
  s.setDefault("Devices.Audio.Driver", SQ_SETTING_STRING, "");
  s.setDefault("Devices.Audio.Device", SQ_SETTING_STRING, "");
  s.setDefault("Devices.Midi.Enabled", SQ_SETTING_BOOL, "true");
  s.setDefault("Devices.Midi.LiveInputHeadless", SQ_SETTING_BOOL, "false");
  s.setDefault("Media.Cache.MaxMB", SQ_SETTING_INT, "2048");
  s.setDefault("Media.RecordFormat", SQ_SETTING_STRING, "wav");
  s.setDefault("Platform.Log.Level", SQ_SETTING_STRING, "info");
  s.setDefault("UI.Theme", SQ_SETTING_STRING, "dark");
  // OSC is unauthenticated UDP: loopback only until the user opts into LAN control.
  s.setDefault("Services.hardware-remote.OscBindAddress", SQ_SETTING_STRING, "127.0.0.1");
}

sq_result Shell::start(const ShellOptions& opt) {
  if (started_) return SQ_OK;
  started_ = true;
  registerDefaults(settings);
  servicesDir_ = opt.servicesDir.empty() ? sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services") : opt.servicesDir;
  if (opt.loadServices) {
    auto res = loader.scan(servicesDir_);
    sq_log_printf(SQ_LOG_INFO, kTag, "services: %d loaded, %d rejected from %s", res.loaded, res.rejected, servicesDir_.c_str());
  }
#if SQ_SHELL_ENGINE
  // Document services after the modules: scripting-js registers IObjectRegistry in its INIT, and
  // engine/doc adds its object://doc/ resolver only if that registry already exists.
  sq_result r = edoc::registerDocumentServices(&host, &impl_->docs);
  if (SQ_FAILED(r)) sq_log_printf(SQ_LOG_ERROR, kTag, "document services: %s", sq_result_string(r));
  r = edit::registerEditServices(&host);
  if (SQ_FAILED(r)) sq_log_printf(SQ_LOG_ERROR, kTag, "edit services: %s", sq_result_string(r));
  impl_->nullDriver = SQ_SUCCEEDED(engine::audio::registerNullDriver(&registry));
  impl_->audio = sq::Ptr<engine::audio::AudioEngine>::adopt(new engine::audio::AudioEngine(&registry, &settings));
  impl_->audio->setInsertResolver([this](const std::string& spec, std::string* err) { return impl_->resolveInsert(spec, err); });
  impl_->audio->setInstrumentResolver([this](const std::string& spec, std::string* err) { return impl_->resolveInstrument(spec, err); });
  r = impl_->audio->registerService(&host);
  if (SQ_FAILED(r)) sq_log_printf(SQ_LOG_ERROR, kTag, "audio engine service: %s", sq_result_string(r));
  // Keep the engine bound to whatever document becomes active (also when a script switches it).
  impl_->docObserver = sq::Ptr<IObserver>::adopt(new FnObserver([this](const char* topic, IAttributes*) {
    if (std::strcmp(topic, "document.activated") == 0 || std::strcmp(topic, "document.closed") == 0) impl_->bindEngine();
  }));
  if (impl_->docs) impl_->docs->addObserver(impl_->docObserver.get());
  // Loop changes from anywhere (commands, the ruler, scripts) are saved in the active song.
  impl_->engineObserver = sq::Ptr<IObserver>::adopt(new FnObserver([this](const char* topic, IAttributes* args) {
    if (std::strcmp(topic, "transport.loop") == 0) impl_->onLoopChanged(args);
  }));
  impl_->audio->addObserver(impl_->engineObserver.get());
  std::string asDir = settings.get("Engine.Autosave.Dir");
  if (!asDir.empty() && impl_->docs) impl_->docs->setAutosave(asDir, settings.getInt("Engine.Autosave.IntervalMs", 60000));
  std::string drv = !opt.audioDriver.empty() ? opt.audioDriver : settings.get("Devices.Audio.Driver");
#if defined(__linux__)
  const char* const platformDriver = "alsa";
#else
  const char* const platformDriver = "wasapi";  // Windows (macOS: no native driver yet)
#endif
  const bool defaulted = drv.empty();
  if (drv.empty()) drv = opt.gui ? platformDriver : "none";
  if (drv != "none") {
    sq_result dr = openAudioDevice(drv);
#if defined(__linux__)
    if (SQ_FAILED(dr) && opt.gui && defaulted) {  // Linux default chain: alsa, then pulse, then null
      sq_log_printf(SQ_LOG_WARN, kTag, "audio driver 'alsa' unavailable (%s); trying 'pulse'", sq_result_string(dr));
      dr = openAudioDevice(drv = "pulse");
    }
#else
    (void)defaulted;
#endif
    if (SQ_FAILED(dr) && opt.gui && drv != "null") {
      sq_log_printf(SQ_LOG_WARN, kTag, "audio device '%s' unavailable (%s); using the null device", drv.c_str(), sq_result_string(dr));
      openAudioDevice("null");
    }
  }
  // Live MIDI: every input of the platform MIDI drivers feeds AudioEngine::pushLiveMidi. Opened at
  // startup in the window (Devices.Midi.Enabled); batch modes open them with Midi.OpenInputs or
  // Devices.Midi.LiveInputHeadless=true (inputs may be exclusive: control surfaces need them too).
  if (settings.getBool("Devices.Midi.Enabled", SQ_TRUE) && (opt.gui || settings.getBool("Devices.Midi.LiveInputHeadless", SQ_FALSE)))
    impl_->openMidiInputs();
#else
  (void)opt;
  sq_log_printf(SQ_LOG_WARN, kTag, "engine units not in this build (filtered): documents/audio unavailable");
#endif
  impl_->startUi();
  impl_->registerBuiltins();
  return SQ_OK;
}

void Shell::stop() {
  if (!started_) return;
  started_ = false;
  impl_->handlerCache.clear();
  if (impl_->index) {
    impl_->index->close();
    impl_->index = nullptr;
  }
  impl_->builtins.clear();
  impl_->stopOsc();
  impl_->stopUi();  // before the modules unload: UiServices holds scripting's IObjectRegistry
#if SQ_SHELL_ENGINE
  if (impl_->docs && impl_->docObserver) impl_->docs->removeObserver(impl_->docObserver.get());
  impl_->docObserver = nullptr;
  impl_->closeMidiInputs();  // before the engine goes (the inbox holds a raw engine pointer)
  impl_->midiInbox = nullptr;
  impl_->sampleCache.clear();
  if (impl_->audio && impl_->engineObserver) impl_->audio->removeObserver(impl_->engineObserver.get());
  impl_->engineObserver = nullptr;
  if (impl_->audio) {
    impl_->audio->setDocument(nullptr);
    impl_->audio->closeDevice();
    impl_->audio->idle();
    impl_->audio->setInsertResolver({});
    impl_->audio->setInstrumentResolver({});
    impl_->audio->unregisterService(&host);
    impl_->audio = nullptr;
  }
  if (impl_->docs) {
    while (impl_->docs->getDocumentCount() > 0) {
      sq::Ptr<IDocument> d;
      if (impl_->docs->getDocument(0, d.put()) != SQ_OK || SQ_FAILED(impl_->docs->closeDocument(d.get()))) break;
    }
  }
  edit::unregisterEditServices(&host);
  edoc::unregisterDocumentServices(&host);
  impl_->docs = nullptr;
  if (impl_->nullDriver) engine::audio::unregisterNullDriver(&registry);
  impl_->nullDriver = false;
#endif
  loader.unloadAll(true);
  host.clearServices();
}

void Shell::idle() {
#if SQ_SHELL_ENGINE
  if (impl_->audio) impl_->audio->idle();
  impl_->syncLiveTarget();  // live MIDI follows the selected track
  impl_->pumpMidi();
  if (impl_->docs) impl_->docs->tick(static_cast<int64_t>(sq_time_monotonic_ns() / 1000000ull));
#endif
  if (auto se = scripts()) se->idle();
  impl_->pumpSurfaces();  // object:// writes, LED/motor feedback, MIDI hot-plug reconnects
  impl_->drainOsc();
}

IDocumentManager* Shell::documents() const {
#if SQ_SHELL_ENGINE
  return impl_->docs;
#else
  return nullptr;
#endif
}
sq::Ptr<IDocument> Shell::activeDocument() const { return impl_->active(); }
IAudioEngine* Shell::engine() const {
#if SQ_SHELL_ENGINE
  return impl_->audio.get();
#else
  return nullptr;
#endif
}
engine::audio::AudioEngine* Shell::audio() const {
#if SQ_SHELL_ENGINE
  return impl_->audio.get();
#else
  return nullptr;
#endif
}
sq::Ptr<IScriptEngine> Shell::scripts() const {
  sq::Ptr<IScriptEngine> s;
  const_cast<HostContext&>(host).queryService(&IScriptEngine::iid, s.putVoid());
  return s;
}
ui::ThemeResolver* Shell::themes() const { return impl_->themes.get(); }
sq::Ptr<IPluginRegistry> Shell::plugins() const {
  sq::Ptr<IPluginRegistry> p;
  const_cast<HostContext&>(host).queryService(&IPluginRegistry::iid, p.putVoid());
  return p;
}

sq_result Shell::openAudioDevice(const std::string& driverIn) {
#if SQ_SHELL_ENGINE
  if (!impl_->audio) return SQ_E_UNSUPPORTED;
  std::string drv = driverIn.empty() ? settings.get("Devices.Audio.Driver", "null") : driverIn;
  std::string dev = settings.get("Devices.Audio.Device");
  sq_guid cls{};
  if (drv == "null" || drv == "offline") {
    cls = engine::audio::kNullDriverClassId;
    if (dev.empty()) dev = drv;
  } else if (drv == "wasapi") {
    cls = sq::audiodevice::kWasapiDriverClassId;
    if (dev.empty()) dev = "wasapi:default";
  } else if (drv == "asio") {
    cls = sq::audiodevice::kAsioDriverClassId;
  } else if (drv == "alsa" || drv == "pulse") {
    // Linux driver services: found by their driver name ("ALSA", "Pulse"), case-insensitively.
    bool found = false;
    for (uint32_t i = 0; i < registry.getClassCount() && !found; ++i) {
      sq_class_info ci{};
      ci.struct_size = sizeof ci;
      if (registry.getClassInfo(i, &ci) != SQ_OK || !ci.category || std::strcmp(ci.category, sq::audiodevice::kCategory) != 0) continue;
      auto d = sq::createInstance<IAudioDriver>(&registry, ci.class_id);
      sq_audio_driver_info di{};
      di.struct_size = sizeof di;
      if (!d || d->getDriverInfo(&di) != SQ_OK) continue;
      std::string n(di.name, ::strnlen(di.name, sizeof di.name));
      for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (n == drv) {
        cls = ci.class_id;
        found = true;
      }
    }
    if (!found) {
      message_ = "audio driver '" + drv + "' not available (service not loaded)";
      return SQ_E_NOTFOUND;
    }
    if (dev.empty() && drv == "pulse") dev = "pulse:default";
  } else {
    message_ = "unknown audio driver '" + drv + "'";
    return SQ_E_INVALIDARG;
  }
  sq_result r = impl_->audio->openDevice(&cls, dev.empty() ? nullptr : dev.c_str(), nullptr);
  message_ = SQ_SUCCEEDED(r) ? "audio: " + deviceDescription() : "audio device " + drv + ": " + sq_result_string(r);
  sq_log_printf(SQ_SUCCEEDED(r) ? SQ_LOG_INFO : SQ_LOG_WARN, kTag, "%s", message_.c_str());
  return r;
#else
  (void)driverIn;
  return SQ_E_UNSUPPORTED;
#endif
}

std::string Shell::deviceDescription() const {
#if SQ_SHELL_ENGINE
  if (!impl_->audio) return "no engine";
  sq_engine_status st{};
  st.struct_size = sizeof st;
  impl_->audio->getStatus(&st);
  if (!(st.flags & SQ_ENGINE_DEVICE_OPEN)) return "no audio device";
  return std::string(st.device_id) + " @ " + std::to_string(static_cast<int>(st.sample_rate)) + " Hz / " + std::to_string(st.block_frames) +
         " frames";
#else
  return "no engine";
#endif
}

std::vector<CommandInfo> Shell::commands() {
  std::vector<CommandInfo> out;
  for (const auto& b : impl_->builtins) out.push_back(CommandInfo{b.category, b.name, b.title, "app"});
#if SQ_SHELL_ENGINE
  for (const auto& c : edit::commandTable()) out.push_back(CommandInfo{c.category, c.name, c.title, "edit"});
#endif
  for (const auto& s : impl_->scriptCatalog()) out.push_back(CommandInfo{s.category, s.name, s.title, "script"});
  return out;
}

sq_result Shell::execute(std::string_view dotted, IAttributes* args, IAttributes* result) {
  size_t dot = dotted.find('.');
  if (dot == std::string_view::npos) return SQ_E_INVALIDARG;
  return execute(dotted.substr(0, dot), dotted.substr(dot + 1), args, result);
}

sq_result Shell::execute(std::string_view category, std::string_view name, IAttributes* args, IAttributes* result) {
  message_.clear();
  for (auto& b : impl_->builtins)
    if (b.category == category && b.name == name) {
      if (b.enabled && !b.enabled()) return SQ_COMMAND_DISABLED;
      // Built-ins run arbitrary C++ (file import/export, parsing user data): an exception must
      // become an error result, never terminate the process.
      try {
        return b.run(args, result);
      } catch (const std::bad_alloc&) {
        message_ = "out of memory";
        return SQ_E_OUTOFMEMORY;
      } catch (const std::exception& e) {
        message_ = std::string("internal error: ") + e.what();
        return SQ_E_FAIL;
      } catch (...) {
        message_ = "internal error";
        return SQ_E_FAIL;
      }
    }
  std::string cat(category), nm(name);
#if SQ_SHELL_ENGINE
  if (const edit::CommandInfo* ci = edit::findCommand(cat.c_str(), nm.c_str())) {
    auto d = activeDocument();
    if (!d) return SQ_E_NOTFOUND;
    return edit::runCommand(d.get(), ci->id, args, false);
  }
#endif
  sq_command cmd{sizeof(sq_command), cat.c_str(), nm.c_str(), 0};
  for (const auto& s : impl_->scriptCatalog()) {
    if (s.category != cat || s.name != nm) continue;
    auto h = impl_->handler(s.classId);
    if (!h) continue;
    sq_result r = h->interpretCommand(&cmd, args, SQ_FALSE);
    if (SQ_FAILED(r) && r != SQ_E_NOTFOUND) {
      if (auto se = scripts()) {
        char buf[2048];
        uint32_t need = 0;
        if (se->getLastError(buf, sizeof buf, &need) == SQ_OK) message_ = buf;
      }
    }
    if (r != SQ_E_NOTFOUND) return r;
  }
  // Native "Command" classes from other modules, by class name "Category.Name".
  const std::string dotted = cat + "." + nm;
  for (uint32_t i = 0; i < registry.getClassCount(); ++i) {
    sq_class_info ci{};
    ci.struct_size = sizeof ci;
    if (registry.getClassInfo(i, &ci) != SQ_OK || !ci.category || std::strcmp(ci.category, scripting::kCommandCategory) != 0) continue;
    if (!ci.name || dotted != ci.name) continue;
    auto h = impl_->handler(sq::toString(ci.class_id));
    if (!h) continue;
    sq_result r = h->interpretCommand(&cmd, args, SQ_FALSE);
    if (r != SQ_E_NOTFOUND) return r;
  }
  return SQ_E_NOTFOUND;
}

sq_result Shell::isEnabled(std::string_view category, std::string_view name, IAttributes* args) {
  for (auto& b : impl_->builtins)
    if (b.category == category && b.name == name) return !b.enabled || b.enabled() ? SQ_OK : SQ_COMMAND_DISABLED;
  std::string cat(category), nm(name);
#if SQ_SHELL_ENGINE
  if (const edit::CommandInfo* ci = edit::findCommand(cat.c_str(), nm.c_str())) {
    auto d = activeDocument();
    return d ? edit::runCommand(d.get(), ci->id, args, true) : SQ_COMMAND_DISABLED;
  }
#endif
  sq_command cmd{sizeof(sq_command), cat.c_str(), nm.c_str(), 0};
  for (const auto& s : impl_->scriptCatalog()) {
    if (s.category != cat || s.name != nm) continue;
    if (auto h = impl_->handler(s.classId)) {
      sq_result r = h->interpretCommand(&cmd, args, SQ_TRUE);
      if (r != SQ_E_NOTFOUND) return r;
    }
  }
  return SQ_E_NOTFOUND;
}

}  // namespace sq::app
