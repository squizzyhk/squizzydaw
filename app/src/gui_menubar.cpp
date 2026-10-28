// Squizzy Studio GUI - menu bar, main menus and global keyboard shortcuts.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "gui_panels.hpp"
#include "sq/interfaces/audiodevice.h"
#include "sq/platform.h"

namespace sq::app::gui {
namespace {

MenuItem cmd(GuiContext& ctx, const std::string& label, const std::string& command, const std::string& shortcut = {}) {
  MenuItem m;
  m.label = label;
  m.shortcut = shortcut;
  m.action = [&ctx, command] { ctx.run(command); };
  return m;
}
MenuItem fn(const std::string& label, std::function<void()> f, const std::string& shortcut = {}, bool checked = false) {
  MenuItem m;
  m.label = label;
  m.shortcut = shortcut;
  m.action = std::move(f);
  m.checked = checked;
  return m;
}
// Edit menu entry: the focused panel (piano roll notes, arrange clips) first, else `fallback`.
MenuItem editItem(GuiContext& ctx, const std::string& label, const char* editCmd, const char* fallback, const std::string& shortcut = {}) {
  MenuItem m;
  m.label = label;
  m.shortcut = shortcut;
  m.action = [&ctx, editCmd, fallback] { runEditCommand(ctx, editCmd, fallback); };
  return m;
}

std::string songPath(GuiContext& ctx) {
  auto d = ctx.doc();
  char buf[2048] = {};
  uint32_t need = 0;
  if (d && d->getPath(buf, sizeof buf, &need) == SQ_OK && buf[0]) return buf;
  return {};
}
std::string songName(GuiContext& ctx) {
  const std::string p = songPath(ctx);
  return p.empty() ? std::string("Untitled") : p.substr(p.find_last_of("/\\") + 1);
}
bool songModified(GuiContext& ctx) {
  auto d = ctx.doc();
  sq_document_info di{};
  di.struct_size = sizeof di;
  return d && d->getDocumentInfo(&di) == SQ_OK && di.modified;
}

// Runs a command that makes another song active (New, Open, Import DAWproject) and closes the song
// it replaced, so no hidden documents pile up behind the visible one.
void replaceSong(GuiContext& ctx, const char* command, IAttributes* args = nullptr) {
  sq::Ptr<IDocument> old = ctx.doc();
  const sq_result r = ctx.run(command, args);
  sq::Ptr<IDocument> now = ctx.doc();
  if (SQ_SUCCEEDED(r) && old && now && old.get() != now.get())
    if (IDocumentManager* m = ctx.shell().documents()) m->closeDocument(old.get());
}

void openSong(GuiContext& ctx) {
  guardUnsavedChanges(ctx, [&ctx] {
    ctx.chooseFile(false, "Open Song", {"song", "dawproject"}, {}, [&ctx](const std::string& path) {
      auto a = sq::makeAttributes();
      a->setString("path", path.c_str());
      const bool daw = path.size() > 11 && path.substr(path.size() - 11) == ".dawproject";
      replaceSong(ctx, daw ? "File.ImportDawProject" : "File.Open", a.get());
    });
  });
}

void newSong(GuiContext& ctx) {
  guardUnsavedChanges(ctx, [&ctx] { replaceSong(ctx, "File.New"); });
}

void closeSong(GuiContext& ctx) {
  guardUnsavedChanges(ctx, [&ctx] { ctx.run("File.Close"); });  // the GUI then starts a new empty song
}

// then (optional) runs after a successful save.
void saveSongAs(GuiContext& ctx, std::function<void()> then = {}) {
  const std::string cur = songPath(ctx);
  ctx.chooseFile(true, "Save Song As", {"song"}, cur.empty() ? std::string("Untitled.song") : cur, [&ctx, then](const std::string& path) {
    auto a = sq::makeAttributes();
    a->setString("path", path.c_str());
    if (SQ_SUCCEEDED(ctx.run("File.Save", a.get())) && then) then();
  });
}

void saveSong(GuiContext& ctx, std::function<void()> then = {}) {
  if (songPath(ctx).empty()) {
    saveSongAs(ctx, std::move(then));
    return;
  }
  if (SQ_SUCCEEDED(ctx.run("File.Save")) && then) then();
}

void exportAudio(GuiContext& ctx) {
  ctx.chooseFile(true, "Export Mixdown", {"wav", "flac", "ogg", "mp3"}, "mixdown.wav", [&ctx](const std::string& path) {
    auto a = sq::makeAttributes();
    a->setString("path", path.c_str());
    ctx.run("File.ExportAudio", a.get());
  });
}

void exportStems(GuiContext& ctx) {
  chooseFolder(ctx, "Export Stems To Folder", {}, [&ctx](const std::string& dir) {
    if (!sq_fs_is_dir(dir.c_str()) && sq_fs_create_dirs(dir.c_str()) != SQ_OK) {
      ctx.setStatus("cannot create folder " + dir);
      return;
    }
    auto a = sq::makeAttributes();
    a->setString("dir", dir.c_str());  // File.ExportStems takes the folder as "dir"
    ctx.run("File.ExportStems", a.get());
  });
}

void importAudio(GuiContext& ctx) {
  ctx.chooseFile(false, "Import Audio or MIDI File", {"wav", "flac", "ogg", "mp3", "aif", "aiff", "m4a", "mid", "midi"}, {},
                 [&ctx](const std::string& path) {
                   // The arrange view imports at the edit cursor on the selected track (real file length).
                   Widget* arr = ctx.findWidget("arrange");
                   if (!arr || !arrangeDropFile(arr, -1.0f, -1.0f, path)) ctx.setStatus("could not import " + path);
                 });
}

// ---- audio device state (Options menu) -----------------------------------------------------------
struct RunningAudio {
  std::string driver;  // "alsa" "pulse" "null" "wasapi" "asio" ... or "none"
  std::string device;
  int rate = 0;
  int block = 0;
};

RunningAudio runningAudio(Shell& sh) {
  RunningAudio ra;
  ra.driver = "none";
  IAudioEngine* e = sh.engine();
  sq_engine_status st{};
  st.struct_size = sizeof st;
  if (!e || e->getStatus(&st) != SQ_OK || !(st.flags & SQ_ENGINE_DEVICE_OPEN)) return ra;
  ra.device.assign(st.device_id, ::strnlen(st.device_id, sizeof st.device_id));
  ra.rate = static_cast<int>(std::lround(st.sample_rate));
  ra.block = static_cast<int>(st.block_frames);
  // The driver of the running device: its class name in the registry ("Null", "ALSA", ...).
  for (uint32_t i = 0; i < sh.registry.getClassCount(); ++i) {
    sq_class_info ci{};
    ci.struct_size = sizeof ci;
    if (sh.registry.getClassInfo(i, &ci) != SQ_OK || !(ci.class_id == st.driver_class)) continue;
    sq::Ptr<IAudioDriver> d;
    if (sh.registry.createInstance(&ci.class_id, &IAudioDriver::iid, d.putVoid()) != SQ_OK || !d) break;
    sq_audio_driver_info di{};
    di.struct_size = sizeof di;
    if (d->getDriverInfo(&di) != SQ_OK) break;
    std::string n(di.name, ::strnlen(di.name, sizeof di.name));
    for (char& ch : n) ch = static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
    ra.driver = n;
    return ra;
  }
  const std::string stored = sh.settings.get("Devices.Audio.Driver");
  ra.driver = stored.empty() ? std::string("unknown") : stored;
  return ra;
}

// Reopens the running device so a new sample rate / buffer size takes effect.
void reopenAudio(GuiContext& ctx) {
  Shell& sh = ctx.shell();
  const RunningAudio ra = runningAudio(sh);
  if (ra.driver == "none" || ra.driver == "unknown") {
    ctx.setStatus("saved - applies when an audio device is opened (Options > Audio Driver)");
    return;
  }
  // Reopen exactly what runs now (it may be a --audio-driver override of the stored choice).
  const std::string oldDev = sh.settings.get("Devices.Audio.Device");
  const bool stored = sh.settings.get("Devices.Audio.Driver") == ra.driver;
  if (!stored) sh.settings.setString("Devices.Audio.Device", ra.device.c_str());
  const sq_result r = sh.openAudioDevice(ra.driver);
  if (!stored) sh.settings.setString("Devices.Audio.Device", oldDev.c_str());
  ctx.setStatus(SQ_SUCCEEDED(r) ? "audio: " + sh.deviceDescription() : "could not reopen the audio device: " + std::string(sq_result_string(r)));
}

}  // namespace

void guardUnsavedChanges(GuiContext& ctx, std::function<void()> proceed) {
  if (!songModified(ctx)) {
    if (proceed) proceed();
    return;
  }
  askChoice(ctx, "Save changes to \"" + songName(ctx) + "\" first?", {"Save", "Don't Save", "Cancel"}, [&ctx, proceed](int pick) {
    if (pick == 0) saveSong(ctx, proceed);
    else if (pick == 1 && proceed) proceed();
  });
}

void runEditCommand(GuiContext& ctx, std::string_view editCmd, const char* fallback) {
  if (ctx.editFocused(editCmd)) return;
  if (fallback) {
    ctx.run(fallback);
    return;
  }
  ctx.setStatus(std::string(editCmd) + ": select notes in the editor (or clips in the arrange) first");
}


std::vector<MenuItem> buildMainMenu(GuiContext& ctx, const std::string& title) {
  std::vector<MenuItem> m;
  if (title == "File") {
    m.push_back(fn("New Song", [&ctx] { newSong(ctx); }, "Ctrl+N"));
    m.push_back(fn("Open...", [&ctx] { openSong(ctx); }, "Ctrl+O"));
    m.push_back(fn("Save", [&ctx] { saveSong(ctx); }, "Ctrl+S"));
    m.push_back(fn("Save As...", [&ctx] { saveSongAs(ctx); }, "Ctrl+Shift+S"));
    m.push_back(MenuItem::sep());
    m.push_back(fn("Import Audio File...", [&ctx] { importAudio(ctx); }, "Ctrl+I"));
    m.push_back(fn("Export Mixdown...", [&ctx] { exportAudio(ctx); }, "Ctrl+E"));
    m.push_back(fn("Export Stems...", [&ctx] { exportStems(ctx); }));
    m.push_back(fn("Export MIDI File...", [&ctx] {
      ctx.chooseFile(true, "Export MIDI File", {"mid"}, "song.mid", [&ctx](const std::string& path) {
        auto a = sq::makeAttributes();
        a->setString("path", path.c_str());
        ctx.run("File.ExportMidi", a.get());
      });
    }));
    m.push_back(fn("Export DAWproject...", [&ctx] {
      ctx.chooseFile(true, "Export DAWproject", {"dawproject"}, "song.dawproject", [&ctx](const std::string& path) {
        auto a = sq::makeAttributes();
        a->setString("path", path.c_str());
        ctx.run("File.ExportDawProject", a.get());
      });
    }));
    m.push_back(MenuItem::sep());
    m.push_back(fn("Close Song", [&ctx] { closeSong(ctx); }));
  } else if (title == "Edit") {
    m.push_back(cmd(ctx, "Undo", "Edit.Undo", "Ctrl+Z"));
    m.push_back(cmd(ctx, "Redo", "Edit.Redo", "Ctrl+Y"));
    m.push_back(MenuItem::sep());
    m.push_back(editItem(ctx, "Cut", "Cut", nullptr, "Ctrl+X"));
    m.push_back(editItem(ctx, "Copy", "Copy", nullptr, "Ctrl+C"));
    m.push_back(editItem(ctx, "Paste", "Paste", nullptr, "Ctrl+V"));
    m.push_back(editItem(ctx, "Duplicate", "Duplicate", "Edit.Duplicate", "Ctrl+D"));
    m.push_back(editItem(ctx, "Delete", "Delete", "Edit.Delete", "Del"));
    m.push_back(MenuItem::sep());
    m.push_back(cmd(ctx, "Split at Cursor", "Edit.Split", "S"));
    m.push_back(cmd(ctx, "Glue", "Edit.Glue", "G"));
    m.push_back(editItem(ctx, "Quantize", "Quantize", "Edit.Quantize", "Q"));
    m.push_back(MenuItem::sep());
    m.push_back(editItem(ctx, "Select All", "SelectAll", "Navigate.SelectAll", "Ctrl+A"));
    m.push_back(editItem(ctx, "Select None", "SelectNone", "Navigate.SelectNone"));
    m.push_back(MenuItem::sep());
    MenuItem clip;
    clip.label = "Clip";
    clip.submenu = {cmd(ctx, "Fade In to Cursor", "Edit.FadeInToCursor"), cmd(ctx, "Fade Out from Cursor", "Edit.FadeOutFromCursor"),
                    cmd(ctx, "Crossfade", "Edit.Crossfade"),             cmd(ctx, "Normalize", "Edit.Normalize"),
                    cmd(ctx, "Gain +1 dB", "Edit.GainUp"),               cmd(ctx, "Gain -1 dB", "Edit.GainDown"),
                    cmd(ctx, "Trim Start to Cursor", "Edit.TrimStartToCursor"), cmd(ctx, "Trim End to Cursor", "Edit.TrimEndToCursor"),
                    cmd(ctx, "Strip Silence", "Edit.StripSilence")};
    m.push_back(clip);
    MenuItem time;
    time.label = "Time";
    time.submenu = {cmd(ctx, "Insert Time", "Edit.InsertTime"), cmd(ctx, "Delete Time", "Edit.DeleteTime"),
                    cmd(ctx, "Ripple Delete", "Edit.DeleteRipple"), cmd(ctx, "Ripple Delete (All Tracks)", "Edit.DeleteRippleAll")};
    m.push_back(time);
  } else if (title == "Track") {
    m.push_back(cmd(ctx, "Add Audio Track", "Track.AddAudio", "Ctrl+T"));
    m.push_back(cmd(ctx, "Add Instrument Track", "Track.AddInstrument", "Ctrl+Shift+T"));
    m.push_back(cmd(ctx, "Add Drum Track", "Track.AddDrums"));
    m.push_back(cmd(ctx, "Add Bus / FX Track", "Track.AddBus"));
    m.push_back(MenuItem::sep());
    m.push_back(fn("Rename Track...", [&ctx] {
      TrackView t;
      const sq_guid id = ctx.selectedTrack();
      if (!ctx.findTrack(id, &t)) return;
      ctx.prompt("Rename Track", t.name, [&ctx, id](const std::string& name) {
        auto a = sq::makeAttributes();
        a->setGuid("track", &id);
        a->setString("name", name.c_str());
        if (SQ_FAILED(ctx.run("Track.Rename", a.get()))) ctx.setPropertyString(id, "name", name);
      });
    }, "F2"));
    m.push_back(cmd(ctx, "Arm for Recording", "Track.ToggleArm"));
    m.push_back(cmd(ctx, "Delete Track", "Track.Remove"));
  } else if (title == "Transport") {
    const auto ts = ctx.transport();
    m.push_back(cmd(ctx, (ts.flags & SQ_TRANSPORT_PLAYING) ? "Stop" : "Play", (ts.flags & SQ_TRANSPORT_PLAYING) ? "Transport.Stop" : "Transport.Play", "Space"));
    m.push_back(fn("Record", [&ctx] { toggleRecord(ctx); }, "R"));
    m.push_back(cmd(ctx, "Go to Start", "Transport.Rewind", "Home"));
    m.push_back(MenuItem::sep());
    MenuItem loop = cmd(ctx, "Loop", "Transport.ToggleLoop", "L");
    loop.checked = (ts.flags & SQ_TRANSPORT_LOOPING) != 0;
    m.push_back(loop);
    MenuItem met = cmd(ctx, "Metronome", "Transport.ToggleMetronome", "C");
    met.checked = (ts.flags & SQ_TRANSPORT_METRONOME) != 0;
    m.push_back(met);
    m.push_back(MenuItem::sep());
    m.push_back(fn("Set Tempo...", [&ctx] {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%.2f", ctx.transport().bpm);
      ctx.prompt("Tempo (BPM)", buf, [&ctx](const std::string& v) {
        const double bpm = std::atof(v.c_str());
        if (!(bpm >= kMinBpm && bpm <= kMaxBpm)) {
          ctx.setStatus("tempo must be between " + formatBpm(kMinBpm) + " and " + formatBpm(kMaxBpm) + " BPM");
          return;
        }
        auto a = sq::makeAttributes();
        a->setDouble("bpm", bpm);
        bool ok = SQ_SUCCEEDED(ctx.shell().execute("Song.SetTempo", a.get()));
        if (!ok) {
          auto p = sq::makeAttributes();
          p->setDouble("position", 0.0);
          p->setDouble("bpm", bpm);
          ok = SQ_SUCCEEDED(ctx.exec(sq::doc::kSetTempo, p.get()));
        }
        ctx.setStatus(ok ? "Tempo: " + formatBpm(bpm) + " BPM" : std::string("could not set the tempo"));
      });
    }));
  } else if (title == "View") {
    m.push_back(fn("Mixer", [&ctx] { ctx.showBottomPanel(BottomPanel::Mixer); }, "F3",
                   ctx.bottomPanelVisible() && ctx.bottomPanel() == BottomPanel::Mixer));
    m.push_back(fn("Editor", [&ctx] { ctx.showBottomPanel(BottomPanel::Editor); }, "F4",
                   ctx.bottomPanelVisible() && ctx.bottomPanel() == BottomPanel::Editor));
    m.push_back(fn("Devices", [&ctx] { ctx.showBottomPanel(BottomPanel::Devices); }, "F6",
                   ctx.bottomPanelVisible() && ctx.bottomPanel() == BottomPanel::Devices));
    m.push_back(fn("Browser", [&ctx] { ctx.setBrowserVisible(!ctx.browserVisible()); }, "F7", ctx.browserVisible()));
    m.push_back(MenuItem::sep());
    MenuItem theme;
    theme.label = "Theme";
    for (const char* t : {"dark", "light"}) {
      MenuItem it;
      it.label = std::string(1, static_cast<char>(t[0] - 32)) + (t + 1);
      it.action = [&ctx, t] {
        auto a = sq::makeAttributes();
        a->setString("name", t);
        ctx.run("UI.SetTheme", a.get());
      };
      theme.submenu.push_back(it);
    }
    m.push_back(theme);
  } else if (title == "Options") {
    Shell& sh = ctx.shell();
    const RunningAudio ra = runningAudio(sh);
    // What runs now (also a --audio-driver override); the stored choice only when nothing is open.
    std::string curDrv = ra.driver;
    if (curDrv == "none" || curDrv == "unknown") {
      curDrv = sh.settings.get("Devices.Audio.Driver");
      if (curDrv.empty()) curDrv = "none";
    }
    const std::string curDev = ra.driver == "none" ? sh.settings.get("Devices.Audio.Device") : ra.device;
    auto select = [&ctx](const std::string& drv, const std::string& dev) {
      auto a = sq::makeAttributes();
      a->setString("driver", drv.c_str());
      a->setString("device", dev.c_str());
      if (SQ_SUCCEEDED(ctx.run("Audio.SelectDevice", a.get())) && drv != "none") ctx.setStatus("audio: " + ctx.shell().deviceDescription());
    };
    MenuItem drivers;
    drivers.label = "Audio Driver";
#ifdef _WIN32
    const char* kDrivers[] = {"wasapi", "asio", "null", "none"};
#elif defined(__linux__)
    const char* kDrivers[] = {"alsa", "pulse", "null", "none"};
#else
    const char* kDrivers[] = {"null", "none"};
#endif
    for (const char* d : kDrivers) {
      MenuItem it;
      it.label = d;
      it.checked = curDrv == d;
      const std::string ds = d;
      it.action = [select, ds] { select(ds, std::string()); };
      drivers.submenu.push_back(it);
    }
    m.push_back(drivers);
    MenuItem devices;
    devices.label = "Audio Device";
    {
      auto a = sq::makeAttributes();
      auto res = sq::makeAttributes();
      a->setString("driver", curDrv.c_str());
      if (curDrv != "none" && curDrv != "null" && SQ_SUCCEEDED(sh.execute("Audio", "ListDevices", a.get(), res.get()))) {
        const std::string lines = sq::getAttrString(res.get(), "devices");
        size_t pos = 0;
        while (pos < lines.size()) {
          size_t nl = lines.find('\n', pos);
          std::string line = lines.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
          pos = nl == std::string::npos ? lines.size() : nl + 1;
          const size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
          if (t2 == std::string::npos) continue;
          const std::string id = line.substr(t1 + 1, t2 - t1 - 1), devName = line.substr(t2 + 1);
          MenuItem it;
          it.label = devName.empty() ? id : devName;
          it.checked = id == curDev;
          it.action = [select, curDrv, id] { select(curDrv, id); };
          devices.submenu.push_back(it);
        }
      }
      if (devices.submenu.empty()) {
        MenuItem none;
        none.label = curDrv == "none" ? "(choose a driver first)" : curDrv == "null" ? "(null: no hardware)" : "(no devices found)";
        none.enabled = false;
        devices.submenu.push_back(none);
      }
    }
    m.push_back(devices);
    // Sample rate / buffer size: ticks show the running device; a change is stored (Devices.*,
    // read by the engine when it opens a device) and the running device is reopened.
    auto settingMenu = [&](const char* label, const char* devKey, const char* engineKey, std::vector<int> values, const char* suffix, int cur) {
      MenuItem mi;
      mi.label = label;
      if (cur <= 0) cur = static_cast<int>(sh.settings.getInt(devKey, sh.settings.getInt(engineKey, 0)));
      for (int v : values) {
        MenuItem it;
        it.label = std::to_string(v) + suffix;
        it.checked = cur == v;
        const std::string dk = devKey, ek = engineKey;
        it.action = [&ctx, dk, ek, v] {
          ctx.shell().settings.setInt(dk.c_str(), v);
          ctx.shell().settings.setInt(ek.c_str(), v);
          reopenAudio(ctx);
        };
        mi.submenu.push_back(it);
      }
      return mi;
    };
    m.push_back(settingMenu("Sample Rate", "Devices.SampleRate", "Engine.SampleRate", {44100, 48000, 88200, 96000}, " Hz", ra.rate));
    m.push_back(settingMenu("Buffer Size", "Devices.BlockSize", "Engine.BlockSize", {32, 64, 128, 256, 512, 1024, 2048}, " samples", ra.block));
    MenuItem llm;
    llm.label = "Low-Latency Input Monitoring";
    llm.checked = sh.settings.getBool("Engine.LowLatencyMonitoring", SQ_TRUE) != 0;
    llm.action = [&ctx] {
      auto& st = ctx.shell().settings;
      st.setBool("Engine.LowLatencyMonitoring", st.getBool("Engine.LowLatencyMonitoring", SQ_TRUE) ? SQ_FALSE : SQ_TRUE);
    };
    m.push_back(llm);
    m.push_back(MenuItem::sep());
    m.push_back(fn("MIDI Inputs...", [&ctx] { ctx.run("Midi.ListInputs"); }));
    m.push_back(fn("Audio Status", [&ctx] { ctx.run("Audio.ListDevices"); }));
  } else if (title == "Help") {
    m.push_back(fn("Keyboard Shortcuts", [&ctx] {
      ctx.confirm("Space play/stop, R record, L loop, C metronome, S split, Q quantize, Del delete, Ctrl+Z/Y undo/redo, "
                  "Ctrl+D duplicate, F3 mixer, F4 editor, F6 devices, F7 browser.",
                  [] {});
    }));
    m.push_back(fn("About Squizzy Studio", [&ctx] { ctx.confirm("Squizzy Studio - a recording-first DAW.", [] {}); }));
  }
  return m;
}

namespace {

class MenuBar final : public Widget {
 public:
  explicit MenuBar(GuiContext& ctx) : Widget("MenuBar"), ctx_(ctx) {}
  void paint(Canvas& c) override {
    const Palette& p = ctx_.palette();
    const Rect& f = frame();
    c.fillRect(f, p.panel);
    c.fillRect(Rect{f.x, f.bottom() - 1, f.w, 1}, p.border);
    c.drawTextBold("Squizzy Studio", Rect{f.x + 12, f.y, 120, f.h}, p.accent, 12.5f);
    float x = f.x + 128;
    titles_.clear();
    for (const char* t : kTitles) {
      const float w = c.measureText(t, 12.5f) + 20;
      Rect r{x, f.y + 2, w, f.h - 4};
      titles_.push_back(r);
      if (static_cast<int>(titles_.size()) - 1 == hot_) c.fillRoundRect(r, 4, p.panel3);
      c.drawText(t, r, p.text, 12.5f, TextAlign::Center);
      x += w;
    }
    // song name + modified marker
    auto d = ctx_.doc();
    std::string title;
    if (d) {
      char buf[1024] = {};
      uint32_t need = 0;
      if (d->getPath(buf, sizeof buf, &need) == SQ_OK && buf[0]) {
        title = buf;
        title = title.substr(title.find_last_of("/\\") + 1);
      } else {
        title = "Untitled";
      }
      sq_document_info di{};
      di.struct_size = sizeof di;
      if (d->getDocumentInfo(&di) == SQ_OK && di.modified) title += " *";
    }
    c.drawText(title, Rect{f.x, f.y, f.w - 12, f.h}, p.dim, 12, TextAlign::Right);
  }
  bool onMouse(const WidgetMouse& e) override {
    int h = -1;
    for (size_t i = 0; i < titles_.size(); ++i)
      if (titles_[i].contains(e.x, e.y)) h = static_cast<int>(i);
    if (e.action == MouseAction::Hover) {
      if (h != hot_) {
        hot_ = h;
        ctx_.invalidate();
      }
      return true;
    }
    if (e.action == MouseAction::Down && h >= 0) {
      open(h, kMenuByClick);
      hot_ = -1;
      return true;
    }
    return h >= 0;
  }

 private:
  // Opens the menu of title i; the overlay switches to neighbours (Left/Right, clicking or hovering
  // another title) by calling back here.
  void open(int i, int how) {
    if (i < 0 || static_cast<size_t>(i) >= titles_.size()) return;
    showMenuBarMenu(ctx_, buildMainMenu(ctx_, kTitles[i]), Point{titles_[static_cast<size_t>(i)].x, frame().bottom()}, titles_, i,
                    [this](int next, int nextHow) { open(next, nextHow); }, how);
  }
  static constexpr const char* kTitles[] = {"File", "Edit", "Track", "Transport", "View", "Options", "Help"};
  GuiContext& ctx_;
  std::vector<Rect> titles_;
  int hot_ = -1;
};

}  // namespace

std::unique_ptr<Widget> makeMenuBar(GuiContext& ctx) { return std::make_unique<MenuBar>(ctx); }

bool handleShortcutKey(GuiContext& ctx, Key k, unsigned mods) {
  const bool ctrl = (mods & kModCtrl) != 0;
  switch (k) {
    case Key::Space: {
      const auto ts = ctx.transport();
      ctx.run((ts.flags & SQ_TRANSPORT_PLAYING) ? "Transport.Stop" : "Transport.Play");
      return true;
    }
    case Key::Home: ctx.run("Transport.Rewind"); return true;
    case Key::Delete:
    case Key::Backspace: runEditCommand(ctx, "Delete", "Edit.Delete"); return true;
    case Key::Left: ctx.run(ctrl ? "Edit.NudgeLeft" : "Navigate.CursorPreviousGrid"); return true;
    case Key::Right: ctx.run(ctrl ? "Edit.NudgeRight" : "Navigate.CursorNextGrid"); return true;
    case Key::Up: ctx.run("Navigate.PreviousTrack"); return true;
    case Key::Down: ctx.run("Navigate.NextTrack"); return true;
    case Key::Tab: ctx.run((mods & kModShift) ? "Navigate.SelectPreviousClip" : "Navigate.SelectNextClip"); return true;
    case Key::F2: {
      for (auto& it : buildMainMenu(ctx, "Track"))
        if (it.label == "Rename Track..." && it.action) it.action();
      return true;
    }
    case Key::F3: ctx.showBottomPanel(BottomPanel::Mixer); return true;
    case Key::F4: ctx.showBottomPanel(BottomPanel::Editor); return true;
    case Key::F6: ctx.showBottomPanel(BottomPanel::Devices); return true;
    case Key::F7: ctx.setBrowserVisible(!ctx.browserVisible()); return true;
    default: return false;
  }
}

bool handleShortcutChar(GuiContext& ctx, uint32_t ch, unsigned mods) {
  if (mods & kModCtrl) {
    const bool shift = (mods & kModShift) != 0;
    switch (ch) {
      case 'N' - '@': newSong(ctx); return true;
      case 'O' - '@': openSong(ctx); return true;
      case 'S' - '@':
        if (shift) saveSongAs(ctx);
        else saveSong(ctx);
        return true;
      case 'Z' - '@': ctx.run(shift ? "Edit.Redo" : "Edit.Undo"); return true;
      case 'Y' - '@': ctx.run("Edit.Redo"); return true;
      case 'D' - '@': runEditCommand(ctx, "Duplicate", "Edit.Duplicate"); return true;
      case 'A' - '@': runEditCommand(ctx, "SelectAll", "Navigate.SelectAll"); return true;
      case 'X' - '@': runEditCommand(ctx, "Cut", nullptr); return true;
      case 'C' - '@': runEditCommand(ctx, "Copy", nullptr); return true;
      case 'V' - '@': runEditCommand(ctx, "Paste", nullptr); return true;
      case 'T' - '@': ctx.run(shift ? "Track.AddInstrument" : "Track.AddAudio"); return true;
      case 'I' - '@': importAudio(ctx); return true;
      case 'E' - '@': exportAudio(ctx); return true;
      default: return false;
    }
  }
  switch (ch) {
    case 's': ctx.run("Edit.Split"); return true;
    case 'r': toggleRecord(ctx); return true;
    case 'l': ctx.run("Transport.ToggleLoop"); return true;
    case 'c': ctx.run("Transport.ToggleMetronome"); return true;
    case 'q': runEditCommand(ctx, "Quantize", "Edit.Quantize"); return true;
    case 'g': ctx.run("Edit.Glue"); return true;
    case ' ': return true;  // Space is handled as a key
    default: return false;
  }
}

}  // namespace sq::app::gui
