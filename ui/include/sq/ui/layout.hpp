// Squizzy Studio UI - flexbox layout engine (own implementation, no third-party code).
//
// Supported: row/column (+reverse), justify-content (start/end/center/space-between/-around/
// -evenly), align-items / align-self (start/end/center/stretch), flex-grow / flex-shrink with
// min/max freezing, flex-basis, width/height in points or percent, min/max sizes, margins,
// padding, gap, flex-wrap (lines stacked from the start; align-content is always "start"),
// absolute positioning (left/top/right/bottom), display:none, and leaf measure callbacks.
// Units are DIPs; results are relative to the parent's border box.
// Not supported (documented limits): auto margins, baseline alignment, aspect-ratio, RTL.
#pragma once

#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace sq::ui {

enum class FlexDirection { Column, Row, ColumnReverse, RowReverse };
enum class Justify { Start, End, Center, SpaceBetween, SpaceAround, SpaceEvenly };
enum class Align { Auto, Start, End, Center, Stretch };
enum class FlexWrap { NoWrap, Wrap };
enum class PositionType { Relative, Absolute };

struct Dim {
  enum class Unit { Auto, Points, Percent };
  Unit unit = Unit::Auto;
  float value = 0;
  static Dim pt(float v) { return Dim{Unit::Points, v}; }
  static Dim pct(float v) { return Dim{Unit::Percent, v}; }
  static Dim none() { return Dim{}; }
  bool isAuto() const { return unit == Unit::Auto; }
  // NaN when unresolvable (auto, or percent of an unknown size).
  float resolve(float parent) const {
    if (unit == Unit::Points) return value;
    if (unit == Unit::Percent && !std::isnan(parent)) return parent * value / 100.0f;
    return NAN;
  }
};

struct Edges {
  float left = 0, top = 0, right = 0, bottom = 0;
  static Edges all(float v) { return Edges{v, v, v, v}; }
  float horizontal() const { return left + right; }
  float vertical() const { return top + bottom; }
};

struct LayoutStyle {
  FlexDirection direction = FlexDirection::Column;
  Justify justify = Justify::Start;
  Align alignItems = Align::Stretch;
  Align alignSelf = Align::Auto;
  FlexWrap wrap = FlexWrap::NoWrap;
  PositionType position = PositionType::Relative;
  bool display = true;  // false = display:none (no space, not laid out)
  float grow = 0;
  float shrink = 0;  // Yoga-style default (CSS uses 1)
  Dim basis;
  Dim width, height, minWidth, minHeight, maxWidth, maxHeight;
  Edges margin, padding;
  float gap = 0;  // between items and between wrapped lines
  Dim left, top, right, bottom;  // absolute offsets
};

struct LayoutBox {
  float x = 0, y = 0, w = 0, h = 0;
};

class LayoutNode {
 public:
  using MeasureFn = std::function<void(float availWidth, float availHeight, float& outW, float& outH)>;
  virtual ~LayoutNode() = default;

  LayoutStyle style;
  LayoutBox box;      // result, relative to parent
  MeasureFn measure;  // leaf content size (excluding padding); NaN avail = unconstrained

  virtual size_t layoutChildCount() const = 0;
  virtual LayoutNode* layoutChild(size_t i) const = 0;
};

// Simple owning node (tests, ad-hoc layouts).
class FlexBox final : public LayoutNode {
 public:
  FlexBox() = default;
  explicit FlexBox(const LayoutStyle& s) { style = s; }
  FlexBox* add(const LayoutStyle& s = {}) {
    children_.push_back(std::make_unique<FlexBox>(s));
    return children_.back().get();
  }
  FlexBox* child(size_t i) const { return children_[i].get(); }
  size_t layoutChildCount() const override { return children_.size(); }
  LayoutNode* layoutChild(size_t i) const override { return children_[i].get(); }

 private:
  std::vector<std::unique_ptr<FlexBox>> children_;
};

// Lays out `root` into width x height (NaN = size to content). Root box is placed at (0,0).
void computeLayout(LayoutNode& root, float width, float height);

}  // namespace sq::ui
