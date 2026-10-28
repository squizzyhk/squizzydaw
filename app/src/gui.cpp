// Squizzy Studio GUI: the main window. Built from the embedded default skin
// (app/resources/default.skin.xml) whose custom elements are the panels of gui_panels.hpp (menu
// bar, transport, browser, arrange, clip editor, mixer, device rack). This file owns the window
// loop, the GuiContext implementation (commands, overlays, panel switching), splitters, the status
// line, keyboard routing and the headless runner used by --screenshot / --ui-script.
#include "gui.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include "default_skin.h"
#include "gui_context.hpp"
#include "gui_overlay.hpp"
#include "gui_panels.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/cpp/object.hpp"
#include "sq/platform.hpp"
#include "sq/ui/image.hpp"
#include "sq/ui/skin.hpp"
#include "sq/ui/theme.hpp"
#include "sq/ui/widgets.hpp"

namespace sq::app {
namespace {

using namespace sq::ui;
using gui::BottomPanel;
using gui::GuiContext;
using gui::MenuItem;
using gui::Palette;

class FnObserver final : public sq::Object<FnObserver, IObserver> {
 public:
  explicit FnObserver(std::function<void(const char*)> fn) : fn_(std::move(fn)) {}
  void SQ_CALL onNotify(IUnknown*, const char* topic, IAttributes*) override {
    if (fn_ && topic) fn_(topic);
  }

 private:
  std::function<void(const char*)> fn_;
};

// Draggable divider: resizes the sibling named `target` (height for horizontal dividers, width for
// vertical ones). sign = +1 when dragging right/down grows the target.
class Splitter final : public Widget {
 public:
  Splitter(GuiContext& ctx, bool vertical) : Widget("Splitter"), ctx_(ctx), vertical_(vertical) {}
  std::string targetName;
  float sign = 1, minSize = 80, maxSize = 2000;
  void paint(Canvas& c) override {
    const Rect& f = frame();
    c.fillRect(f, ctx_.palette().border);
    if (hot_ || drag_) c.fillRect(f, ctx_.palette().accent.withAlpha(0.5f));
  }
  bool onMouse(const WidgetMouse& e) override {
    Widget* t = target();
    switch (e.action) {
      case MouseAction::Hover:
        hot_ = true;
        return true;
      case MouseAction::Down:
        if (!t) return false;
        drag_ = true;
        start_ = vertical_ ? e.x : e.y;
        startSize_ = vertical_ ? t->frame().w : t->frame().h;
        return true;
      case MouseAction::Move:
        if (drag_ && t) {
          const float d = ((vertical_ ? e.x : e.y) - start_) * sign;
          const float sz = std::clamp(startSize_ + d, minSize, maxSize);
          if (vertical_) t->style.width = Dim::pt(sz);
          else t->style.height = Dim::pt(sz);
          ctx_.invalidate();
        }
        return true;
      case MouseAction::Up:
        drag_ = false;
        hot_ = false;
        return true;
      case MouseAction::Leave: hot_ = false; return false;
      default: return false;
    }
  }

 private:
  Widget* target() {
    for (Widget* p = parent(); p; p = p->parent())
      if (Widget* w = p->findByName(targetName)) return w;
    return nullptr;
  }
  GuiContext& ctx_;
  bool vertical_;
  bool hot_ = false, drag_ = false;
  float start_ = 0, startSize_ = 0;
};

// Tab strip of the bottom panel (Mixer | Editor | Devices).
class PanelTabs final : public Widget {
 public:
  explicit PanelTabs(GuiContext& ctx) : Widget("PanelTabs"), ctx_(ctx) {}
  void paint(Canvas& c) override {
    const Palette& p = ctx_.palette();
    const Rect& f = frame();
    c.fillRect(f, p.panel);
    static const char* kNames[] = {"Mixer", "Editor", "Devices"};
    float x = f.x + 8;
    for (int i = 0; i < 3; ++i) {
      const float w = c.measureText(kNames[i], 12) + 28;
      tabs_[i] = Rect{x, f.y + 3, w, f.h - 3};
      const bool on = ctx_.bottomPanelVisible() && static_cast<int>(ctx_.bottomPanel()) == i;
      if (on) c.fillRoundRect(Rect{tabs_[i].x, tabs_[i].y, tabs_[i].w, tabs_[i].h + 6}, 5, p.panel2);
      else if (hot_ == i) c.fillRoundRect(Rect{tabs_[i].x, tabs_[i].y, tabs_[i].w, tabs_[i].h + 6}, 5, p.panel3.withAlpha(0.5f));
      c.drawText(kNames[i], tabs_[i], on ? p.text : p.dim, 12, TextAlign::Center);
      x += w + 4;
    }
    hideR_ = Rect{f.right() - 30, f.y + 3, 24, f.h - 6};
    gui::draw::triangle(c, Rect{hideR_.x + 7, hideR_.y + 8, 10, 6}, ctx_.bottomPanelVisible() ? 2 : 0, hot_ == 3 ? p.text : p.dim);
  }
  bool onMouse(const WidgetMouse& e) override {
    int h = -1;
    for (int i = 0; i < 3; ++i)
      if (tabs_[i].contains(e.x, e.y)) h = i;
    if (hideR_.contains(e.x, e.y)) h = 3;
    if (e.action == MouseAction::Hover) {
      if (h != hot_) {
        hot_ = h;
        ctx_.invalidate();
      }
      return true;
    }
    if (e.action == MouseAction::Down && e.button == MouseButton::Left) {
      if (h == 3) ctx_.setBottomPanelVisible(!ctx_.bottomPanelVisible());
      else if (h >= 0) {
        const auto want = static_cast<BottomPanel>(h);
        if (ctx_.bottomPanelVisible() && ctx_.bottomPanel() == want) ctx_.setBottomPanelVisible(false);
        else ctx_.showBottomPanel(want);
      }
      return true;
    }
    return h >= 0;
  }

 private:
  GuiContext& ctx_;
  Rect tabs_[3];
  Rect hideR_;
  int hot_ = -1;
};

class StatusBar final : public Widget {
 public:
  StatusBar(GuiContext& ctx, std::function<std::string()> left, std::function<std::string()> right)
      : Widget("StatusBar"), ctx_(ctx), left_(std::move(left)), right_(std::move(right)) {}
  void paint(Canvas& c) override {
    const Palette& p = ctx_.palette();
    const Rect& f = frame();
    c.fillRect(f, p.panel);
    c.fillRect(Rect{f.x, f.y, f.w, 1}, p.border);
    c.drawText(left_(), Rect{f.x + 10, f.y, f.w * 0.65f, f.h}, p.dim, 11.5f);
    c.drawText(right_(), Rect{f.x + f.w * 0.5f, f.y, f.w * 0.5f - 10, f.h}, p.dim, 11.5f, TextAlign::Right);
  }

 private:
  GuiContext& ctx_;
  std::function<std::string()> left_, right_;
};

Color parseColorDefine(const std::string& s, Color fallback) {
  if (s.size() == 7 && s[0] == '#') return Color::rgb(static_cast<unsigned>(std::strtoul(s.c_str() + 1, nullptr, 16)));
  if (s.size() == 9 && s[0] == '#') return Color::rgba(static_cast<uint32_t>(std::strtoul(s.c_str() + 1, nullptr, 16)));
  return fallback;
}

}  // namespace

class Gui final : public GuiContext {
 public:
  explicit Gui(Shell& s) : sh_(s) {}
  ~Gui() override { detachObservers(); }

  Shell& sh_;
  std::unique_ptr<Skin> skin;
  SkinDefines defines;
  std::string themeName;
  WidgetTree tree;
  IWindow* window = nullptr;
  gui::OverlayStack overlays;
  Palette pal;
  std::string status;
  uint64_t serial_ = 1;
  BottomPanel bottom_ = BottomPanel::Mixer;
  bool bottomVisible_ = true;
  bool browserVisible_ = true;
  sq_guid editorClip_{};
  std::string dragged_;
  float viewW_ = 0, viewH_ = 0;  // DIPs
  sq::Ptr<IObserver> docObserver_;
  sq::Ptr<ISubject> observedMgr_, observedDoc_;
  sq_guid lastDoc_{};
  bool quitConfirmed = false;

  // ---- GuiContext ----
  Shell& shell() override { return sh_; }
  const Palette& palette() const override { return pal; }
  uint64_t docSerial() const override { return serial_; }
  sq_result run(std::string_view dotted, IAttributes* args) override {
    sq_result r = sh_.execute(dotted, args);
    status = !sh_.lastMessage().empty() ? sh_.lastMessage()
                                        : std::string(dotted) + (SQ_SUCCEEDED(r) ? std::string() : std::string(": ") + sq_result_string(r));
    ++serial_;  // commands may change journals (selection) without a document.changed
    invalidate();
    return r;
  }
  void setStatus(const std::string& text) override {
    status = text;
    invalidate();
  }
  void invalidate() override {
    if (window) window->invalidate();
  }
  void showMenu(std::vector<MenuItem> items, Point at) override {
    overlays.push(gui::makeMenuOverlay(std::move(items), at));
    invalidate();
  }
  void prompt(const std::string& title, const std::string& initial, std::function<void(const std::string&)> onOk) override {
    overlays.push(gui::makePromptOverlay(title, initial, std::move(onOk)));
    invalidate();
  }
  void confirm(const std::string& message, std::function<void()> onYes) override {
    overlays.push(gui::makeConfirmOverlay(message, std::move(onYes)));
    invalidate();
  }
  void chooseFile(bool save, const std::string& title, const std::vector<std::string>& exts, const std::string& initial,
                  std::function<void(const std::string&)> onOk) override {
    overlays.push(gui::makeFileOverlay(save, title, exts, initial, std::move(onOk)));
    invalidate();
  }
  void showBottomPanel(BottomPanel p) override {
    bottom_ = p;
    bottomVisible_ = true;
    applyPanels();
  }
  BottomPanel bottomPanel() const override { return bottom_; }
  bool bottomPanelVisible() const override { return bottomVisible_; }
  void setBottomPanelVisible(bool v) override {
    bottomVisible_ = v;
    applyPanels();
  }
  bool browserVisible() const override { return browserVisible_; }
  void setBrowserVisible(bool v) override {
    browserVisible_ = v;
    applyPanels();
  }
  void openClipEditor(const sq_guid& clip) override {
    editorClip_ = clip;
    showBottomPanel(BottomPanel::Editor);
  }
  sq_guid editorClip() const override { return editorClip_; }
  std::string draggedFile() const override { return dragged_; }
  void setDraggedFile(const std::string& path) override { dragged_ = path; }
  Widget* findWidget(std::string_view name) override { return tree.findByName(name); }

  // ---- setup ----
  void loadPalette() {
    pal = Palette{};
    ThemeResolver* th = sh_.themes();
    pal.trackColors = {Color::rgb(0x4FB3A9), Color::rgb(0x8E7CC3), Color::rgb(0xD9A441), Color::rgb(0x5FA8E8), Color::rgb(0xE07A5F),
                       Color::rgb(0x81B29A), Color::rgb(0xC36FA8), Color::rgb(0xB5B84A)};
    if (!th) return;
    const SkinDefines d = th->skinDefines();
    auto col = [&](const char* name, Color fb) {
      auto it = d.find(std::string("theme.color.") + name);
      return it == d.end() ? fb : parseColorDefine(it->second, fb);
    };
    pal.bg = col("Window.Background", pal.bg);
    pal.panel = col("Panel.Background", pal.panel);
    pal.border = col("Panel.Border", pal.border);
    pal.panel2 = col("Track.Background", pal.panel2);
    pal.trackSel = col("Track.Selected", pal.trackSel);
    pal.text = col("Text", pal.text);
    pal.dim = col("Text.Dim", pal.dim);
    pal.accent = col("Accent", pal.accent);
    pal.rec = col("Transport.Record", pal.rec);
    pal.playhead = col("Playhead", pal.playhead);
    const float lum = 0.299f * pal.bg.r + 0.587f * pal.bg.g + 0.114f * pal.bg.b;
    pal.dark = lum < 0.5f;
    pal.panel3 = pal.dark ? gui::draw::lighten(pal.panel2, 0.08f) : gui::draw::mix(pal.panel2, Color::rgb(0x000000), 0.08f);
    pal.faint = gui::draw::mix(pal.dim, pal.bg, 0.45f);
    pal.trackBg = gui::draw::mix(pal.bg, pal.panel, 0.35f);
    pal.grid = gui::draw::mix(pal.trackBg, pal.text, 0.06f);
    pal.gridBar = gui::draw::mix(pal.trackBg, pal.text, 0.14f);
    if (!pal.dark) pal.playhead = Color::rgb(0x202124);
  }

  SkinDefines themeDefines() const {
    SkinDefines d;
    ThemeResolver* th = sh_.themes();
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
    loadPalette();
    if (!skin) {
      skin = Skin::parse(reinterpret_cast<const char*>(kDefaultSkin), err);
      if (!skin) return false;
      skin->registerWidget("MenuBar", [this] { return gui::makeMenuBar(*this); });
      skin->registerWidget("TransportBar", [this] { return gui::makeTransportBar(*this); });
      skin->registerWidget("Browser", [this] { return gui::makeBrowser(*this); });
      skin->registerWidget("Arrange", [this] { return gui::makeArrange(*this); });
      skin->registerWidget("ClipEditor", [this] { return gui::makeClipEditor(*this); });
      skin->registerWidget("Mixer", [this] { return gui::makeMixer(*this); });
      skin->registerWidget("DeviceRack", [this] { return gui::makeDeviceRack(*this); });
      skin->registerWidget("PanelTabs", [this] { return std::make_unique<PanelTabs>(*this); });
      skin->registerWidget("HSplitter", [this] {
        auto s = std::make_unique<Splitter>(*this, false);
        s->targetName = "bottom";
        s->sign = -1;
        s->minSize = 120;
        s->maxSize = 900;
        return s;
      });
      skin->registerWidget("VSplitter", [this] {
        auto s = std::make_unique<Splitter>(*this, true);
        s->targetName = "browser";
        s->minSize = 160;
        s->maxSize = 600;
        return s;
      });
      skin->registerWidget("StatusBar", [this] {
        return std::make_unique<StatusBar>(*this, [this] { return status; }, [this] { return statusRight(); });
      });
    }
    themeName = sh_.themes() ? sh_.themes()->activeTheme() : std::string();
    defines = themeDefines();
    auto root = skin->build("Main", defines, err);
    if (!root) return false;
    if (window) window->setAccessibilityRoot(nullptr);
    tree.setRoot(std::move(root));
    if (window) window->setAccessibilityRoot(tree.root());
    if (ThemeResolver* th = sh_.themes()) tree.imageProvider = th->imageProvider();
    tree.onCommand = [this](Widget& w) {
      if (!w.command.empty()) run(w.command, nullptr);
    };
    tree.onValueChanged = [this](Widget& w) {
      if (window) window->notifyAccessibilityChanged(&w);
    };
    tree.onAccessibilityChanged = [this](Widget& w) {
      if (window) window->notifyAccessibilityChanged(&w);
    };
    tree.onInvalidate = [this] { invalidate(); };
    tree.textMeasurer = [](std::string_view t, float size) { return measureDip(t, size); };
    applyPanels();
    return true;
  }

  static float measureDip(std::string_view t, float size);

  void applyPanels() {
    if (!tree.root()) return;
    auto show = [&](const char* name, bool on) {
      if (Widget* w = tree.findByName(name)) w->style.display = on;
    };
    show("browser", browserVisible_);
    show("browserSplit", browserVisible_);
    show("bottom", bottomVisible_);
    show("bottomSplit", bottomVisible_);
    show("mixer", bottom_ == BottomPanel::Mixer);
    show("editor", bottom_ == BottomPanel::Editor);
    show("devices", bottom_ == BottomPanel::Devices);
    ++serial_;
    invalidate();
  }

  void followTheme() {
    ThemeResolver* th = sh_.themes();
    if (!th || th->activeTheme() == themeName) return;
    std::string err;
    if (!build(&err)) {
      status = "theme: " + err;
      themeName = th->activeTheme();
    }
    invalidate();
  }

  // Document observers: bump the serial on every change so panels refresh their caches.
  void detachObservers() {
    if (docObserver_) {
      if (observedMgr_) observedMgr_->removeObserver(docObserver_.get());
      if (observedDoc_) observedDoc_->removeObserver(docObserver_.get());
    }
    observedMgr_ = nullptr;
    observedDoc_ = nullptr;
  }
  void watchDocuments() {
    if (!docObserver_) docObserver_ = sq::Ptr<IObserver>::adopt(new FnObserver([this](const char*) {
      ++serial_;
      invalidate();
    }));
    if (!observedMgr_) {
      if (IDocumentManager* m = sh_.documents()) {
        sq::Ptr<ISubject> s;
        if (m->queryInterface(&ISubject::iid, s.putVoid()) == SQ_OK && s) {
          s->addObserver(docObserver_.get());
          observedMgr_ = s;
        }
      }
    }
    auto d = sh_.activeDocument();
    sq_guid id{};
    if (d) d->getId(&id);
    if (id == lastDoc_) return;
    lastDoc_ = id;
    ++serial_;
    if (observedDoc_) observedDoc_->removeObserver(docObserver_.get());
    observedDoc_ = nullptr;
    if (d) {
      sq::Ptr<ISubject> s;
      if (d->queryInterface(&ISubject::iid, s.putVoid()) == SQ_OK && s) {
        s->addObserver(docObserver_.get());
        observedDoc_ = s;
      }
    }
    editorClip_ = sq_guid{};
  }

  std::string statusRight() {
    std::string s = sh_.deviceDescription();
    if (IAudioEngine* e = sh_.engine()) {
      sq_engine_status st{};
      st.struct_size = sizeof st;
      if (e->getStatus(&st) == SQ_OK && (st.flags & SQ_ENGINE_RUNNING)) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "   CPU %d%%", static_cast<int>(st.cpu_load * 100 + 0.5));
        s += buf;
        if (st.xruns) s += "   xruns " + std::to_string(st.xruns);
      }
    }
    return s;
  }

  // ---- input ----
  void layoutAndPaint(IRenderer& r) {
    tree.setScale(r.scale());
    tree.layout(static_cast<float>(r.size().width), static_cast<float>(r.size().height));
    viewW_ = static_cast<float>(r.size().width) / r.scale();
    viewH_ = static_cast<float>(r.size().height) / r.scale();
    r.clear(pal.bg);
    tree.paint(r);
    if (!overlays.empty()) {
      Canvas c(r, r.scale());
      overlays.paint(c, pal, Rect{0, 0, viewW_, viewH_});
    }
  }
  void mouse(const MouseEvent& pe) {
    if (!overlays.empty()) {
      const float s = tree.scale() > 0 ? tree.scale() : 1.0f;
      WidgetMouse e{pe.action == MouseAction::Move ? MouseAction::Hover : pe.action, pe.button, pe.x / s, pe.y / s, pe.wheel, pe.modifiers};
      overlays.mouse(e);
      invalidate();
      return;
    }
    if (tree.dispatchMouse(pe)) invalidate();
    if (pe.action == MouseAction::Up && !dragged_.empty()) {
      dragged_.clear();  // a drop nobody took
      invalidate();
    }
  }
  void key(Key k, unsigned mods) {
    if (overlays.key(k, mods)) {
      invalidate();
      return;
    }
    if (k == Key::Escape && !dragged_.empty()) {
      dragged_.clear();
      return;
    }
    if (tree.dispatchKey(k, mods)) {
      invalidate();
      return;
    }
    if (gui::handleShortcutKey(*this, k, mods)) invalidate();
  }
  void chr(uint32_t ch, unsigned mods) {
    if (overlays.chr(ch, mods)) {
      invalidate();
      return;
    }
    for (Widget* w = tree.focus(); w; w = w->parent())
      if (auto* ci = dynamic_cast<gui::CharInput*>(w); ci && ci->onChar(ch, mods)) {
        invalidate();
        return;
      }
    if (gui::handleShortcutChar(*this, ch, mods)) invalidate();
  }

  void idle() {
    sh_.idle();
    watchDocuments();
    followTheme();
  }
};

// The software renderer's font metrics (every window backend paints with it). Font size is in
// DIPs and so is the result (metrics scale linearly).
float Gui::measureDip(std::string_view t, float size) { return sq::ui::measureUiText(t, size); }

namespace {

// ---- headless runner: --screenshot / --ui-script ------------------------------------------------
std::vector<std::string> splitWords(const std::string& line) {
  std::vector<std::string> w;
  std::string cur;
  bool q = false;
  for (char ch : line) {
    if (ch == '"') q = !q;
    else if ((ch == ' ' || ch == '\t') && !q) {
      if (!cur.empty()) w.push_back(cur);
      cur.clear();
    } else cur.push_back(ch);
  }
  if (!cur.empty()) w.push_back(cur);
  return w;
}

unsigned parseMods(const std::vector<std::string>& w, size_t from) {
  unsigned m = 0;
  for (size_t i = from; i < w.size(); ++i) {
    if (w[i] == "ctrl") m |= kModCtrl;
    else if (w[i] == "shift") m |= kModShift;
    else if (w[i] == "alt") m |= kModAlt;
  }
  return m;
}

Key keyByName(const std::string& n) {
  static const std::pair<const char*, Key> kKeys[] = {
      {"escape", Key::Escape}, {"enter", Key::Enter},     {"space", Key::Space},   {"left", Key::Left},     {"right", Key::Right},
      {"up", Key::Up},         {"down", Key::Down},       {"tab", Key::Tab},       {"home", Key::Home},     {"end", Key::End},
      {"pageup", Key::PageUp}, {"pagedown", Key::PageDown}, {"plus", Key::Plus},   {"minus", Key::Minus},   {"zero", Key::Zero},
      {"delete", Key::Delete}, {"backspace", Key::Backspace}, {"insert", Key::Insert}, {"f1", Key::F1},    {"f2", Key::F2},
      {"f3", Key::F3},         {"f4", Key::F4},           {"f5", Key::F5},         {"f6", Key::F6},         {"f7", Key::F7},
      {"f8", Key::F8},         {"f9", Key::F9},           {"f10", Key::F10},       {"f11", Key::F11},       {"f12", Key::F12}};
  for (const auto& [name, k] : kKeys)
    if (n == name) return k;
  return Key::Unknown;
}

bool renderTo(Gui& gui, int w, int h, float scale, const std::string& path) {
  PixelBuffer pb;
  std::string err;
  if (!renderOffscreen(Backend::Software, w, h, scale, [&](IRenderer& r) { gui.layoutAndPaint(r); }, pb, &err)) {
    std::fprintf(stderr, "screenshot: %s\n", err.c_str());
    return false;
  }
  std::string png;
  if (!encodePngCompressed(pb, png, &err)) {
    std::fprintf(stderr, "screenshot: %s\n", err.c_str());
    return false;
  }
  if (sq::writeFile(path, png) != SQ_OK) {
    std::fprintf(stderr, "screenshot: cannot write %s\n", path.c_str());
    return false;
  }
  std::printf("screenshot: %s (%dx%d)\n", path.c_str(), w, h);
  return true;
}

}  // namespace

int runGuiOffscreen(Shell& shell, const GuiRunOptions& o) {
  Gui gui(shell);
  std::string err;
  if (!gui.build(&err)) {
    std::fprintf(stderr, "skin: %s\n", err.c_str());
    return 3;
  }
  if (!shell.activeDocument()) shell.execute("File.New");
  gui.watchDocuments();
  const int w = o.width > 0 ? o.width : 1600, h = o.height > 0 ? o.height : 1000;
  const float scale = o.scale > 0 ? o.scale : 1.0f;
  auto frame = [&] {
    gui.idle();
    PixelBuffer pb;
    renderOffscreen(Backend::Software, w, h, scale, [&](IRenderer& r) { gui.layoutAndPaint(r); }, pb, nullptr);
  };
  frame();
  int rc = 0;
  std::vector<std::string> lines;
  if (!o.script.empty()) {
    std::string text = o.script;
    if (auto f = sq::readFile(o.script); f && o.script.find('\n') == std::string::npos && o.script.find(';') == std::string::npos) text = *f;
    std::string cur;
    for (char ch : text) {
      if (ch == '\n' || ch == ';') {
        lines.push_back(cur);
        cur.clear();
      } else cur.push_back(ch);
    }
    lines.push_back(cur);
  }
  auto mouse = [&](MouseAction a, float x, float y, MouseButton b, unsigned m, float wheel = 0) {
    MouseEvent e;
    e.action = a;
    e.button = b;
    e.x = x * scale;
    e.y = y * scale;
    e.modifiers = m;
    e.wheel = wheel;
    gui.mouse(e);
  };
  for (const std::string& line : lines) {
    auto wds = splitWords(line);
    if (wds.empty() || wds[0][0] == '#') continue;
    const std::string& op = wds[0];
    auto num = [&](size_t i) { return i < wds.size() ? static_cast<float>(std::atof(wds[i].c_str())) : 0.0f; };
    if (op == "click" || op == "rclick" || op == "dclick") {
      const MouseButton b = op == "rclick" ? MouseButton::Right : MouseButton::Left;
      const unsigned m = parseMods(wds, 3);
      mouse(MouseAction::Move, num(1), num(2), MouseButton::None, m);
      mouse(MouseAction::Down, num(1), num(2), b, m);
      mouse(MouseAction::Up, num(1), num(2), b, m);
      if (op == "dclick") {
        mouse(MouseAction::DoubleClick, num(1), num(2), b, m);
        mouse(MouseAction::Up, num(1), num(2), b, m);
      }
    } else if (op == "move") {
      mouse(MouseAction::Move, num(1), num(2), MouseButton::None, parseMods(wds, 3));
    } else if (op == "down" || op == "up") {
      mouse(op == "down" ? MouseAction::Down : MouseAction::Up, num(1), num(2), MouseButton::Left, parseMods(wds, 3));
    } else if (op == "drag") {
      const unsigned m = parseMods(wds, 5);
      const int steps = 8;
      mouse(MouseAction::Move, num(1), num(2), MouseButton::None, m);
      mouse(MouseAction::Down, num(1), num(2), MouseButton::Left, m);
      for (int i = 1; i <= steps; ++i) {
        const float t = static_cast<float>(i) / steps;
        mouse(MouseAction::Move, num(1) + (num(3) - num(1)) * t, num(2) + (num(4) - num(2)) * t, MouseButton::Left, m);
      }
      mouse(MouseAction::Up, num(3), num(4), MouseButton::Left, m);
    } else if (op == "wheel") {
      mouse(MouseAction::Wheel, num(1), num(2), MouseButton::None, parseMods(wds, 4), num(3));
    } else if (op == "key") {
      const Key k = keyByName(wds.size() > 1 ? wds[1] : "");
      const unsigned m = parseMods(wds, 2);
      gui.key(k, m);
      // Keys that also produce characters on real windows (WM_CHAR): enter, escape, backspace, tab.
      if (k == Key::Enter) gui.chr(13, m);
      else if (k == Key::Backspace) gui.chr(8, m);
      else if (k == Key::Escape) gui.chr(27, m);
      else if (k == Key::Space) gui.chr(' ', m);
    } else if (op == "char") {
      const unsigned m = parseMods(wds, 2);
      uint32_t ch = wds.size() > 1 && !wds[1].empty() ? static_cast<unsigned char>(wds[1][0]) : 0;
      if (m & kModCtrl) ch = (ch >= 'a' && ch <= 'z') ? ch - 'a' + 1 : (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 1 : ch;
      gui.key(Key::Unknown, m);
      gui.chr(ch, m);
    } else if (op == "type") {
      std::string t = line.substr(line.find("type") + 5);
      for (unsigned char ch : t) gui.chr(ch, 0);
    } else if (op == "run") {
      std::string spec = line.substr(line.find("run") + 4);
      auto ws = splitWords(spec);
      if (!ws.empty()) {
        auto args = sq::makeAttributes();
        for (size_t i = 1; i < ws.size(); ++i) {
          size_t eq = ws[i].find('=');
          if (eq == std::string::npos) continue;
          std::string k = ws[i].substr(0, eq), v = ws[i].substr(eq + 1);
          char* end = nullptr;
          double d = std::strtod(v.c_str(), &end);
          sq_guid g{};
          if (!v.empty() && end && *end == 0) args->setDouble(k.c_str(), d);
          else if (v == "true" || v == "false") args->setBool(k.c_str(), v == "true" ? SQ_TRUE : SQ_FALSE);
          else if (sq::parseGuid(v, g)) args->setGuid(k.c_str(), &g);
          else args->setString(k.c_str(), v.c_str());
        }
        sq_result r = gui.run(ws[0], args.get());
        std::printf("ui-script run %s: %s%s%s\n", ws[0].c_str(), sq_result_string(r), shell.lastMessage().empty() ? "" : " - ",
                    shell.lastMessage().c_str());
      }
    } else if (op == "idle" || op == "wait") {
      const int ms = static_cast<int>(num(1));
      const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
      while (std::chrono::steady_clock::now() < until) {
        frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    } else if (op == "shot") {
      frame();
      if (!renderTo(gui, w, h, scale, wds.size() > 1 ? wds[1] : o.screenshot)) rc = 4;
    } else if (op == "drop" && wds.size() > 3) {  // drop X Y PATH: a browser file dropped on the arrange view
      if (Widget* a = gui.tree.findByName("arrange")) gui::arrangeDropFile(a, num(1), num(2), wds[3]);
    } else if (op == "print") {
      std::printf("ui-script: status=\"%s\"\n", gui.status.c_str());
    } else {
      std::fprintf(stderr, "ui-script: unknown op '%s'\n", op.c_str());
      rc = 5;
    }
    frame();
  }
  if (!o.screenshot.empty()) {
    frame();
    if (!renderTo(gui, w, h, scale, o.screenshot)) rc = 4;
  }
  gui.overlays.clear();
  return rc;
}

int runGui(Shell& shell, int quitAfterMs) {
  auto window = sq::ui::createWindow(sq::ui::WindowDesc{"Squizzy Studio", 1440, 900, true});
  if (!window) {
    std::printf("no window backend on this platform (%s); use --headless or --screenshot\n", sq::ui::defaultBackendName());
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
  gui.watchDocuments();
  gui.status = "Ready.  Space: play/stop   R: record   Ctrl+Z/Y: undo/redo   Double-click a clip to edit it";

  sq::ui::WindowCallbacks cb;
  cb.onPaint = [&](sq::ui::IRenderer& r) { gui.layoutAndPaint(r); };
  cb.onMouse = [&](const sq::ui::MouseEvent& e) { gui.mouse(e); };
  cb.onKeyEx = [&](sq::ui::Key k, unsigned mods) {
    if (k == sq::ui::Key::F5 && (mods & kModCtrl)) {
      int ok = 0, busy = 0;
      for (const auto& m : shell.loader.modules()) {
        if (shell.loader.reload(m.name).ok()) ++ok;
        else ++busy;
      }
      gui.status = "reloaded " + std::to_string(ok) + " service(s), " + std::to_string(busy) + " failed/busy";
      window->invalidate();
      return;
    }
    gui.key(k, mods);
  };
  cb.onChar = [&](uint32_t ch, unsigned mods) { gui.chr(ch, mods); };
  cb.onCloseRequest = [&]() {
    auto d = shell.activeDocument();
    sq_document_info di{};
    di.struct_size = sizeof di;
    if (d && d->getDocumentInfo(&di) == SQ_OK && di.modified && !gui.quitConfirmed) {
      gui.confirm("The song has unsaved changes. Quit without saving?", [&] {
        gui.quitConfirmed = true;
        window->close();
      });
      return false;
    }
    return true;
  };
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
      window->setAccessibilityRoot(nullptr);
      break;
    }
    gui.idle();
    uint64_t now = sq_time_monotonic_ns();
    if (now - lastRepaint > 33000000ull) {  // ~30 fps playhead / meters
      lastRepaint = now;
      window->invalidate();
    }
    sq_thread_sleep_ms(4);
  }
  gui.overlays.clear();
  return 0;
}

}  // namespace sq::app
