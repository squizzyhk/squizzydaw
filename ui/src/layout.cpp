// Flexbox layout engine (own implementation). See layout.hpp for the supported subset.
#include "sq/ui/layout.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sq::ui {
namespace {

struct Sz {
  float w, h;
};

bool isRow(FlexDirection d) { return d == FlexDirection::Row || d == FlexDirection::RowReverse; }
bool isReverse(FlexDirection d) { return d == FlexDirection::RowReverse || d == FlexDirection::ColumnReverse; }

float clampDim(float v, const Dim& mn, const Dim& mx, float parent) {
  if (std::isnan(v)) return v;
  float hi = mx.resolve(parent);
  float lo = mn.resolve(parent);
  if (!std::isnan(hi)) v = std::min(v, hi);
  if (!std::isnan(lo)) v = std::max(v, lo);  // min wins over max (CSS)
  return std::max(v, 0.0f);
}

float orZero(float v) { return std::isnan(v) ? 0.0f : v; }

struct Item {
  LayoutNode* node = nullptr;
  float basis = 0;   // flex base size
  float hypo = 0;    // hypothetical main size (basis clamped)
  float target = 0;  // resolved main size
  float cross = 0;   // resolved cross size
  float minMain = NAN, maxMain = NAN;
  float marginLead = 0, marginTrail = 0;  // main axis, in flow order (accounts for reverse)
  float marginCrossStart = 0, marginCrossEnd = 0;
  Align align = Align::Stretch;
  bool frozen = false;
  float outerHypo() const { return hypo + marginLead + marginTrail; }
  float outerTarget() const { return target + marginLead + marginTrail; }
  float outerCross() const { return cross + marginCrossStart + marginCrossEnd; }
};

Sz layoutImpl(LayoutNode& n, float w, float h, float pw, float ph, bool perform);

void resolveFlexible(std::vector<Item*>& line, float available, float gap) {
  if (std::isnan(available)) {
    for (Item* it : line) it->target = it->hypo;
    return;
  }
  float gaps = line.empty() ? 0.0f : gap * static_cast<float>(line.size() - 1);
  float sumHypo = gaps;
  for (Item* it : line) sumHypo += it->outerHypo();
  const bool growing = sumHypo < available;
  for (Item* it : line) {
    const LayoutStyle& cs = it->node->style;
    float factor = growing ? cs.grow : cs.shrink;
    it->frozen = factor <= 0 || (growing && it->basis > it->hypo) || (!growing && it->basis < it->hypo);
    it->target = it->frozen ? it->hypo : it->basis;
  }
  float initialFree = 0;
  for (int guard = 0; guard < 64; ++guard) {
    float used = gaps, sumFactor = 0, sumScaled = 0;
    bool anyOpen = false;
    for (Item* it : line) {
      used += (it->frozen ? it->target : it->basis) + it->marginLead + it->marginTrail;
      if (!it->frozen) {
        anyOpen = true;
        sumFactor += growing ? it->node->style.grow : it->node->style.shrink;
        sumScaled += it->node->style.shrink * it->basis;
      }
    }
    if (!anyOpen) break;
    float free = available - used;
    if (guard == 0) initialFree = free;
    if (sumFactor < 1.0f && std::fabs(initialFree * sumFactor) < std::fabs(free)) free = initialFree * sumFactor;
    float violation = 0;
    for (Item* it : line) {
      if (it->frozen) continue;
      float t = it->basis;
      if (growing) {
        if (sumFactor > 0) t = it->basis + free * it->node->style.grow / sumFactor;
      } else if (sumScaled > 0) {
        t = it->basis + free * (it->node->style.shrink * it->basis) / sumScaled;
      }
      float c = t;
      if (!std::isnan(it->maxMain)) c = std::min(c, it->maxMain);
      if (!std::isnan(it->minMain)) c = std::max(c, it->minMain);
      c = std::max(c, 0.0f);
      violation += c - t;
      it->target = c;
    }
    if (std::fabs(violation) < 1e-4f) {
      for (Item* it : line) it->frozen = true;
      break;
    }
    for (Item* it : line) {
      if (it->frozen) continue;
      bool unclampedMin = !std::isnan(it->minMain) && it->target <= it->minMain + 1e-4f;
      bool unclampedMax = !std::isnan(it->maxMain) && it->target >= it->maxMain - 1e-4f;
      if ((violation > 0 && unclampedMin) || (violation < 0 && unclampedMax)) it->frozen = true;
    }
  }
}

Sz layoutImpl(LayoutNode& n, float w, float h, float pw, float ph, bool perform) {
  const LayoutStyle& s = n.style;
  if (std::isnan(w)) w = s.width.resolve(pw);
  if (std::isnan(h)) h = s.height.resolve(ph);
  w = clampDim(w, s.minWidth, s.maxWidth, pw);
  h = clampDim(h, s.minHeight, s.maxHeight, ph);
  const float padH = s.padding.horizontal(), padV = s.padding.vertical();

  std::vector<LayoutNode*> flow, absolute;
  for (size_t i = 0, c = n.layoutChildCount(); i < c; ++i) {
    LayoutNode* ch = n.layoutChild(i);
    if (!ch || !ch->style.display) {
      if (ch && perform) ch->box = LayoutBox{};
      continue;
    }
    (ch->style.position == PositionType::Absolute ? absolute : flow).push_back(ch);
  }

  if (flow.empty() && absolute.empty()) {
    if ((std::isnan(w) || std::isnan(h)) && n.measure) {
      float mw = 0, mh = 0;
      n.measure(std::isnan(w) ? NAN : std::max(0.0f, w - padH), std::isnan(h) ? NAN : std::max(0.0f, h - padV), mw, mh);
      if (std::isnan(w)) w = clampDim(orZero(mw) + padH, s.minWidth, s.maxWidth, pw);
      if (std::isnan(h)) h = clampDim(orZero(mh) + padV, s.minHeight, s.maxHeight, ph);
    }
    if (std::isnan(w)) w = clampDim(padH, s.minWidth, s.maxWidth, pw);
    if (std::isnan(h)) h = clampDim(padV, s.minHeight, s.maxHeight, ph);
    if (perform) {
      n.box.w = w;
      n.box.h = h;
    }
    return {w, h};
  }

  const bool row = isRow(s.direction);
  const bool rev = isReverse(s.direction);
  float innerW = std::isnan(w) ? NAN : std::max(0.0f, w - padH);
  float innerH = std::isnan(h) ? NAN : std::max(0.0f, h - padV);
  float innerMain = row ? innerW : innerH;
  float innerCross = row ? innerH : innerW;

  // ---- flex base sizes ----
  std::vector<Item> items(flow.size());
  for (size_t i = 0; i < flow.size(); ++i) {
    Item& it = items[i];
    it.node = flow[i];
    const LayoutStyle& cs = it.node->style;
    const Edges& m = cs.margin;
    float mStart = row ? m.left : m.top, mEnd = row ? m.right : m.bottom;
    it.marginLead = rev ? mEnd : mStart;
    it.marginTrail = rev ? mStart : mEnd;
    it.marginCrossStart = row ? m.top : m.left;
    it.marginCrossEnd = row ? m.bottom : m.right;
    it.align = cs.alignSelf == Align::Auto ? (s.alignItems == Align::Auto ? Align::Stretch : s.alignItems) : cs.alignSelf;
    const Dim& mainDim = row ? cs.width : cs.height;
    const Dim& crossDim = row ? cs.height : cs.width;
    it.minMain = (row ? cs.minWidth : cs.minHeight).resolve(innerMain);
    it.maxMain = (row ? cs.maxWidth : cs.maxHeight).resolve(innerMain);
    float basis = cs.basis.resolve(innerMain);
    if (std::isnan(basis)) basis = mainDim.resolve(innerMain);
    if (std::isnan(basis)) {
      float crossHint = crossDim.resolve(innerCross);
      Sz c = row ? layoutImpl(*it.node, NAN, crossHint, innerW, innerH, false)
                 : layoutImpl(*it.node, crossHint, NAN, innerW, innerH, false);
      basis = row ? c.w : c.h;
    }
    it.basis = std::max(0.0f, basis);
    float hy = it.basis;
    if (!std::isnan(it.maxMain)) hy = std::min(hy, it.maxMain);
    if (!std::isnan(it.minMain)) hy = std::max(hy, it.minMain);
    it.hypo = std::max(0.0f, hy);
  }

  // ---- lines ----
  std::vector<std::vector<Item*>> lines;
  {
    std::vector<Item*> cur;
    float used = 0;
    for (Item& it : items) {
      float add = it.outerHypo() + (cur.empty() ? 0.0f : s.gap);
      if (s.wrap == FlexWrap::Wrap && !std::isnan(innerMain) && !cur.empty() && used + add > innerMain + 1e-3f) {
        lines.push_back(std::move(cur));
        cur.clear();
        used = 0;
        add = it.outerHypo();
      }
      cur.push_back(&it);
      used += add;
    }
    if (!cur.empty()) lines.push_back(std::move(cur));
  }

  // ---- main sizes, cross sizes per line ----
  std::vector<float> lineCross(lines.size(), 0.0f);
  float contentMain = 0;
  for (size_t li = 0; li < lines.size(); ++li) {
    auto& line = lines[li];
    resolveFlexible(line, innerMain, s.gap);
    float lineMain = line.empty() ? 0.0f : s.gap * static_cast<float>(line.size() - 1);
    float maxCross = 0;
    for (Item* it : line) {
      lineMain += it->outerTarget();
      const LayoutStyle& cs = it->node->style;
      const Dim& crossDim = row ? cs.height : cs.width;
      float c = crossDim.resolve(innerCross);
      if (std::isnan(c)) {
        Sz sz = row ? layoutImpl(*it->node, it->target, NAN, innerW, innerH, false)
                    : layoutImpl(*it->node, NAN, it->target, innerW, innerH, false);
        c = row ? sz.h : sz.w;
      } else {
        c = row ? clampDim(c, cs.minHeight, cs.maxHeight, innerCross) : clampDim(c, cs.minWidth, cs.maxWidth, innerCross);
      }
      it->cross = c;
      maxCross = std::max(maxCross, it->outerCross());
    }
    lineCross[li] = maxCross;
    contentMain = std::max(contentMain, lineMain);
  }
  if (lines.size() == 1 && !std::isnan(innerCross)) lineCross[0] = innerCross;

  // ---- container size ----
  float contentCross = 0;
  for (size_t li = 0; li < lines.size(); ++li) contentCross += lineCross[li] + (li ? s.gap : 0.0f);
  if (row) {
    if (std::isnan(w)) w = clampDim(contentMain + padH, s.minWidth, s.maxWidth, pw);
    if (std::isnan(h)) h = clampDim(contentCross + padV, s.minHeight, s.maxHeight, ph);
  } else {
    if (std::isnan(h)) h = clampDim(contentMain + padV, s.minHeight, s.maxHeight, ph);
    if (std::isnan(w)) w = clampDim(contentCross + padH, s.minWidth, s.maxWidth, pw);
  }
  innerW = std::max(0.0f, w - padH);
  innerH = std::max(0.0f, h - padV);
  innerMain = row ? innerW : innerH;
  innerCross = row ? innerH : innerW;
  if (lines.size() == 1) lineCross[0] = innerCross;

  // stretch
  for (size_t li = 0; li < lines.size(); ++li) {
    for (Item* it : lines[li]) {
      const LayoutStyle& cs = it->node->style;
      const Dim& crossDim = row ? cs.height : cs.width;
      if (it->align == Align::Stretch && crossDim.isAuto()) {
        float c = lineCross[li] - it->marginCrossStart - it->marginCrossEnd;
        it->cross = row ? clampDim(c, cs.minHeight, cs.maxHeight, innerCross) : clampDim(c, cs.minWidth, cs.maxWidth, innerCross);
      }
    }
  }

  if (perform) {
    n.box.w = w;
    n.box.h = h;
    const float padMainStart = row ? s.padding.left : s.padding.top;
    const float padCrossStart = row ? s.padding.top : s.padding.left;
    float crossPos = 0;
    for (size_t li = 0; li < lines.size(); ++li) {
      auto& line = lines[li];
      const size_t cnt = line.size();
      float used = cnt ? s.gap * static_cast<float>(cnt - 1) : 0.0f;
      for (Item* it : line) used += it->outerTarget();
      float free = innerMain - used;
      float lead = 0, between = 0;
      switch (s.justify) {
        case Justify::Start: break;
        case Justify::End: lead = free; break;
        case Justify::Center: lead = free / 2; break;
        case Justify::SpaceBetween:
          if (free > 0 && cnt > 1) between = free / static_cast<float>(cnt - 1);
          break;
        case Justify::SpaceAround:
          if (free > 0) {
            between = free / static_cast<float>(cnt);
            lead = between / 2;
          }
          break;
        case Justify::SpaceEvenly:
          if (free > 0) {
            between = free / static_cast<float>(cnt + 1);
            lead = between;
          }
          break;
      }
      float pos = lead;
      for (Item* it : line) {
        float start = pos + it->marginLead;
        float mainPos = rev ? innerMain - start - it->target : start;
        float avail = lineCross[li] - it->outerCross();
        float c = 0;
        switch (it->align) {
          case Align::End: c = avail; break;
          case Align::Center: c = avail / 2; break;
          default: break;
        }
        float cPos = crossPos + c + it->marginCrossStart;
        LayoutNode& ch = *it->node;
        float cw = row ? it->target : it->cross;
        float chh = row ? it->cross : it->target;
        ch.box.x = row ? padMainStart + mainPos : padCrossStart + cPos;
        ch.box.y = row ? padCrossStart + cPos : padMainStart + mainPos;
        layoutImpl(ch, cw, chh, innerW, innerH, true);
        ch.box.w = cw;
        ch.box.h = chh;
        pos = start + it->target + it->marginTrail + between + s.gap;
      }
      crossPos += lineCross[li] + s.gap;
    }
    // absolute children: containing block = this node's padding box
    for (LayoutNode* ch : absolute) {
      const LayoutStyle& cs = ch->style;
      float l = cs.left.resolve(w), t = cs.top.resolve(h), r = cs.right.resolve(w), b = cs.bottom.resolve(h);
      float cw = cs.width.resolve(w), chh = cs.height.resolve(h);
      if (std::isnan(cw) && !std::isnan(l) && !std::isnan(r)) cw = w - l - r - cs.margin.horizontal();
      if (std::isnan(chh) && !std::isnan(t) && !std::isnan(b)) chh = h - t - b - cs.margin.vertical();
      Sz sz = layoutImpl(*ch, cw, chh, w, h, false);
      cw = sz.w;
      chh = sz.h;
      ch->box.x = !std::isnan(l) ? l + cs.margin.left : !std::isnan(r) ? w - r - cw - cs.margin.right : s.padding.left + cs.margin.left;
      ch->box.y = !std::isnan(t) ? t + cs.margin.top : !std::isnan(b) ? h - b - chh - cs.margin.bottom : s.padding.top + cs.margin.top;
      layoutImpl(*ch, cw, chh, w, h, true);
      ch->box.w = cw;
      ch->box.h = chh;
    }
  }
  return {w, h};
}

}  // namespace

void computeLayout(LayoutNode& root, float width, float height) {
  Sz s = layoutImpl(root, width, height, width, height, false);
  root.box = LayoutBox{0, 0, s.w, s.h};
  layoutImpl(root, s.w, s.h, width, height, true);
  root.box.x = 0;
  root.box.y = 0;
}

}  // namespace sq::ui
