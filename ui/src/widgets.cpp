// Retained widget tree: geometry, hit-testing, event dispatch, painting of the built-in widgets.
#include "sq/ui/widgets.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace sq::ui {

namespace {
std::atomic<uint32_t> g_nextAccId{1};
constexpr float kPi = 3.14159265358979f;

Point polar(Point c, float r, float deg) {
  float a = deg * kPi / 180.0f;
  return Point{c.x + r * std::sin(a), c.y - r * std::cos(a)};
}

Color mix(Color a, Color b, float t) {
  return Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}
}  // namespace

AccessibleNode* findAccessible(AccessibleNode* root, uint32_t id) {
  if (!root) return nullptr;
  if (root->accessibleId() == id) return root;
  for (size_t i = 0, n = root->accessibleChildCount(); i < n; ++i)
    if (AccessibleNode* f = findAccessible(root->accessibleChild(i), id)) return f;
  return nullptr;
}

// ---- Widget ------------------------------------------------------------------------------------

Widget::Widget(std::string type) : type_(std::move(type)), accId_(g_nextAccId.fetch_add(1, std::memory_order_relaxed)) {}

Widget::~Widget() {
  children_.clear();
  if (WidgetTree* t = tree()) t->widgetRemoved(*this);
}

Widget* Widget::addChild(std::unique_ptr<Widget> w) {
  if (!w) return nullptr;
  w->parent_ = this;
  children_.push_back(std::move(w));
  return children_.back().get();
}

void Widget::clearChildren() {
  children_.clear();
  invalidate();
}

WidgetTree* Widget::tree() const {
  const Widget* w = this;
  while (w->parent_) w = w->parent_;
  return w->tree_;
}

Widget* Widget::findByName(std::string_view n) {
  if (name == n) return this;
  for (auto& c : children_)
    if (Widget* f = c->findByName(n)) return f;
  return nullptr;
}

void Widget::forEach(const std::function<void(Widget&)>& fn) {
  fn(*this);
  for (auto& c : children_) c->forEach(fn);
}

void Widget::setValue(double v, bool notify) {
  if (std::isnan(v)) return;
  double lo = std::min(minValue, maxValue), hi = std::max(minValue, maxValue);
  v = std::clamp(v, lo, hi);
  if (v == value_) return;
  value_ = v;
  if (WidgetTree* t = tree()) t->widgetChanged(*this, notify);
}

void Widget::invalidate() {
  if (WidgetTree* t = tree()) t->widgetChanged(*this, false);
}

bool Widget::setRangeValue(double v) {
  if (!hasValue() || !enabled) return false;
  setValue(v, true);
  return true;
}

bool Widget::onKey(Key k, unsigned mods) {
  if (!hasValue() || !enabled) return false;
  double range = maxValue - minValue;
  double step = range * ((mods & kModShift) ? 0.001 : 0.01);
  switch (k) {
    case Key::Up:
    case Key::Right: setValue(value_ + step, true); return true;
    case Key::Down:
    case Key::Left: setValue(value_ - step, true); return true;
    case Key::PageUp: setValue(value_ + range * 0.1, true); return true;
    case Key::PageDown: setValue(value_ - range * 0.1, true); return true;
    case Key::Home: setValue(minValue, true); return true;
    case Key::End: setValue(maxValue, true); return true;
    case Key::Delete:
    case Key::Zero: setValue(defaultValue, true); return true;
    default: return false;
  }
}

std::string Widget::accessibleName() const {
  if (!text.empty()) return text;
  if (!tooltip.empty()) return tooltip;
  return name;
}

Rect Widget::accessibleBounds() const {
  WidgetTree* t = tree();
  float s = t ? t->scale() : 1.0f;
  return Rect{frame_.x * s, frame_.y * s, frame_.w * s, frame_.h * s};
}

size_t Widget::accessibleChildCount() const {
  size_t n = 0;
  for (auto& c : children_)
    if (c->style.display) ++n;
  return n;
}

AccessibleNode* Widget::accessibleChild(size_t i) const {
  for (auto& c : children_) {
    if (!c->style.display) continue;
    if (i-- == 0) return c.get();
  }
  return nullptr;
}

void Widget::paintBox(Canvas& c) {
  if (visual.background.a > 0) {
    if (visual.radius > 0) c.fillRoundRect(frame_, visual.radius, visual.background);
    else c.fillRect(frame_, visual.background);
  }
  if (visual.borderWidth > 0 && visual.border.a > 0) {
    Rect r = frame_.inset(visual.borderWidth / 2);
    if (visual.radius > 0) c.strokeRoundRect(r, visual.radius, visual.border, visual.borderWidth);
    else c.strokeRect(r, visual.border, visual.borderWidth);
  }
}

void Widget::paint(Canvas& c) { paintBox(c); }

// ---- Label -------------------------------------------------------------------------------------

Label::Label() : Widget("Label") {
  hitTestable = false;
  measure = [this](float, float, float& w, float& h) {
    WidgetTree* t = tree();
    w = t ? t->measureText(text, visual.fontSize) : static_cast<float>(text.size()) * visual.fontSize * 0.55f;
    h = std::ceil(visual.fontSize * 1.4f);
  };
}

void Label::paint(Canvas& c) {
  paintBox(c);
  if (text.empty()) return;
  const Rect& f = frame();
  float lineH = visual.fontSize * 1.4f;
  Rect r{f.x + style.padding.left, f.y + (f.h - lineH) / 2, f.w - style.padding.horizontal(), lineH};
  c.drawText(text, r, enabled ? visual.foreground : visual.foreground.withAlpha(visual.foreground.a * 0.45f), visual.fontSize, visual.textAlign);
}

// ---- Knob --------------------------------------------------------------------------------------

Knob::Knob() : Widget("Knob") {
  measure = [](float, float, float& w, float& h) { w = h = 36; };
}

void Knob::paint(Canvas& c) {
  paintBox(c);
  const Rect& f = frame();
  float d = std::min(f.w, f.h);
  if (d <= 4) return;
  Point ctr{f.x + f.w / 2, f.y + f.h / 2};
  float trackW = std::max(2.0f, d * 0.09f);
  float r = d / 2 - trackW / 2 - 1;
  const float startDeg = -135, sweepDeg = 270;
  float t = static_cast<float>(normalized());
  float valueDeg = startDeg + sweepDeg * t;
  c.drawArc(ctr, r, startDeg, sweepDeg, visual.track, trackW);
  float from = bipolar ? 0.0f : startDeg;
  if (std::fabs(valueDeg - from) > 0.01f) {
    float a0 = std::min(from, valueDeg), a1 = std::max(from, valueDeg);
    c.drawArc(ctr, r, a0, a1 - a0, enabled ? visual.accent : visual.track, trackW);
  }
  float body = r - trackW * 1.6f;
  Rect bodyRect{ctr.x - body, ctr.y - body, body * 2, body * 2};
  ImagePtr img;
  if (!visual.image.empty())
    if (WidgetTree* tr = tree()) img = tr->image(visual.image);
  if (img) {
    c.drawImage(*img, bodyRect, visual.opacity);
  } else {
    c.fillEllipse(bodyRect, mix(visual.track, visual.thumb, 0.25f));
    c.strokeEllipse(bodyRect.inset(0.5f), mix(visual.track, visual.thumb, 0.45f), 1);
  }
  c.drawLine(polar(ctr, body * 0.3f, valueDeg), polar(ctr, body * 0.9f, valueDeg), visual.thumb, std::max(1.5f, d * 0.05f));
}

bool Knob::onMouse(const WidgetMouse& e) {
  if (!enabled) return false;
  switch (e.action) {
    case MouseAction::Down:
      if (e.button != MouseButton::Left) return false;
      dragY_ = e.y;
      dragValue_ = value_;
      return true;
    case MouseAction::Move: {
      double range = maxValue - minValue;
      double perDip = range / (dragPixelsPerRange * ((e.modifiers & kModShift) ? 10.0 : 1.0));
      setValue(dragValue_ + (dragY_ - e.y) * perDip, true);
      return true;
    }
    case MouseAction::Up: return true;
    case MouseAction::Wheel: setValue(value_ + e.wheel * (maxValue - minValue) * 0.02, true); return true;
    case MouseAction::DoubleClick: setValue(defaultValue, true); return true;
    default: return false;
  }
}

// ---- Fader -------------------------------------------------------------------------------------

Fader::Fader() : Widget("Fader") {
  measure = [this](float, float, float& w, float& h) {
    w = horizontal ? 120.0f : 28.0f;
    h = horizontal ? 28.0f : 120.0f;
  };
}

static Rect thumbRect(const Rect& f, bool horizontal, float t) {
  if (horizontal) {
    float tw = std::min(14.0f, f.w * 0.25f);
    float x = f.x + (f.w - tw) * t;
    return Rect{x, f.y + f.h * 0.1f, tw, f.h * 0.8f};
  }
  float th = std::min(18.0f, f.h * 0.2f);
  float y = f.y + (f.h - th) * (1.0f - t);
  return Rect{f.x + f.w * 0.1f, y, f.w * 0.8f, th};
}

void Fader::paint(Canvas& c) {
  paintBox(c);
  const Rect& f = frame();
  float t = static_cast<float>(normalized());
  Rect th = thumbRect(f, horizontal, t);
  if (horizontal) {
    Rect slot{f.x + 2, f.y + f.h / 2 - 2, f.w - 4, 4};
    c.fillRoundRect(slot, 2, visual.track);
    c.fillRoundRect(Rect{slot.x, slot.y, th.x + th.w / 2 - slot.x, slot.h}, 2, visual.accent);
  } else {
    Rect slot{f.x + f.w / 2 - 2, f.y + 2, 4, f.h - 4};
    c.fillRoundRect(slot, 2, visual.track);
    float top = th.y + th.h / 2;
    c.fillRoundRect(Rect{slot.x, top, slot.w, slot.bottom() - top}, 2, enabled ? visual.accent : visual.track);
  }
  c.fillRoundRect(th, 3, visual.thumb);
  if (horizontal) c.drawLine(Point{th.x + th.w / 2, th.y + 3}, Point{th.x + th.w / 2, th.bottom() - 3}, visual.track, 1.5f);
  else c.drawLine(Point{th.x + 3, th.y + th.h / 2}, Point{th.right() - 3, th.y + th.h / 2}, visual.track, 1.5f);
}

void Fader::setFromPoint(float x, float y) {
  const Rect& f = frame();
  Rect th = thumbRect(f, horizontal, 0);
  double t;
  if (horizontal) t = (x - f.x - th.w / 2) / std::max(1.0f, f.w - th.w);
  else t = 1.0 - (y - f.y - th.h / 2) / std::max(1.0f, f.h - th.h);
  setValue(minValue + std::clamp(t, 0.0, 1.0) * (maxValue - minValue), true);
}

bool Fader::onMouse(const WidgetMouse& e) {
  if (!enabled) return false;
  switch (e.action) {
    case MouseAction::Down:
      if (e.button != MouseButton::Left) return false;
      setFromPoint(e.x, e.y);
      return true;
    case MouseAction::Move: setFromPoint(e.x, e.y); return true;
    case MouseAction::Up: return true;
    case MouseAction::Wheel: setValue(value_ + e.wheel * (maxValue - minValue) * 0.02, true); return true;
    case MouseAction::DoubleClick: setValue(defaultValue, true); return true;
    default: return false;
  }
}

// ---- Button ------------------------------------------------------------------------------------

Button::Button() : Widget("Button") {
  visual.radius = 4;
  visual.textAlign = TextAlign::Center;
  measure = [this](float, float, float& w, float& h) {
    WidgetTree* t = tree();
    float tw = t ? t->measureText(text, visual.fontSize) : static_cast<float>(text.size()) * visual.fontSize * 0.55f;
    w = std::max(24.0f, tw + 16);
    h = std::ceil(visual.fontSize * 1.4f) + 8;
  };
}

void Button::paint(Canvas& c) {
  const Rect& f = frame();
  bool on = toggled();
  Color bg = on ? visual.accent : (visual.background.a > 0 ? visual.background : visual.track);
  if (pressed_) bg = mix(bg, Color::rgb(0x000000), 0.25f);
  if (!enabled) bg = bg.withAlpha(bg.a * 0.5f);
  c.fillRoundRect(f, visual.radius, bg);
  if (visual.borderWidth > 0 && visual.border.a > 0) c.strokeRoundRect(f.inset(visual.borderWidth / 2), visual.radius, visual.border, visual.borderWidth);
  if (!text.empty()) {
    float lineH = visual.fontSize * 1.4f;
    Color fg = on ? mix(visual.foreground, Color::rgb(0x101114), 0.85f) : visual.foreground;
    c.drawText(text, Rect{f.x + 2, f.y + (f.h - lineH) / 2, f.w - 4, lineH}, fg, visual.fontSize, visual.textAlign);
  }
}

bool Button::toggle() {
  if (!toggles || !enabled) return false;
  setValue(value_ >= 0.5 ? 0.0 : 1.0, true);
  return true;
}

bool Button::invoke() {
  if (!enabled) return false;
  if (toggles) toggle();
  if (WidgetTree* t = tree())
    if (t->onCommand) t->onCommand(*this);
  return true;
}

bool Button::onMouse(const WidgetMouse& e) {
  if (!enabled) return false;
  switch (e.action) {
    case MouseAction::Down:
      if (e.button != MouseButton::Left) return false;
      pressed_ = true;
      invalidate();
      return true;
    case MouseAction::Up: {
      bool was = pressed_;
      pressed_ = false;
      invalidate();
      if (was && frame().contains(e.x, e.y)) invoke();
      return true;
    }
    case MouseAction::Move: return pressed_;
    default: return false;
  }
}

// ---- Meter -------------------------------------------------------------------------------------

Meter::Meter() : Widget("Meter") {
  hitTestable = false;
  measure = [this](float, float, float& w, float& h) {
    w = horizontal ? 120.0f : 8.0f;
    h = horizontal ? 8.0f : 120.0f;
  };
}

void Meter::paint(Canvas& c) {
  const Rect& f = frame();
  c.fillRoundRect(f, std::min(2.0f, std::min(f.w, f.h) / 2), visual.track);
  float t = static_cast<float>(std::clamp(normalized(), 0.0, 1.0));
  const float hotFrom = 0.85f;
  if (t <= 0) return;
  if (horizontal) {
    float cold = std::min(t, hotFrom);
    c.fillRect(Rect{f.x, f.y, f.w * cold, f.h}, visual.accent);
    if (t > hotFrom) c.fillRect(Rect{f.x + f.w * hotFrom, f.y, f.w * (t - hotFrom), f.h}, hot);
  } else {
    float cold = std::min(t, hotFrom);
    c.fillRect(Rect{f.x, f.bottom() - f.h * cold, f.w, f.h * cold}, visual.accent);
    if (t > hotFrom) c.fillRect(Rect{f.x, f.bottom() - f.h * t, f.w, f.h * (t - hotFrom)}, hot);
  }
}

// ---- ImageView ---------------------------------------------------------------------------------

ImageView::ImageView() : Widget("Image") {
  hitTestable = false;
  measure = [this](float, float, float& w, float& h) {
    w = h = 16;
    if (WidgetTree* t = tree())
      if (ImagePtr img = t->image(visual.image)) {
        w = static_cast<float>(img->width()) / img->density();
        h = static_cast<float>(img->height()) / img->density();
      }
  };
}

void ImageView::paint(Canvas& c) {
  paintBox(c);
  WidgetTree* t = tree();
  ImagePtr img = t ? t->image(visual.image) : nullptr;
  if (!img || img->width() <= 0 || img->height() <= 0) return;
  const Rect& f = frame();
  float iw = static_cast<float>(img->width()) / img->density(), ih = static_cast<float>(img->height()) / img->density();
  float k = std::min(f.w / iw, f.h / ih);
  Rect dst{f.x + (f.w - iw * k) / 2, f.y + (f.h - ih * k) / 2, iw * k, ih * k};
  c.drawImage(*img, dst, visual.opacity);
}

// ---- WidgetTree --------------------------------------------------------------------------------

WidgetTree::WidgetTree() = default;

WidgetTree::~WidgetTree() {
  capture_ = hover_ = focus_ = nullptr;
  root_.reset();
}

void WidgetTree::setRoot(std::unique_ptr<Widget> root) {
  capture_ = hover_ = focus_ = nullptr;
  if (root_) root_->tree_ = nullptr;
  root_ = std::move(root);
  if (root_) {
    root_->parent_ = nullptr;
    root_->tree_ = this;
  }
  if (onInvalidate) onInvalidate();
}

std::unique_ptr<Widget> WidgetTree::takeRoot() {
  capture_ = hover_ = focus_ = nullptr;
  if (root_) root_->tree_ = nullptr;
  return std::move(root_);
}

void WidgetTree::setScale(float s) { scale_ = s > 0.05f ? s : 1.0f; }

static void computeFrames(Widget& w, float ox, float oy, void (*setFrame)(Widget&, const Rect&)) {
  Rect f{ox + w.box.x, oy + w.box.y, w.box.w, w.box.h};
  setFrame(w, f);
  for (size_t i = 0; i < w.childCount(); ++i) computeFrames(*w.child(i), f.x, f.y, setFrame);
}

void WidgetTree::layout(float widthPx, float heightPx) {
  if (!root_) return;
  computeLayout(*root_, widthPx / scale_, heightPx / scale_);
  computeFrames(*root_, 0, 0, [](Widget& w, const Rect& r) { w.frame_ = r; });
}

void WidgetTree::paintRec(Widget& w, Canvas& c) {
  if (!w.style.display) return;
  w.paint(c);
  for (size_t i = 0; i < w.childCount(); ++i) paintRec(*w.child(i), c);
  if (focus_ == &w) c.strokeRoundRect(w.frame().inset(-1), 3, w.visual.accent.withAlpha(0.8f), 1);
}

void WidgetTree::paint(IRenderer& r) {
  if (!root_) return;
  Canvas c(r, scale_);
  paintRec(*root_, c);
}

Widget* WidgetTree::hitRec(Widget& w, float x, float y) const {
  if (!w.style.display || !w.frame().contains(x, y)) return nullptr;
  for (size_t i = w.childCount(); i-- > 0;)
    if (Widget* h = hitRec(*w.child(i), x, y)) return h;
  return w.hitTestable ? &w : nullptr;
}

Widget* WidgetTree::hitTest(float xDip, float yDip) const { return root_ ? hitRec(*root_, xDip, yDip) : nullptr; }

void WidgetTree::setFocus(Widget* w) {
  if (focus_ == w) return;
  focus_ = w;
  if (onInvalidate) onInvalidate();
}

bool WidgetTree::dispatchMouse(const MouseEvent& pe) {
  if (!root_) return false;
  WidgetMouse e{pe.action, pe.button, pe.x / scale_, pe.y / scale_, pe.wheel, pe.modifiers};
  auto bubble = [&](Widget* w) -> Widget* {
    for (; w; w = w->parent())
      if (w->onMouse(e)) return w;
    return nullptr;
  };
  bool changed = false;
  switch (pe.action) {
    case MouseAction::Move:
      if (capture_) return capture_->onMouse(e);
      {
        Widget* h = hitTest(e.x, e.y);
        if (h != hover_) {
          hover_ = h;
          changed = true;
        }
        WidgetMouse he = e;
        he.action = MouseAction::Hover;
        for (Widget* w = h; w; w = w->parent())
          if (w->onMouse(he)) return true;
      }
      return changed;
    case MouseAction::Leave:
      changed = hover_ != nullptr;
      hover_ = nullptr;
      return changed;
    case MouseAction::Down: {
      Widget* target = bubble(hitTest(e.x, e.y));
      capture_ = target;
      Widget* f = target;
      while (f && !f->focusable()) f = f->parent();
      setFocus(f);
      return target != nullptr;
    }
    case MouseAction::Up:
      if (capture_) {
        Widget* c = capture_;
        capture_ = nullptr;
        return c->onMouse(e);
      }
      return bubble(hitTest(e.x, e.y)) != nullptr;
    case MouseAction::Wheel:
    case MouseAction::DoubleClick: return bubble(hitTest(e.x, e.y)) != nullptr;
    case MouseAction::Hover: return false;
  }
  return false;
}

bool WidgetTree::dispatchKey(Key k, unsigned mods) {
  if (k == Key::Tab && root_) {  // cycle focus through focusable widgets
    std::vector<Widget*> order;
    root_->forEach([&](Widget& w) {
      if (w.focusable() && w.enabled && w.style.display) order.push_back(&w);
    });
    if (order.empty()) return false;
    auto it = std::find(order.begin(), order.end(), focus_);
    size_t idx = it == order.end() ? 0 : static_cast<size_t>(it - order.begin());
    if (it != order.end()) idx = (mods & kModShift) ? (idx + order.size() - 1) % order.size() : (idx + 1) % order.size();
    setFocus(order[idx]);
    return true;
  }
  for (Widget* w = focus_; w; w = w->parent())
    if (w->onKey(k, mods)) return true;
  return false;
}

void WidgetTree::widgetChanged(Widget& w, bool userDriven) {
  if (userDriven && onValueChanged) onValueChanged(w);
  if (onAccessibilityChanged) onAccessibilityChanged(w);
  if (onInvalidate) onInvalidate();
}

void WidgetTree::widgetRemoved(Widget& w) {
  if (capture_ == &w) capture_ = nullptr;
  if (hover_ == &w) hover_ = nullptr;
  if (focus_ == &w) focus_ = nullptr;
}

float WidgetTree::measureText(std::string_view text, float sizeDip) const {
  if (textMeasurer) return textMeasurer(text, sizeDip);
  return static_cast<float>(text.size()) * sizeDip * 0.55f;
}

ImagePtr WidgetTree::image(const std::string& url) const {
  if (url.empty() || !imageProvider) return nullptr;
  return imageProvider(url, scale_);
}

}  // namespace sq::ui
