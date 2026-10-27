// Squizzy Studio GUI: the main window built from the embedded default skin (app/resources/
// default.skin.xml): transport bar, arrange view (tracks + clips + playhead), mixer strips (one
// skin form instance per track, bound to document properties), status line. Every button, key and
// fader goes through Shell::execute / document actions, so the GUI has no private edit paths.
#include "gui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

#include "default_skin.h"
#include "sq/cpp/attributes.hpp"
#include "sq/platform.hpp"
#include "sq/ui/skin.hpp"
#include "sq/ui/theme.hpp"
#include "sq/ui/widgets.hpp"

namespace sq::app {
namespace {

using namespace sq::ui;

std::string nodeName(INode* n) {
  char buf[256];
  uint32_t need = 0;
  return n && n->getName(buf, sizeof buf, &need) == SQ_OK ? std::string(buf) : std::string();
}
sq_guid nodeId(INode* n) {
  sq_guid g{};
  if (n) n->getId(&g);
  return g;
}

// Arrange view: one lane per track, clips as boxes, playhead + edit cursor. Click = select clip /
// place the cursor (through Navigate.SetEditState), double-click empty space = cursor only.
class ArrangeView final : public Widget {
 public:
  explicit ArrangeView(Shell& sh) : Widget("Arrange"), sh_(sh) { hitTestable = true; }
  double pxPerBeat = 24;
  float laneH = 46, headerW = 150;
  double cursor = 0;
  sq_guid selected{};

  void paint(Canvas& c) override {
    paintBox(c);
    const Rect& f = frame();
    c.pushClip(f);
    auto doc = sh_.activeDocument();
    if (!doc) {
      c.drawText("No song open - press Ctrl+N or the New button", Rect{f.x + 16, f.y + 16, f.w - 32, 24}, Color::rgb(0x9AA0A8), 14);
      c.popClip();
      return;
    }
    // ruler
    c.fillRect(Rect{f.x, f.y, f.w, 20}, Color::rgb(0x23252B));
    for (int bar = 0; bar < 512; ++bar) {
      float x = f.x + headerW + static_cast<float>(bar * 4 * pxPerBeat);
      if (x > f.right()) break;
      c.drawLine(Point{x, f.y + 10}, Point{x, f.bottom()}, Color::rgb(0x2C2F36), 1);
      c.drawText(std::to_string(bar + 1), Rect{x + 3, f.y + 2, 40, 16}, Color::rgb(0x8A9099), 11);
    }
    sq_document_info di{};
    di.struct_size = sizeof di;
    doc->getDocumentInfo(&di);
    for (uint32_t i = 0; i < di.track_count; ++i) {
      sq::Ptr<ITrack> t;
      if (doc->getTrack(i, t.put()) != SQ_OK) continue;
      sq_track_info ti{};
      ti.struct_size = sizeof ti;
      t->getTrackInfo(&ti);
      float y = f.y + 22 + static_cast<float>(i) * laneH;
      if (y > f.bottom()) break;
      c.fillRect(Rect{f.x, y, headerW - 2, laneH - 2}, Color::rgb(0x2F323A));
      Color kindCol = ti.kind == SQ_TRACK_INSTRUMENT ? Color::rgb(0x8E7CC3) : Color::rgb(0x4FB3A9);
      c.fillRect(Rect{f.x, y, 4, laneH - 2}, kindCol);
      std::string label = nodeName(t.get());
      if (ti.flags & SQ_TRACK_ARMED) label += "  [R]";
      c.drawText(label, Rect{f.x + 10, y + 4, headerW - 14, 18}, Color::rgb(0xE4E6EB), 12);
      for (uint32_t k = 0; k < ti.clip_count; ++k) {
        sq::Ptr<IClip> cl;
        if (t->getClip(k, cl.put()) != SQ_OK) continue;
        sq_clip_info ci{};
        ci.struct_size = sizeof ci;
        cl->getClipInfo(&ci);
        Rect r{f.x + headerW + static_cast<float>(ci.start * pxPerBeat), y + 2, static_cast<float>(ci.length * pxPerBeat), laneH - 6};
        bool sel = nodeId(cl.get()) == selected;
        c.fillRoundRect(r, 3, kindCol.withAlpha((ci.flags & SQ_CLIP_MUTED) ? 0.25f : 0.6f));
        if (sel) c.strokeRoundRect(r, 3, Color::rgb(0xFFFFFF), 2);
        c.drawText(nodeName(cl.get()), Rect{r.x + 4, r.y + 2, r.w - 8, 16}, Color::rgb(0x101114), 11);
      }
    }
    float cx = f.x + headerW + static_cast<float>(cursor * pxPerBeat);
    c.drawLine(Point{cx, f.y}, Point{cx, f.bottom()}, Color::rgb(0xD9A441), 1);
    if (IAudioEngine* e = sh_.engine()) {
      sq::Ptr<ITransport> tr;
      e->queryInterface(&ITransport::iid, tr.putVoid());
      sq_transport_state ts{};
      ts.struct_size = sizeof ts;
      if (tr && tr->getTransportState(&ts) == SQ_OK) {
        float px = f.x + headerW + static_cast<float>(ts.position * pxPerBeat);
        c.drawLine(Point{px, f.y}, Point{px, f.bottom()}, (ts.flags & SQ_TRANSPORT_RECORDING) ? Color::rgb(0xE0604A) : Color::rgb(0xFFFFFF), 2);
      }
    }
    c.popClip();
  }

  bool onMouse(const WidgetMouse& e) override {
    if (e.action == MouseAction::Wheel) {
      pxPerBeat = std::clamp(pxPerBeat * (e.wheel > 0 ? 1.2 : 1 / 1.2), 2.0, 400.0);
      invalidate();
      return true;
    }
    if (e.action != MouseAction::Down || e.button != MouseButton::Left) return false;
    auto doc = sh_.activeDocument();
    if (!doc) return true;
    const Rect& f = frame();
    double ppq = std::max(0.0, (e.x - f.x - headerW) / pxPerBeat);
    int lane = static_cast<int>((e.y - f.y - 22) / laneH);
    auto args = sq::makeAttributes();
    args->setDouble("cursor", ppq);
    cursor = ppq;
    selected = sq_guid{};
    sq::Ptr<ITrack> t;
    if (lane >= 0 && doc->getTrack(static_cast<uint32_t>(lane), t.put()) == SQ_OK && t) {
      sq_guid tg = nodeId(t.get());
      args->setGuid("track", &tg);
      sq_track_info ti{};
      ti.struct_size = sizeof ti;
      t->getTrackInfo(&ti);
      for (uint32_t k = 0; k < ti.clip_count; ++k) {
        sq::Ptr<IClip> cl;
        if (t->getClip(k, cl.put()) != SQ_OK) continue;
        sq_clip_info ci{};
        ci.struct_size = sizeof ci;
        cl->getClipInfo(&ci);
        if (ppq >= ci.start && ppq < ci.start + ci.length) selected = nodeId(cl.get());
      }
    }
    std::string sel = sq_guid_is_null(&selected) ? std::string() : sq::toString(selected);
    args->setString("nodes", sel.c_str());
    sh_.execute("Navigate", "SetEditState", args.get());
    invalidate();
    return true;
  }

 private:
  Shell& sh_;
};

struct Gui {
  Shell& sh;
  std::unique_ptr<Skin> skin;
  SkinDefines defines;    // theme defines the tree was built with
  std::string themeName;  // theme the tree was built with
  WidgetTree tree;
  IWindow* window = nullptr;
  std::string status;
  uint32_t lastTrackCount = UINT32_MAX;
  uint64_t lastDocSerial = 0;
  sq_guid lastDoc{};

  explicit Gui(Shell& s) : sh(s) {}

  void runCommand(const std::string& dotted, IAttributes* args = nullptr) {
    sq_result r = sh.execute(dotted, args);
    status = !sh.lastMessage().empty() ? sh.lastMessage() : dotted + (SQ_SUCCEEDED(r) ? "" : std::string(": ") + sq_result_string(r));
    rebuildMixerIfNeeded(true);
    invalidate();
  }
  void invalidate() {
    if (window) window->invalidate();
  }

  // Active theme as skin defines: theme.color.* / theme.metric.* (ThemeResolver::skinDefines) plus
  // the default skin's short color names mapped onto theme colors, so default.skin.xml keeps its
  // built-in fallbacks when no theme is loaded.
  SkinDefines themeDefines() const {
    SkinDefines d;
    ThemeResolver* th = sh.themes();
    if (!th) return d;
    d = th->skinDefines();
    static const std::pair<const char*, const char*> kAlias[] = {
        {"bg", "Window.Background"}, {"panel", "Panel.Background"}, {"panel2", "Track.Background"}, {"text", "Text"},
        {"dim", "Text.Dim"},         {"accent", "Accent"},          {"rec", "Transport.Record"}};
    for (const auto& [shortName, color] : kAlias)
      if (auto it = d.find(std::string("theme.color.") + color); it != d.end()) d[shortName] = it->second;
    return d;
  }

  bool build(std::string* err) {
    if (!skin) {
      skin = Skin::parse(reinterpret_cast<const char*>(kDefaultSkin), err);
      if (!skin) return false;
      skin->registerWidget("Arrange", [this] { return std::make_unique<ArrangeView>(sh); });
    }
    themeName = sh.themes() ? sh.themes()->activeTheme() : std::string();
    defines = themeDefines();
    auto root = skin->build("Main", defines, err);
    if (!root) return false;
    if (window) window->setAccessibilityRoot(nullptr);  // before the old tree dies
    tree.setRoot(std::move(root));
    if (window) window->setAccessibilityRoot(tree.root());
    if (ThemeResolver* th = sh.themes()) tree.imageProvider = th->imageProvider();
    tree.onCommand = [this](Widget& w) {
      if (!w.command.empty()) runCommand(w.command);
      else if (!w.target.empty()) writeBinding(w);
    };
    tree.onValueChanged = [this](Widget& w) {
      if (!w.target.empty()) writeBinding(w);
      if (window) window->notifyAccessibilityChanged(&w);
    };
    tree.onAccessibilityChanged = [this](Widget& w) {
      if (window) window->notifyAccessibilityChanged(&w);
    };
    tree.onInvalidate = [this] { invalidate(); };
    rebuildMixerIfNeeded(true);
    return true;
  }

  // UI.Theme changed (UI.SetTheme, settings): rebuild the widget tree with the new colors.
  void followTheme() {
    ThemeResolver* th = sh.themes();
    if (!th || th->activeTheme() == themeName) return;
    std::string err;
    if (!build(&err)) {
      status = "theme: " + err;
      themeName = th->activeTheme();  // do not retry every frame
    }
    invalidate();
  }

  // Widget -> document: one SetProperty per change (drags merge into one undo step).
  void writeBinding(Widget& w) {
    auto doc = sh.activeDocument();
    if (!doc) return;
    const std::string prefix = "object://doc/active/node/";
    if (w.target.url.rfind(prefix, 0) != 0) return;
    sq_guid node{};
    if (!sq::parseGuid(w.target.url.substr(prefix.size()), node)) return;
    auto p = sq::makeAttributes();
    p->setGuid("node", &node);
    p->setString("key", w.target.property.c_str());
    if (dynamic_cast<Button*>(&w)) p->setBool("value", w.value() >= 0.5 ? SQ_TRUE : SQ_FALSE);
    else {
      p->setDouble("value", w.value());
      p->setBool("merge", SQ_TRUE);
    }
    sq::Ptr<IEditor> ed;
    if (doc->getEditor(ed.put()) == SQ_OK) ed->execute(&sq::doc::kSetProperty, p.get(), nullptr);
  }

  // Document -> widgets (values) and strip layout (track list changes).
  void rebuildMixerIfNeeded(bool force) {
    auto doc = sh.activeDocument();
    Widget* mixer = tree.findByName("mixer");
    if (!mixer) return;
    sq_document_info di{};
    di.struct_size = sizeof di;
    if (doc) doc->getDocumentInfo(&di);
    uint32_t count = doc ? di.track_count : 0;
    if (!force && count == lastTrackCount && di.id == lastDoc) return;
    lastTrackCount = count;
    lastDoc = di.id;
    mixer->clearChildren();
    for (uint32_t i = 0; i < count; ++i) {
      sq::Ptr<ITrack> t;
      if (doc->getTrack(i, t.put()) != SQ_OK) continue;
      sq_track_info ti{};
      ti.struct_size = sizeof ti;
      t->getTrackInfo(&ti);
      if (ti.kind != SQ_TRACK_AUDIO && ti.kind != SQ_TRACK_INSTRUMENT) continue;
      SkinDefines d = defines;
      d["trackName"] = nodeName(t.get());
      d["trackId"] = sq::toString(nodeId(t.get()));
      d["trackIndex"] = std::to_string(i);
      d["kind"] = ti.kind == SQ_TRACK_AUDIO ? "audio" : "instrument";
      std::string err;
      auto strip = skin->build("Strip", d, &err);
      if (!strip) continue;
      auto set = [&](const std::string& n, double v) {
        if (Widget* w = strip->findByName(n)) w->setValue(v);
      };
      set("vol-" + std::to_string(i), ti.volume_db);
      set("pan-" + std::to_string(i), ti.pan);
      set("mute-" + std::to_string(i), (ti.flags & SQ_TRACK_MUTE) ? 1 : 0);
      set("solo-" + std::to_string(i), (ti.flags & SQ_TRACK_SOLO) ? 1 : 0);
      set("arm-" + std::to_string(i), (ti.flags & SQ_TRACK_ARMED) ? 1 : 0);
      mixer->addChild(std::move(strip));
    }
    if (window) window->notifyAccessibilityChanged(mixer);  // strips replaced: structure changed
  }
  void refreshLabels() {
    if (Widget* pos = tree.findByName("position")) {
      if (IAudioEngine* e = sh.engine()) {
        sq::Ptr<ITransport> tr;
        e->queryInterface(&ITransport::iid, tr.putVoid());
        sq_transport_state ts{};
        ts.struct_size = sizeof ts;
        if (tr && tr->getTransportState(&ts) == SQ_OK) {
          uint32_t num = ts.numerator ? ts.numerator : 4;
          int64_t beats = static_cast<int64_t>(std::floor(ts.position));
          char buf[64];
          std::snprintf(buf, sizeof buf, "%lld.%lld.%03d  %s", static_cast<long long>(beats / num + 1), static_cast<long long>(beats % num + 1),
                        static_cast<int>((ts.position - std::floor(ts.position)) * 1000),
                        (ts.flags & SQ_TRANSPORT_RECORDING) ? "REC" : (ts.flags & SQ_TRANSPORT_PLAYING) ? "PLAY" : "STOP");
          pos->text = buf;
        }
      }
    }
    if (Widget* s = tree.findByName("status")) s->text = status;
    if (Widget* d = tree.findByName("device")) d->text = sh.deviceDescription();
  }

  void onKey(Key k, unsigned mods) {
    if (tree.dispatchKey(k, mods)) return;
    switch (k) {
      case Key::Space: {
        sq_transport_state ts{};
        ts.struct_size = sizeof ts;
        sq::Ptr<ITransport> tr;
        if (IAudioEngine* e = sh.engine()) e->queryInterface(&ITransport::iid, tr.putVoid());
        if (tr) tr->getTransportState(&ts);
        runCommand((ts.flags & SQ_TRANSPORT_PLAYING) ? "Transport.Stop" : "Transport.Play");
        break;
      }
      case Key::Home: runCommand("Transport.Rewind"); break;
      case Key::Delete: runCommand("Edit.Delete"); break;
      case Key::Left: runCommand((mods & kModCtrl) ? "Edit.NudgeLeft" : "Navigate.CursorPreviousGrid"); break;
      case Key::Right: runCommand((mods & kModCtrl) ? "Edit.NudgeRight" : "Navigate.CursorNextGrid"); break;
      case Key::Up: runCommand("Navigate.PreviousTrack"); break;
      case Key::Down: runCommand("Navigate.NextTrack"); break;
      case Key::Tab: runCommand((mods & kModShift) ? "Navigate.SelectPreviousClip" : "Navigate.SelectNextClip"); break;
      default: break;
    }
  }
  void onChar(uint32_t ch, unsigned mods) {
    if (mods & kModCtrl) {
      switch (ch) {
        case 'N' - '@': runCommand("File.New"); break;
        case 'S' - '@': runCommand("File.Save"); break;
        case 'Z' - '@': runCommand("Edit.Undo"); break;
        case 'Y' - '@': runCommand("Edit.Redo"); break;
        case 'D' - '@': runCommand("Edit.Duplicate"); break;
        case 'A' - '@': runCommand("Navigate.SelectAll"); break;
        default: break;
      }
      return;
    }
    switch (ch) {
      case 's': runCommand("Edit.Split"); break;
      case 'r': runCommand("Transport.Record"); break;
      case 'l': runCommand("Transport.ToggleLoop"); break;
      case 'q': runCommand("Edit.Quantize"); break;
      case 'g': runCommand("Edit.Glue"); break;
      default: break;
    }
  }
};

}  // namespace

int runGui(Shell& shell, int quitAfterMs) {
  auto window = sq::ui::createWindow(sq::ui::WindowDesc{"Squizzy Studio", 1280, 800, true});
  if (!window) {
    std::printf("no window backend on this platform (%s); use --headless\n", sq::ui::defaultBackendName());
    return 2;
  }
  Gui gui(shell);
  gui.window = window.get();
  std::string err;
  if (!gui.build(&err)) {
    std::fprintf(stderr, "skin: %s\n", err.c_str());
    return 3;
  }
  if (!shell.activeDocument()) shell.execute("File.New");
  gui.status = "F5: reload services  |  Space: play/stop  R: record  S: split  Ctrl+Z/Y: undo/redo";
  gui.rebuildMixerIfNeeded(true);

  sq::ui::WindowCallbacks cb;
  cb.onPaint = [&](sq::ui::IRenderer& r) {
    gui.refreshLabels();
    gui.tree.setScale(r.scale());
    gui.tree.layout(static_cast<float>(r.size().width), static_cast<float>(r.size().height));
    r.clear(sq::ui::Color::rgb(0x1C1D21));
    gui.tree.paint(r);
  };
  cb.onMouse = [&](const sq::ui::MouseEvent& e) {
    if (gui.tree.dispatchMouse(e)) window->invalidate();
  };
  cb.onKeyEx = [&](sq::ui::Key k, unsigned mods) {
    if (k == sq::ui::Key::F5) {
      int ok = 0, busy = 0;
      for (const auto& m : shell.loader.modules()) {
        if (shell.loader.reload(m.name).ok()) ++ok;
        else ++busy;
      }
      gui.status = "reloaded " + std::to_string(ok) + " service(s), " + std::to_string(busy) + " failed/busy";
      window->invalidate();
      return;
    }
    if (k == sq::ui::Key::Escape) {
      window->close();
      return;
    }
    gui.onKey(k, mods);
  };
  cb.onChar = [&](uint32_t ch, unsigned mods) { gui.onChar(ch, mods); };
  window->setCallbacks(std::move(cb));
  window->invalidate();
  uint64_t start = sq_time_monotonic_ns();
  uint64_t lastRepaint = 0;
  for (;;) {
    bool alive = window->pumpEvents(false);
    if (alive && quitAfterMs >= 0 && (sq_time_monotonic_ns() - start) / 1000000ull >= static_cast<uint64_t>(quitAfterMs)) {
      window->close();
      alive = window->pumpEvents(false);
    }
    if (!alive) {
      window->setAccessibilityRoot(nullptr);  // the tree dies with `gui`
      break;
    }
    shell.idle();
    gui.followTheme();
    gui.rebuildMixerIfNeeded(false);
    uint64_t now = sq_time_monotonic_ns();
    if (now - lastRepaint > 33000000ull) {  // ~30 fps playhead / meters
      lastRepaint = now;
      window->invalidate();
    }
    sq_thread_sleep_ms(5);
  }
  return 0;
}

}  // namespace sq::app
