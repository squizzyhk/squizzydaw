// Squizzy Studio GUI - menu bar, main menus and global keyboard shortcuts.
#include <algorithm>
#include <cstring>

#include "gui_panels.hpp"

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

void openSong(GuiContext& ctx) {
  ctx.chooseFile(false, "Open Song", {"song", "dawproject"}, {}, [&ctx](const std::string& path) {
    auto a = sq::makeAttributes();
    a->setString("path", path.c_str());
    const bool daw = path.size() > 11 && path.substr(path.size() - 11) == ".dawproject";
    ctx.run(daw ? "File.ImportDawProject" : "File.Open", a.get());
  });
}

void saveSongAs(GuiContext& ctx) {
  ctx.chooseFile(true, "Save Song As", {"song"}, {}, [&ctx](const std::string& path) {
    auto a = sq::makeAttributes();
    a->setString("path", path.c_str());
    ctx.run("File.Save", a.get());
  });
}

void saveSong(GuiContext& ctx) {
  auto d = ctx.doc();
  char buf[1024] = {};
  uint32_t need = 0;
  if (d && d->getPath(buf, sizeof buf, &need) == SQ_OK && buf[0]) ctx.run("File.Save");
  else saveSongAs(ctx);
}

void exportAudio(GuiContext& ctx) {
  ctx.chooseFile(true, "Export Mixdown", {"wav", "flac", "ogg", "mp3"}, "mixdown.wav", [&ctx](const std::string& path) {
    auto a = sq::makeAttributes();
    a->setString("path", path.c_str());
    ctx.run("File.ExportAudio", a.get());
  });
}

void importAudio(GuiContext& ctx) {
  ctx.chooseFile(false, "Import Audio File", {"wav", "flac", "ogg", "mp3", "aif", "aiff", "m4a"}, {}, [&ctx](const std::string& path) {
    // New audio track at the edit cursor with the file as one clip.
    const double at = ctx.editState().cursor;
    ctx.setDraggedFile(path);
    auto a = sq::makeAttributes();
    std::string name = path.substr(path.find_last_of("/\\") + 1);
    a->setString("name", name.c_str());
    if (SQ_FAILED(ctx.run("Track.AddAudio", a.get()))) return;
    sq_guid track = ctx.editState().track;
    auto ts = ctx.tracks();
    if (!ts.empty()) track = ts.back().id;
    double lenPpq = 4.0;
    auto p = sq::makeAttributes();
    p->setGuid("track", &track);
    p->setInt("kind", SQ_CLIP_AUDIO);
    p->setDouble("start", at);
    p->setDouble("length", lenPpq);
    p->setString("source", path.c_str());
    auto res = sq::makeAttributes();
    ctx.exec(sq::doc::kAddClip, p.get(), res.get());
    ctx.setDraggedFile({});
  });
}

}  // namespace

std::vector<MenuItem> buildMainMenu(GuiContext& ctx, const std::string& title) {
  std::vector<MenuItem> m;
  if (title == "File") {
    m.push_back(cmd(ctx, "New Song", "File.New", "Ctrl+N"));
    m.push_back(fn("Open...", [&ctx] { openSong(ctx); }, "Ctrl+O"));
    m.push_back(fn("Save", [&ctx] { saveSong(ctx); }, "Ctrl+S"));
    m.push_back(fn("Save As...", [&ctx] { saveSongAs(ctx); }, "Ctrl+Shift+S"));
    m.push_back(MenuItem::sep());
    m.push_back(fn("Import Audio File...", [&ctx] { importAudio(ctx); }, "Ctrl+I"));
    m.push_back(fn("Export Mixdown...", [&ctx] { exportAudio(ctx); }, "Ctrl+E"));
    m.push_back(fn("Export Stems...", [&ctx] {
      ctx.chooseFile(true, "Export Stems To Folder", {}, {}, [&ctx](const std::string& dir) {
        auto a = sq::makeAttributes();
        a->setString("path", dir.c_str());
        ctx.run("File.ExportStems", a.get());
      });
    }));
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
    m.push_back(cmd(ctx, "Close Song", "File.Close"));
  } else if (title == "Edit") {
    m.push_back(cmd(ctx, "Undo", "Edit.Undo", "Ctrl+Z"));
    m.push_back(cmd(ctx, "Redo", "Edit.Redo", "Ctrl+Y"));
    m.push_back(MenuItem::sep());
    m.push_back(cmd(ctx, "Split at Cursor", "Edit.Split", "S"));
    m.push_back(cmd(ctx, "Duplicate", "Edit.Duplicate", "Ctrl+D"));
    m.push_back(cmd(ctx, "Delete", "Edit.Delete", "Del"));
    m.push_back(cmd(ctx, "Glue", "Edit.Glue", "G"));
    m.push_back(cmd(ctx, "Quantize", "Edit.Quantize", "Q"));
    m.push_back(MenuItem::sep());
    m.push_back(cmd(ctx, "Select All", "Navigate.SelectAll", "Ctrl+A"));
    m.push_back(cmd(ctx, "Select None", "Navigate.SelectNone"));
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
    m.push_back(cmd(ctx, "Record", "Transport.Record", "R"));
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
        if (bpm < 10 || bpm > 999) {
          ctx.setStatus("tempo must be between 10 and 999 BPM");
          return;
        }
        auto a = sq::makeAttributes();
        a->setDouble("bpm", bpm);
        if (SQ_FAILED(ctx.run("Song.SetTempo", a.get()))) {
          auto p = sq::makeAttributes();
          p->setDouble("position", 0.0);
          p->setDouble("bpm", bpm);
          ctx.exec(sq::doc::kSetTempo, p.get());
        }
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
    std::string curDrv = sh.settings.get("Devices.Audio.Driver");
#ifdef _WIN32
    if (curDrv.empty()) curDrv = "wasapi";
#elif defined(__linux__)
    if (curDrv.empty()) curDrv = "alsa";
#endif
    const std::string curDev = sh.settings.get("Devices.Audio.Device");
    auto select = [&ctx](const std::string& drv, const std::string& dev) {
      auto a = sq::makeAttributes();
      a->setString("driver", drv.c_str());
      a->setString("device", dev.c_str());
      ctx.run("Audio.SelectDevice", a.get());
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
      if (!curDrv.empty() && curDrv != "none" && curDrv != "null" &&
          SQ_SUCCEEDED(sh.execute("Audio", "ListDevices", a.get(), res.get()))) {
        const std::string lines = sq::getAttrString(res.get(), "devices");
        size_t pos = 0;
        while (pos < lines.size()) {
          size_t nl = lines.find('\n', pos);
          std::string line = lines.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
          pos = nl == std::string::npos ? lines.size() : nl + 1;
          const size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
          if (t2 == std::string::npos) continue;
          const std::string id = line.substr(t1 + 1, t2 - t1 - 1), name = line.substr(t2 + 1);
          MenuItem it;
          it.label = name.empty() ? id : name;
          it.checked = id == curDev;
          it.action = [select, curDrv, id] { select(curDrv, id); };
          devices.submenu.push_back(it);
        }
      }
      if (devices.submenu.empty()) {
        MenuItem none;
        none.label = curDrv.empty() || curDrv == "none" ? "(choose a driver first)" : "(no devices found)";
        none.enabled = false;
        devices.submenu.push_back(none);
      }
    }
    m.push_back(devices);
    auto settingMenu = [&](const char* label, const char* key, std::vector<int> values, const char* suffix) {
      MenuItem mi;
      mi.label = label;
      const int64_t cur = sh.settings.getInt(key, 0);
      for (int v : values) {
        MenuItem it;
        it.label = std::to_string(v) + suffix;
        it.checked = cur == v;
        const std::string k = key;
        it.action = [&ctx, select, k, v] {
          ctx.shell().settings.setInt(k.c_str(), v);
          const std::string d = ctx.shell().settings.get("Devices.Audio.Driver");
          if (!d.empty() && d != "none") select(d, ctx.shell().settings.get("Devices.Audio.Device"));
        };
        mi.submenu.push_back(it);
      }
      return mi;
    };
    m.push_back(settingMenu("Sample Rate", "Engine.SampleRate", {44100, 48000, 88200, 96000}, " Hz"));
    m.push_back(settingMenu("Buffer Size", "Engine.BlockSize", {32, 64, 128, 256, 512, 1024, 2048}, " samples"));
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
      ctx_.showMenu(buildMainMenu(ctx_, kTitles[h]), Point{titles_[static_cast<size_t>(h)].x, frame().bottom()});
      hot_ = -1;
      return true;
    }
    return h >= 0;
  }

 private:
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
    case Key::Delete: ctx.run("Edit.Delete"); return true;
    case Key::Backspace: ctx.run("Edit.Delete"); return true;
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
      case 'N' - '@': ctx.run("File.New"); return true;
      case 'O' - '@': openSong(ctx); return true;
      case 'S' - '@':
        if (shift) saveSongAs(ctx);
        else saveSong(ctx);
        return true;
      case 'Z' - '@': ctx.run(shift ? "Edit.Redo" : "Edit.Undo"); return true;
      case 'Y' - '@': ctx.run("Edit.Redo"); return true;
      case 'D' - '@': ctx.run("Edit.Duplicate"); return true;
      case 'A' - '@': ctx.run("Navigate.SelectAll"); return true;
      case 'T' - '@': ctx.run(shift ? "Track.AddInstrument" : "Track.AddAudio"); return true;
      case 'I' - '@': importAudio(ctx); return true;
      case 'E' - '@': exportAudio(ctx); return true;
      default: return false;
    }
  }
  switch (ch) {
    case 's': ctx.run("Edit.Split"); return true;
    case 'r': ctx.run("Transport.Record"); return true;
    case 'l': ctx.run("Transport.ToggleLoop"); return true;
    case 'c': ctx.run("Transport.ToggleMetronome"); return true;
    case 'q': ctx.run("Edit.Quantize"); return true;
    case 'g': ctx.run("Edit.Glue"); return true;
    case ' ': return true;  // Space is handled as a key
    default: return false;
  }
}

}  // namespace sq::app::gui
