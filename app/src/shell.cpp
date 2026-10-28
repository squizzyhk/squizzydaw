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
#include "sq/engine/audio/devices.hpp"
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
  // Track by args "track" (guid) / "track_index" (int); default: the last track.
  sq::Ptr<ITrack> track(IAttributes* args) {
    sq::Ptr<ITrack> t;
    auto d = active();
    if (!d) return t;
    sq_document_info di{};
    di.struct_size = sizeof di;
    d->getDocumentInfo(&di);
    if (args && args->getType("track") == SQ_VALUE_GUID) {
      sq_guid g{};
      args->getGuid("track", &g);
      sq::Ptr<INode> n;
      if (d->findNode(&g, n.put()) == SQ_OK && n) t = n.as<ITrack>();
      return t;
    }
    int64_t idx = args && args->getType("track_index") != SQ_VALUE_NONE ? args->getInt("track_index", -1) : static_cast<int64_t>(di.track_count) - 1;
    if (idx >= 0 && idx < di.track_count) d->getTrack(static_cast<uint32_t>(idx), t.put());
    return t;
  }
  static sq_guid nodeId(INode* n) {
    sq_guid g{};
    if (n) n->getId(&g);
    return g;
  }
  void bindEngine() {
#if SQ_SHELL_ENGINE
    if (audio) audio->setDocument(active().get());
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
    add("File", "Open", "Open...", [this](IAttributes* a, IAttributes*) {
      std::string path = str(a, "path");
      if (path.empty() || !docs) return SQ_E_INVALIDARG;
      sq::Ptr<IDocument> d;
      sq_result r = docs->openDocument(path.c_str(), d.put());
      if (SQ_FAILED(r)) return setMessage(r, "cannot open " + path + ": " + sq_result_string(r));
      docs->setActiveDocument(d.get());
      bindEngine();
      return setMessage(SQ_OK, "opened " + path);
    });
    add("File", "Save", "Save", [this](IAttributes* a, IAttributes*) {
      auto d = active();
      if (!d || !docs) return SQ_E_NOTFOUND;
      std::string path = str(a, "path");
      sq_result r = docs->saveDocument(d.get(), path.empty() ? nullptr : path.c_str());
      return setMessage(r, SQ_SUCCEEDED(r) ? "saved" : std::string("save failed: ") + sq_result_string(r));
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
      return setMessage(r, SQ_SUCCEEDED(r) ? "exported " + std::to_string(rep.tracks) + " track(s) to " + path : err);
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
      return setMessage(SQ_OK, "imported " + path);
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
      double s = num(a, "start", ts.loop_start), e = num(a, "end", ts.loop_end > ts.loop_start ? ts.loop_end : s + 16);
      return audio->setLoop(s, e, (ts.flags & SQ_TRANSPORT_LOOPING) ? SQ_FALSE : SQ_TRUE);
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
    add("Track", "AddAudio", "Add Audio Track", [this](IAttributes* a, IAttributes* res) { return addTrack(a, res, SQ_TRACK_AUDIO); }, haveDoc);
    add("Track", "AddInstrument", "Add Instrument Track", [this](IAttributes* a, IAttributes* res) { return addTrack(a, res, SQ_TRACK_INSTRUMENT); }, haveDoc);
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
    add("Track", "SetVolume", "Set Track Volume", [this](IAttributes* a, IAttributes*) {
      auto t = track(a);
      if (!t || !a) return SQ_E_INVALIDARG;
      const double db = num(a, "value", 0.0);
      if (db < -96.0 || db > 24.0) return SQ_E_INVALIDARG;
      return setTrackProp(t.get(), "volumeDb", [&](IAttributes* p) { p->setDouble("value", db); });
    }, haveDoc);
    add("Track", "SetPan", "Set Track Pan", [this](IAttributes* a, IAttributes*) {
      auto t = track(a);
      if (!t || !a) return SQ_E_INVALIDARG;
      const double pan = num(a, "value", 0.0);
      if (pan < -1.0 || pan > 1.0) return SQ_E_INVALIDARG;
      return setTrackProp(t.get(), "pan", [&](IAttributes* p) { p->setDouble("value", pan); });
    }, haveDoc);
    add("Track", "ToggleMute", "Mute Track", [this](IAttributes* a, IAttributes*) {
      auto t = track(a);
      if (!t) return SQ_E_NOTFOUND;
      sq_track_info ti{};
      ti.struct_size = sizeof ti;
      t->getTrackInfo(&ti);
      return setTrackProp(t.get(), "mute", [&](IAttributes* p) { p->setBool("value", (ti.flags & SQ_TRACK_MUTE) ? SQ_FALSE : SQ_TRUE); });
    }, haveDoc);
    add("Track", "ToggleSolo", "Solo Track", [this](IAttributes* a, IAttributes*) {
      auto t = track(a);
      if (!t) return SQ_E_NOTFOUND;
      sq_track_info ti{};
      ti.struct_size = sizeof ti;
      t->getTrackInfo(&ti);
      return setTrackProp(t.get(), "solo", [&](IAttributes* p) { p->setBool("value", (ti.flags & SQ_TRACK_SOLO) ? SQ_FALSE : SQ_TRUE); });
    }, haveDoc);
    add("Track", "Rename", "Rename Track", [this](IAttributes* a, IAttributes*) {
      auto t = track(a);
      if (!t || !a) return SQ_E_INVALIDARG;
      const std::string name = str(a, "name");
      if (name.empty()) return SQ_E_INVALIDARG;
      return setTrackProp(t.get(), "name", [&](IAttributes* p) { p->setString("value", name.c_str()); });
    }, haveDoc);
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
      static const std::set<std::string> known = {"none", "null", "offline", "wasapi", "asio"};
      if (!known.count(drv)) return setMessage(SQ_E_INVALIDARG, "driver must be none|null|offline|wasapi|asio");
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

  sq_result addTrack(IAttributes* a, IAttributes* res, int32_t kind) {
    auto p = sq::makeAttributes();
    p->setInt("kind", kind);
    std::string name = str(a, "name", kind == SQ_TRACK_AUDIO ? "Audio" : "Instrument");
    p->setString("name", name.c_str());
    if (a && a->getType("channels") != SQ_VALUE_NONE) p->setInt("channels", a->getInt("channels", 2));
    return exec(sq::doc::kAddTrack, p.get(), res);
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
    rq.tail_seconds = num(a, "tail", 0.0);
    if (rq.tail_seconds > 0) rq.flags |= SQ_RENDER_TAIL;
    rq.output = out.c_str();
    rq.file_extension = ext.c_str();
    sq::Ptr<IRenderTask> task;
    sq_result r = audio->startRender(&rq, nullptr, task.put());
    if (SQ_FAILED(r)) return setMessage(r, std::string("render failed to start: ") + sq_result_string(r));
    r = task->wait(600000);
    audio->idle();  // main thread: release the finished render's graph (plugin instances) now
    if (SQ_SUCCEEDED(r)) r = task->getResult();
    if (res && SQ_SUCCEEDED(r)) res->setInt("outputs", task->getOutputCount());
    return setMessage(r, SQ_SUCCEEDED(r) ? "exported " + out : std::string("export failed: ") + sq_result_string(r));
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

  engine::audio::ProcessorProto resolveInsert(const std::string& spec, std::string* err) {
    std::vector<std::string> parts;
    size_t p = 0;
    while (p <= spec.size()) {
      size_t e = spec.find('|', p);
      parts.push_back(spec.substr(p, e == std::string::npos ? std::string::npos : e - p));
      if (e == std::string::npos) break;
      p = e + 1;
    }
    if (parts.size() < 3 || parts[0].empty() || parts[1].empty()) {
      if (err) *err = "malformed insert spec";
      return nullptr;
    }
    std::vector<std::pair<uint32_t, double>> params;
    for (size_t i = 3; i < parts.size(); ++i) {
      size_t eq = parts[i].find('=');
      if (eq == std::string::npos) continue;
      params.emplace_back(static_cast<uint32_t>(std::strtoul(parts[i].substr(0, eq).c_str(), nullptr, 10)),
                          std::strtod(parts[i].substr(eq + 1).c_str(), nullptr));
    }
    sq_plugin_desc desc{};
    desc.struct_size = sizeof desc;
    auto cp = [](char* dst, size_t cap, const std::string& s) {
      size_t n = std::min(cap - 1, s.size());
      std::memcpy(dst, s.data(), n);
      dst[n] = 0;
    };
    cp(desc.format, sizeof desc.format, parts[0]);
    cp(desc.id, sizeof desc.id, parts[1]);
    cp(desc.path, sizeof desc.path, parts[2]);
    cp(desc.name, sizeof desc.name, parts[1]);
    desc.status = SQ_PLUGIN_STATUS_OK;
    IHostContext* hostCtx = &sh.host;
    IClassRegistry* creg = &sh.registry;
    auto factory = [desc, params, hostCtx, creg](std::string* e) -> sq::Ptr<IPluginInstance> {
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
    return std::make_shared<engine::audio::PluginFx>(factory, desc.name);
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
  r = impl_->audio->registerService(&host);
  if (SQ_FAILED(r)) sq_log_printf(SQ_LOG_ERROR, kTag, "audio engine service: %s", sq_result_string(r));
  // Keep the engine bound to whatever document becomes active (also when a script switches it).
  impl_->docObserver = sq::Ptr<IObserver>::adopt(new FnObserver([this](const char* topic, IAttributes*) {
    if (std::strcmp(topic, "document.activated") == 0 || std::strcmp(topic, "document.closed") == 0) impl_->bindEngine();
  }));
  if (impl_->docs) impl_->docs->addObserver(impl_->docObserver.get());
  std::string asDir = settings.get("Engine.Autosave.Dir");
  if (!asDir.empty() && impl_->docs) impl_->docs->setAutosave(asDir, settings.getInt("Engine.Autosave.IntervalMs", 60000));
  std::string drv = !opt.audioDriver.empty() ? opt.audioDriver : settings.get("Devices.Audio.Driver");
  if (drv.empty()) drv = opt.gui ? "wasapi" : "none";
  if (drv != "none") {
    sq_result dr = openAudioDevice(drv);
    if (SQ_FAILED(dr) && opt.gui && drv != "null") {
      sq_log_printf(SQ_LOG_WARN, kTag, "audio device '%s' unavailable (%s); using the null device", drv.c_str(), sq_result_string(dr));
      openAudioDevice("null");
    }
  }
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
  if (impl_->audio) {
    impl_->audio->setDocument(nullptr);
    impl_->audio->closeDevice();
    impl_->audio->idle();
    impl_->audio->setInsertResolver({});
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
