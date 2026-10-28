// Squizzy Studio UI - retained-mode widget tree (backend-neutral).
//
// Widgets are layout nodes (flexbox, layout.hpp) and accessibility nodes (accessibility.hpp).
// Geometry is in DIPs; WidgetTree converts to pixels with scale = dpiScale * zoom. Typical use:
//   WidgetTree tree; tree.setRoot(buildForm(...));         // skin.hpp
//   tree.setScale(window.dpiScale() * zoom);
//   tree.layout(widthPx, heightPx); tree.paint(renderer); tree.dispatchMouse(ev);
// Everything here runs on the UI thread.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sq/ui/accessibility.hpp"
#include "sq/ui/image.hpp"
#include "sq/ui/layout.hpp"
#include "sq/ui/ui.hpp"

namespace sq::ui {

// DIP drawing on top of a pixel renderer.
class Canvas {
 public:
  Canvas(IRenderer& r, float scale) : r_(r), s_(scale > 0 ? scale : 1.0f) {}
  IRenderer& renderer() const { return r_; }
  float scale() const { return s_; }
  Rect px(const Rect& d) const { return Rect{d.x * s_, d.y * s_, d.w * s_, d.h * s_}; }
  Point px(Point p) const { return Point{p.x * s_, p.y * s_}; }

  void fillRect(const Rect& r, Color c) { r_.fillRect(px(r), c); }
  void strokeRect(const Rect& r, Color c, float w) { r_.strokeRect(px(r), c, w * s_); }
  void fillRoundRect(const Rect& r, float rad, Color c) { r_.fillRoundRect(px(r), rad * s_, c); }
  void strokeRoundRect(const Rect& r, float rad, Color c, float w) { r_.strokeRoundRect(px(r), rad * s_, c, w * s_); }
  void fillEllipse(const Rect& r, Color c) { r_.fillEllipse(px(r), c); }
  void strokeEllipse(const Rect& r, Color c, float w) { r_.strokeEllipse(px(r), c, w * s_); }
  void drawLine(Point a, Point b, Color c, float w) { r_.drawLine(px(a), px(b), c, w * s_); }
  void drawArc(Point ctr, float radius, float start, float sweep, Color c, float w) { r_.drawArc(px(ctr), radius * s_, start, sweep, c, w * s_); }
  void drawText(std::string_view t, const Rect& r, Color c, float size, TextAlign a = TextAlign::Left) { r_.drawText(t, px(r), c, size * s_, a); }
  void drawTextBold(std::string_view t, const Rect& r, Color c, float size, TextAlign a = TextAlign::Left) {
    r_.drawTextStyled(t, px(r), c, size * s_, a, true);
  }
  void fillPolygon(const std::vector<Point>& pts, Color c) {
    std::vector<Point> q(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) q[i] = px(pts[i]);
    r_.fillPolygon(q.data(), q.size(), c);
  }
  void strokePolyline(const std::vector<Point>& pts, Color c, float w) {
    std::vector<Point> q(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) q[i] = px(pts[i]);
    r_.strokePolyline(q.data(), q.size(), c, w * s_);
  }
  // Text width in DIPs.
  float measureText(std::string_view t, float size) { return r_.measureText(t, size * s_) / s_; }
  void drawImage(const Image& img, const Rect& r, float opacity = 1.0f) { r_.drawImage(img, px(r), opacity); }
  void pushClip(const Rect& r) { r_.pushClip(px(r)); }
  void popClip() { r_.popClip(); }

 private:
  IRenderer& r_;
  float s_;
};

struct WidgetMouse {
  MouseAction action = MouseAction::Move;
  MouseButton button = MouseButton::None;
  float x = 0, y = 0;  // DIPs, tree coordinates
  float wheel = 0;
  unsigned modifiers = 0;
};

struct Visual {
  Color background{0, 0, 0, 0};
  Color border{0, 0, 0, 0};
  float borderWidth = 0;
  float radius = 0;
  Color foreground = Color::rgb(0xE6E6E6);  // text
  Color accent = Color::rgb(0x4FB3A9);
  Color track = Color::rgb(0x3A3D44);
  Color thumb = Color::rgb(0xD8DADF);
  float fontSize = 12;
  TextAlign textAlign = TextAlign::Left;
  std::string image;  // theme:// URL
  float opacity = 1;
};

struct Binding {
  std::string url;                 // object:// URL (absolute after skin instantiation)
  std::string property = "value";  // IController property
  bool empty() const { return url.empty(); }
};

class WidgetTree;

class Widget : public LayoutNode, public AccessibleNode {
 public:
  explicit Widget(std::string type);
  ~Widget() override;
  Widget(const Widget&) = delete;
  Widget& operator=(const Widget&) = delete;

  const std::string& type() const { return type_; }
  std::string name;   // skin name (lookup key)
  std::string text;   // label / caption / accessible name fallback
  std::string tooltip;
  std::string remoteId;  // hardware control id (remote pages)
  std::string remoteType;  // remote control type override: knob encoder fader button pad display led
  bool remoteFeedback = false;  // surface shows the value (motor fader, LED ring)
  std::string command;   // command a button fires ("Transport.Play"); empty = none
  Visual visual;
  Binding target;
  bool enabled = true;
  bool hitTestable = true;

  Widget* parent() const { return parent_; }
  size_t childCount() const { return children_.size(); }
  Widget* child(size_t i) const { return children_[i].get(); }
  Widget* addChild(std::unique_ptr<Widget> w);
  // Destroys every child (the tree drops hover/focus/capture pointers into them).
  void clearChildren();
  WidgetTree* tree() const;
  Widget* findByName(std::string_view n);
  void forEach(const std::function<void(Widget&)>& fn);

  // Absolute frame in DIPs (valid after WidgetTree::layout).
  const Rect& frame() const { return frame_; }

  // Value model (knobs, faders, toggle buttons, meters). Values live in [minValue, maxValue].
  virtual bool hasValue() const { return false; }
  double value() const { return value_; }
  // Clamps, stores, repaints; notify=true reports the change to WidgetTree::onValueChanged.
  void setValue(double v, bool notify = false);
  double minValue = 0, maxValue = 1, defaultValue = 0;
  double normalized() const { return maxValue > minValue ? (value_ - minValue) / (maxValue - minValue) : 0.0; }

  virtual void paint(Canvas& c);  // self only; children are painted by the tree
  virtual bool onMouse(const WidgetMouse& e) { (void)e; return false; }
  virtual bool onKey(Key k, unsigned mods);
  virtual bool focusable() const { return false; }

  // AccessibleNode
  uint32_t accessibleId() const override { return accId_; }
  AccessibleRole accessibleRole() const override { return AccessibleRole::Group; }
  std::string accessibleName() const override;
  std::string accessibleHelp() const override { return tooltip; }
  Rect accessibleBounds() const override;
  size_t accessibleChildCount() const override;
  AccessibleNode* accessibleChild(size_t i) const override;
  AccessibleNode* accessibleParent() const override { return parent_; }
  bool accessibleEnabled() const override { return enabled; }
  bool hasRangeValue() const override { return hasValue(); }
  double rangeValue() const override { return value_; }
  double rangeMin() const override { return minValue; }
  double rangeMax() const override { return maxValue; }
  bool rangeReadOnly() const override { return !enabled; }
  bool setRangeValue(double v) override;

  // LayoutNode
  size_t layoutChildCount() const override { return children_.size(); }
  LayoutNode* layoutChild(size_t i) const override { return children_[i].get(); }

 protected:
  void invalidate();
  void paintBox(Canvas& c);
  double value_ = 0;

 private:
  friend class WidgetTree;
  std::string type_;
  Widget* parent_ = nullptr;
  WidgetTree* tree_ = nullptr;  // set on the root only
  std::vector<std::unique_ptr<Widget>> children_;
  Rect frame_;
  uint32_t accId_;
};

class View : public Widget {
 public:
  View() : Widget("View") {}
  explicit View(std::string type) : Widget(std::move(type)) {}
};

class Label : public Widget {
 public:
  Label();
  void paint(Canvas& c) override;
  AccessibleRole accessibleRole() const override { return AccessibleRole::Text; }
};

class Knob : public Widget {
 public:
  Knob();
  bool hasValue() const override { return true; }
  bool bipolar = false;
  float dragPixelsPerRange = 200;  // DIPs of vertical drag for the full range
  void paint(Canvas& c) override;
  bool onMouse(const WidgetMouse& e) override;
  bool focusable() const override { return true; }
  AccessibleRole accessibleRole() const override { return AccessibleRole::Slider; }

 private:
  float dragY_ = 0;
  double dragValue_ = 0;
};

class Fader : public Widget {
 public:
  Fader();
  bool hasValue() const override { return true; }
  bool horizontal = false;
  void paint(Canvas& c) override;
  bool onMouse(const WidgetMouse& e) override;
  bool focusable() const override { return true; }
  AccessibleRole accessibleRole() const override { return AccessibleRole::Slider; }

 private:
  void setFromPoint(float x, float y);
};

class Button : public Widget {
 public:
  Button();
  bool toggles = false;  // latching (mute/solo) vs momentary
  bool hasValue() const override { return toggles; }
  void paint(Canvas& c) override;
  bool onMouse(const WidgetMouse& e) override;
  bool focusable() const override { return true; }
  AccessibleRole accessibleRole() const override { return toggles ? AccessibleRole::ToggleButton : AccessibleRole::Button; }
  bool isToggle() const override { return toggles; }
  bool toggled() const override { return toggles && value_ >= 0.5; }
  bool toggle() override;
  bool invoke() override;

 private:
  bool pressed_ = false;
};

class Meter : public Widget {
 public:
  Meter();
  bool hasValue() const override { return true; }
  bool horizontal = false;
  Color hot = Color::rgb(0xE0604A);
  void paint(Canvas& c) override;
  AccessibleRole accessibleRole() const override { return AccessibleRole::Meter; }
  bool rangeReadOnly() const override { return true; }
};

class ImageView : public Widget {
 public:
  ImageView();
  void paint(Canvas& c) override;
  AccessibleRole accessibleRole() const override { return AccessibleRole::Image; }
};

class WidgetTree {
 public:
  WidgetTree();
  ~WidgetTree();
  WidgetTree(const WidgetTree&) = delete;
  WidgetTree& operator=(const WidgetTree&) = delete;

  void setRoot(std::unique_ptr<Widget> root);
  std::unique_ptr<Widget> takeRoot();
  Widget* root() const { return root_.get(); }

  // scale = dpiScale * zoom (pixels per DIP).
  void setScale(float s);
  float scale() const { return scale_; }

  // Lays the root out into a pixel viewport. Returns the root size in DIPs.
  void layout(float widthPx, float heightPx);
  void paint(IRenderer& r);
  Widget* hitTest(float xDip, float yDip) const;
  // Returns true if anything changed (caller should repaint).
  bool dispatchMouse(const MouseEvent& pixelEvent);
  bool dispatchKey(Key k, unsigned mods);
  Widget* focus() const { return focus_; }
  void setFocus(Widget* w);
  Widget* hover() const { return hover_; }
  Widget* findByName(std::string_view n) const { return root_ ? root_->findByName(n) : nullptr; }

  // Hooks (all optional).
  std::function<void(Widget&)> onValueChanged;  // user-driven value changes
  std::function<void(Widget&)> onCommand;       // button clicks
  std::function<void()> onInvalidate;           // something wants a repaint
  std::function<void(Widget&)> onAccessibilityChanged;
  std::function<ImagePtr(const std::string& url, float scale)> imageProvider;
  // Text width in DIPs for layout of labels (default: an approximation).
  std::function<float(std::string_view text, float sizeDip)> textMeasurer;

  // Internal: called by widgets.
  void widgetChanged(Widget& w, bool userDriven);
  void widgetRemoved(Widget& w);
  float measureText(std::string_view text, float sizeDip) const;
  ImagePtr image(const std::string& url) const;

 private:
  void paintRec(Widget& w, Canvas& c);
  Widget* hitRec(Widget& w, float x, float y) const;
  std::unique_ptr<Widget> root_;
  float scale_ = 1.0f;
  Widget* capture_ = nullptr;
  Widget* hover_ = nullptr;
  Widget* focus_ = nullptr;
};

// Rounds a value to the device pixel grid of `scale`.
inline float snapToPixel(float dip, float scale) { return static_cast<float>(static_cast<int>(dip * scale + 0.5f)) / scale; }

}  // namespace sq::ui
