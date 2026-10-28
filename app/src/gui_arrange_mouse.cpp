// Squizzy Studio GUI - arrange view mouse interaction (see gui_arrange.hpp). Drags are previewed
// locally and committed as one document edit on mouse up.
#include <algorithm>
#include <cmath>

#include "gui_arrange.hpp"

namespace sq::app::gui::arrange {
namespace {
bool overlaps(const Rect& a, const Rect& b) { return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom(); }
Rect normRect(float x0, float y0, float x1, float y1) { return Rect{std::min(x0, x1), std::min(y0, y1), std::fabs(x1 - x0), std::fabs(y1 - y0)}; }
}  // namespace

// ---- hit testing ------------------------------------------------------------------------------------
Hit ArrangeView::hitTest(float x, float y) const {
  Hit h;
  const Rect& f = frame();
  if (!f.contains(x, y)) return h;
  if (toolbarRect().contains(x, y)) {
    h.kind = HitKind::Toolbar;
    for (const auto& b : toolButtons_)
      if (b.r.contains(x, y)) h.button = b.id;
    return h;
  }
  if (cornerRect().contains(x, y)) {
    h.kind = HitKind::Corner;
    return h;
  }
  const Rect mr = markerRect(), rr = rulerRect();
  if (mr.contains(x, y)) {
    h.ppq = ppqAt(x);
    h.kind = HitKind::MarkerStrip;
    for (size_t i = markers_.size(); i-- > 0;) {
      const auto& mv = markers_[i];
      if (x >= mv.x - 2 && x <= mv.x + mv.w) {
        h.kind = HitKind::Marker;
        h.marker = static_cast<int>(i);
        break;
      }
    }
    return h;
  }
  if (rr.contains(x, y)) {
    h.ppq = ppqAt(x);
    h.kind = y >= rr.y + rr.h * 0.52f ? HitKind::LoopZone : HitKind::Ruler;
    return h;
  }
  if (vbarRect().contains(x, y)) {
    h.kind = HitKind::VScrollbar;
    return h;
  }
  if (hbarRect().contains(x, y)) {
    h.kind = HitKind::HScrollbar;
    return h;
  }
  const Rect L = lanesRect();
  if (y < L.y || y >= L.bottom()) return h;
  if (std::fabs(x - (f.x + headerW_)) <= 3) {
    h.kind = HitKind::HeaderBorder;
    return h;
  }
  h.ppq = ppqAt(x);
  const float cy = y - L.y + scrollY_;
  const bool inHeader = x < f.x + headerW_;
  for (size_t i = 0; i < lanes_.size(); ++i) {
    const Lane& l = lanes_[i];
    if (cy < l.y || cy >= l.y + l.total) continue;
    h.lane = static_cast<int>(i);
    if (cy >= l.y + l.h) {  // automation lane
      for (size_t a = 0; a < l.autos.size(); ++a) {
        const AutoLane& al = l.autos[a];
        if (cy < al.y || cy >= al.y + al.h) continue;
        h.autoLane = static_cast<int>(a);
        if (inHeader) {
          h.kind = HitKind::AutoHeader;
          return h;
        }
        h.kind = HitKind::AutoLane;
        const Rect r = autoLaneRect(l, al);
        for (size_t p = 0; p < al.points.size(); ++p) {
          const float px = xOf(al.points[p].first), py = autoValueY(al, r, al.points[p].second);
          if (std::fabs(px - x) <= 6 && std::fabs(py - y) <= 6) {
            h.kind = HitKind::AutoPoint;
            h.point = static_cast<int>(p);
          }
        }
        return h;
      }
      return h;
    }
    if (inHeader) {
      if (cy >= l.y + l.h - 4) {
        h.kind = HitKind::LaneBorder;
        return h;
      }
      for (int b = 0; b < kBtnCount; ++b) {
        if ((b == kBtnArm || b == kBtnMonitor) && !(l.audio() || l.instrument())) continue;
        const Rect br = headerButtonRect(l, b);
        if (br.y + br.h <= laneTop(l) + l.h && br.contains(x, y)) {
          h.kind = HitKind::HeaderButton;
          h.button = b;
          return h;
        }
      }
      const Rect vr = volumeRect(l), pr = panRect(l);
      if (vr.w > 0 && Rect{vr.x - 2, vr.y - 4, vr.w + 4, vr.h + 8}.contains(x, y)) {
        h.kind = HitKind::HeaderVolume;
        return h;
      }
      if (pr.w > 0 && Rect{pr.x - 2, pr.y - 4, pr.w + 4, pr.h + 8}.contains(x, y)) {
        h.kind = HitKind::HeaderPan;
        return h;
      }
      h.kind = nameRect(l).contains(x, y) ? HitKind::HeaderName : HitKind::Header;
      return h;
    }
    h.kind = HitKind::LaneEmpty;
    for (size_t k = l.clips.size(); k-- > 0;) {
      const ClipEntry& ce = l.clips[k];
      const Rect r = clipRect(l, ce);
      if (!Rect{r.x - 1, r.y, r.w + 2, r.h}.contains(x, y)) continue;
      h.clip = static_cast<int>(k);
      h.kind = HitKind::Clip;
      const float edge = std::min(6.0f, std::max(2.0f, r.w / 4));
      if (ce.v.info.kind == SQ_CLIP_AUDIO && r.w > 24) {
        const float hy = r.y + (r.h >= 30 ? 15.0f : 0.0f);
        const float fi = std::clamp(fadeInX(ce), r.x + 4, r.right() - 4), fo = std::clamp(fadeOutX(ce), r.x + 4, r.right() - 4);
        if (std::fabs(y - hy) <= 6 && std::fabs(x - fi) <= 6) {
          h.kind = HitKind::FadeInHandle;
          return h;
        }
        if (std::fabs(y - hy) <= 6 && std::fabs(x - fo) <= 6) {
          h.kind = HitKind::FadeOutHandle;
          return h;
        }
      }
      if (x <= r.x + edge) h.kind = HitKind::ClipEdgeL;
      else if (x >= r.right() - edge) h.kind = HitKind::ClipEdgeR;
      return h;
    }
    return h;
  }
  h.kind = HitKind::BelowTracks;
  return h;
}

// ---- dispatch ---------------------------------------------------------------------------------------
bool ArrangeView::onMouse(const WidgetMouse& e) {
  mouseX_ = e.x;
  mouseY_ = e.y;
  switch (e.action) {
    case MouseAction::Hover: {
      refresh();
      const Hit h = hitTest(e.x, e.y);
      const int tool = h.kind == HitKind::Toolbar ? h.button : 0;
      if (h.kind != hover_.kind || h.lane != hover_.lane || h.clip != hover_.clip || h.point != hover_.point || h.button != hover_.button ||
          h.marker != hover_.marker || h.autoLane != hover_.autoLane || tool != hotTool_ || h.kind == HitKind::Ruler || h.kind == HitKind::LoopZone ||
          h.kind == HitKind::MarkerStrip || tool_ != Tool::Pointer)
        ctx_.invalidate();
      hover_ = h;
      hotTool_ = tool;
      mouseInside_ = true;
      return true;
    }
    case MouseAction::Leave:
      hover_ = Hit{};
      hotTool_ = 0;
      mouseInside_ = false;
      ctx_.invalidate();
      return false;
    case MouseAction::Wheel: wheel(e); return true;
    case MouseAction::Down:
      refresh();
      if (e.button == MouseButton::Right) rightClick(e);
      else if (e.button == MouseButton::Left) beginDrag(e, hitTest(e.x, e.y));
      return true;
    case MouseAction::Move:
      if (drag_ != Drag::None) updateDrag(e);
      return true;
    case MouseAction::Up:
      if (drag_ != Drag::None) endDrag(e);
      return true;
    case MouseAction::DoubleClick:
      refresh();
      if (e.button == MouseButton::Left || e.button == MouseButton::None) doubleClick(e);
      return true;
  }
  return false;
}

// ---- press ------------------------------------------------------------------------------------------
void ArrangeView::beginDrag(const WidgetMouse& e, const Hit& h) {
  downX_ = e.x;
  downY_ = e.y;
  downMods_ = e.modifiers;
  moved_ = false;
  downHit_ = h;
  drag_ = Drag::None;
  dragNode_ = sq_guid{};
  dragDelta_ = 0;
  dragLaneDelta_ = 0;
  dragCopy_ = false;
  selectOnUp_ = toggleOnUp_ = false;
  autoNew_ = false;
  justAddedPoint_ = false;
  const bool ctrl = (e.modifiers & kModCtrl) != 0, shift = (e.modifiers & kModShift) != 0, alt = (e.modifiers & kModAlt) != 0;
  const auto ts = ctx_.transport();
  switch (h.kind) {
    case HitKind::Toolbar:
      if (h.button) {
        for (const auto& b : toolButtons_)
          if (b.id == h.button) toolbarClick(h.button, Point{b.r.x, b.r.bottom() + 2});
      }
      return;
    case HitKind::Marker:
      drag_ = Drag::MarkerMove;
      dragA_ = markers_[static_cast<size_t>(h.marker)].m.position;
      dragPpq0_ = h.ppq;
      return;
    case HitKind::Ruler:
      drag_ = Drag::Locate;
      locate(snap(h.ppq, e.modifiers));
      return;
    case HitKind::LoopZone: {
      const float xs = xOf(ts.loop_start), xe = xOf(ts.loop_end);
      dragA_ = ts.loop_start;
      dragB_ = ts.loop_end;
      dragPpq0_ = snap(h.ppq, e.modifiers);
      if (ts.loop_end > ts.loop_start && std::fabs(e.x - xs) <= 6) drag_ = Drag::LoopStart;
      else if (ts.loop_end > ts.loop_start && std::fabs(e.x - xe) <= 6) drag_ = Drag::LoopEnd;
      else if (ts.loop_end > ts.loop_start && e.x > xs && e.x < xe) drag_ = Drag::LoopMove;
      else {
        drag_ = Drag::LoopNew;
        dragA_ = dragB_ = dragPpq0_;
      }
      return;
    }
    case HitKind::HeaderBorder:
      drag_ = Drag::HeaderResize;
      dragStartVal_ = headerW_;
      return;
    case HitKind::LaneBorder:
      drag_ = Drag::LaneResize;
      dragStartVal_ = lanes_[static_cast<size_t>(h.lane)].h;
      return;
    case HitKind::HeaderButton: {
      const Lane& l = lanes_[static_cast<size_t>(h.lane)];
      if (l.t.id != es_.track) ctx_.selectTrack(l.t.id);
      toggleTrackFlag(l, h.button);
      return;
    }
    case HitKind::HeaderVolume:
    case HitKind::HeaderPan: {
      const Lane& l = lanes_[static_cast<size_t>(h.lane)];
      if (l.t.id != es_.track) ctx_.selectTrack(l.t.id);
      drag_ = h.kind == HitKind::HeaderVolume ? Drag::Volume : Drag::Pan;
      dragStartVal_ = static_cast<float>(h.kind == HitKind::HeaderVolume ? l.t.info.volume_db : l.t.info.pan);
      dragA_ = dragStartVal_;
      dragNode_ = l.t.id;
      return;
    }
    case HitKind::Header:
    case HitKind::HeaderName: ctx_.selectTrack(lanes_[static_cast<size_t>(h.lane)].t.id); return;
    case HitKind::AutoHeader: {
      const Lane& l = lanes_[static_cast<size_t>(h.lane)];
      const Rect L = lanesRect();
      const AutoLane& a = l.autos[static_cast<size_t>(h.autoLane)];
      autoTargetMenu(h.lane, h.autoLane, Point{frame().x + 24, L.y + a.y - scrollY_ + 28});
      return;
    }
    case HitKind::ClipEdgeL:
    case HitKind::ClipEdgeR:
    case HitKind::FadeInHandle:
    case HitKind::FadeOutHandle:
    case HitKind::Clip: {
      const Lane& l = lanes_[static_cast<size_t>(h.lane)];
      const ClipEntry& ce = l.clips[static_cast<size_t>(h.clip)];
      if (l.t.id != es_.track) ctx_.selectTrack(l.t.id);
      if (tool_ == Tool::Split) {
        splitAt(ce, snap(h.ppq, e.modifiers));
        return;
      }
      dragNode_ = ce.v.id;
      dragPpq0_ = h.ppq;
      if (h.kind == HitKind::ClipEdgeL || h.kind == HitKind::ClipEdgeR) {
        if (!selected(ce.v.id)) ctx_.setSelection({ce.v.id});
        drag_ = h.kind == HitKind::ClipEdgeL ? Drag::TrimLeft : Drag::TrimRight;
        dragA_ = ce.v.info.start;
        dragB_ = ce.v.info.length;
        return;
      }
      if (h.kind == HitKind::FadeInHandle || h.kind == HitKind::FadeOutHandle) {
        if (!selected(ce.v.id)) ctx_.setSelection({ce.v.id});
        drag_ = h.kind == HitKind::FadeInHandle ? Drag::FadeIn : Drag::FadeOut;
        dragA_ = h.kind == HitKind::FadeInHandle ? ce.v.info.fade_in_seconds : ce.v.info.fade_out_seconds;
        return;
      }
      if (ctrl || shift) {
        if (selected(ce.v.id)) {
          toggleOnUp_ = true;
        } else {
          auto sel = es_.selection;
          sel.push_back(ce.v.id);
          ctx_.setSelection(sel);
        }
      } else if (!selected(ce.v.id)) {
        ctx_.setSelection({ce.v.id});
      } else {
        selectOnUp_ = es_.selection.size() > 1;
      }
      es_ = ctx_.editState();
      drag_ = alt ? Drag::Pending : Drag::ClipMove;
      return;
    }
    case HitKind::LaneEmpty: {
      const Lane& l = lanes_[static_cast<size_t>(h.lane)];
      if (l.t.id != es_.track) ctx_.selectTrack(l.t.id);
      if (tool_ == Tool::Draw) {
        createMidiClip(l, h.ppq);
        return;
      }
      drag_ = Drag::Rubber;
      rubber_ = Rect{e.x, e.y, 0, 0};
      return;
    }
    case HitKind::AutoLane:
    case HitKind::AutoPoint: {
      const Lane& l = lanes_[static_cast<size_t>(h.lane)];
      const AutoLane& a = l.autos[static_cast<size_t>(h.autoLane)];
      dragPoints_ = a.points;
      dragNode_ = l.t.id;
      drag_ = Drag::AutoPoint;
      if (h.kind == HitKind::AutoLane) {
        const Rect r = autoLaneRect(l, a);
        const double pos = snap(h.ppq, e.modifiers);
        const double val = autoValueAt(a, r, e.y);
        auto it = std::upper_bound(dragPoints_.begin(), dragPoints_.end(), pos, [](double v, const std::pair<double, double>& pt) { return v < pt.first; });
        it = dragPoints_.insert(it, std::make_pair(pos, val));
        downHit_.point = static_cast<int>(it - dragPoints_.begin());
        autoNew_ = true;
      }
      return;
    }
    case HitKind::VScrollbar: {
      const Rect vb = vbarRect(), L = lanesRect();
      const float totalH = std::max(contentH_ + 80.0f, L.h);
      const float th = std::max(24.0f, vb.h * L.h / totalH);
      const float ty = vb.y + (vb.h - th) * (scrollY_ / std::max(1.0f, totalH - L.h));
      if (e.y < ty) scrollY_ -= L.h * 0.9f;
      else if (e.y > ty + th) scrollY_ += L.h * 0.9f;
      clampScroll();
      drag_ = Drag::VScroll;
      dragStartVal_ = scrollY_;
      ctx_.invalidate();
      return;
    }
    case HitKind::HScrollbar: {
      const Rect hb = hbarRect();
      const double total = contentEnd(), span = visibleSpan();
      const float tw = std::max(30.0f, hb.w * static_cast<float>(span / total));
      const float tx = hb.x + (hb.w - tw) * static_cast<float>(scroll_ / std::max(1e-9, total - span));
      if (e.x < tx) scroll_ -= span * 0.9;
      else if (e.x > tx + tw) scroll_ += span * 0.9;
      clampScroll();
      drag_ = Drag::HScroll;
      dragA_ = scroll_;
      ctx_.invalidate();
      return;
    }
    case HitKind::BelowTracks:
      if (!ctrl && !shift && !es_.selection.empty()) ctx_.setSelection({});
      return;
    default: return;
  }
}

// ---- motion -----------------------------------------------------------------------------------------
void ArrangeView::autoScrollDuringDrag(float x, float y) {
  const Rect L = lanesRect();
  if (x > L.right() - 8) scroll_ += static_cast<double>(x - (L.right() - 8)) / ppx_ * 0.5;
  else if (x < L.x + 4 && scroll_ > 0) scroll_ -= static_cast<double>((L.x + 4) - x) / ppx_ * 0.5;
  if (y > L.bottom() - 6) scrollY_ += (y - (L.bottom() - 6)) * 0.5f;
  else if (y < L.y + 2) scrollY_ -= ((L.y + 2) - y) * 0.5f;
  clampScroll();
}

void ArrangeView::updateDrag(const WidgetMouse& e) {
  if ((downHit_.lane >= 0 && static_cast<size_t>(downHit_.lane) >= lanes_.size()) ||
      (downHit_.marker >= 0 && static_cast<size_t>(downHit_.marker) >= markers_.size()) ||
      (downHit_.autoLane >= 0 && downHit_.lane >= 0 && static_cast<size_t>(downHit_.autoLane) >= lanes_[static_cast<size_t>(downHit_.lane)].autos.size())) {
    drag_ = Drag::None;  // the document changed under the gesture
    return;
  }
  const float dx = e.x - downX_, dy = e.y - downY_;
  if (!moved_ && std::fabs(dx) + std::fabs(dy) >= 3) moved_ = true;
  const Rect L = lanesRect();
  const double ppq = ppqAt(e.x);
  switch (drag_) {
    case Drag::Locate: locate(snap(ppq, e.modifiers)); break;
    case Drag::LoopNew: {
      const double b = snap(ppq, e.modifiers);
      dragA_ = std::min(dragPpq0_, b);
      dragB_ = std::max(dragPpq0_, b);
      break;
    }
    case Drag::LoopStart: dragA_ = std::clamp(snap(ppq, e.modifiers), 0.0, dragB_ - 1.0 / 64.0); break;
    case Drag::LoopEnd: dragB_ = std::max(snap(ppq, e.modifiers), dragA_ + 1.0 / 64.0); break;
    case Drag::LoopMove: {
      const auto ts = ctx_.transport();
      const double len = ts.loop_end - ts.loop_start;
      const double ns = std::max(0.0, snap(ts.loop_start + (ppq - ppqAt(downX_)), e.modifiers));
      dragA_ = ns;
      dragB_ = ns + len;
      break;
    }
    case Drag::MarkerMove: dragA_ = snap(ppq, e.modifiers); break;
    case Drag::HeaderResize: headerW_ = std::clamp(dragStartVal_ + dx, 150.0f, std::max(150.0f, std::min(440.0f, frame().w * 0.5f))); break;
    case Drag::LaneResize: {
      const Lane& l = lanes_[static_cast<size_t>(downHit_.lane)];
      laneHeights_[l.t.id] = std::clamp(dragStartVal_ + dy, kMinLaneH, kMaxLaneH);
      relayout();
      break;
    }
    case Drag::Rubber:
      autoScrollDuringDrag(e.x, e.y);
      rubber_ = normRect(std::clamp(downX_, L.x, L.right()), std::clamp(downY_, L.y, L.bottom()), std::clamp(e.x, L.x, L.right()),
                         std::clamp(e.y, L.y, L.bottom()));
      break;
    case Drag::Pending:
      if (!moved_) break;
      drag_ = Drag::ClipMove;
      [[fallthrough]];
    case Drag::ClipMove: {
      if (!moved_) break;
      autoScrollDuringDrag(e.x, e.y);
      const ClipEntry* anchor = findClip(dragNode_);
      if (!anchor) break;
      dragCopy_ = (e.modifiers & (kModCtrl | kModAlt)) != 0;
      const double raw = anchor->v.info.start + (ppqAt(e.x) - dragPpq0_);
      double delta = snap(raw, e.modifiers) - anchor->v.info.start;
      double minStart = 1e18;
      for (const auto& l : lanes_)
        for (const auto& c : l.clips)
          if (selected(c.v.id)) minStart = std::min(minStart, c.v.info.start);
      if (minStart < 1e17) delta = std::max(delta, -minStart);
      dragDelta_ = delta;
      // track offset from the lane under the mouse
      const float cy = std::clamp(e.y, L.y, L.bottom() - 1) - L.y + scrollY_;
      int li = static_cast<int>(lanes_.size()) - 1;
      for (size_t i = 0; i < lanes_.size(); ++i)
        if (cy < lanes_[i].y + lanes_[i].total) {
          li = static_cast<int>(i);
          break;
        }
      dragLaneDelta_ = downHit_.lane >= 0 ? li - downHit_.lane : 0;
      break;
    }
    case Drag::TrimLeft:
    case Drag::TrimRight: {
      if (!moved_) break;
      autoScrollDuringDrag(e.x, e.y);
      const ClipEntry* ce = findClip(dragNode_);
      if (!ce) break;
      const double minLen = std::min(ce->v.info.length, std::max(1.0 / 64.0, 3.0 / ppx_));
      const double start = ce->v.info.start, end = ce->end();
      const bool audio = ce->v.info.kind == SQ_CLIP_AUDIO;
      if (drag_ == Drag::TrimLeft) {
        double ns = snap(ppq, e.modifiers);
        if (audio) ns = std::max(ns, ppqAtSec(secAt(start) - ce->v.info.source_offset_seconds));  // not before the file start
        ns = std::clamp(ns, 0.0, end - minLen);
        dragA_ = ns;
        dragB_ = end - ns;
      } else {
        double ne = snap(ppq, e.modifiers);
        if (audio) {
          auto pd = peaks_.request(ce->v.source);
          if (pd && pd->state() == PeakData::Ready)
            ne = std::min(ne, ppqAtSec(secAt(start) + pd->info().seconds() - ce->v.info.source_offset_seconds));
        }
        ne = std::max(ne, start + minLen);
        dragA_ = start;
        dragB_ = ne - start;
      }
      break;
    }
    case Drag::FadeIn:
    case Drag::FadeOut: {
      const ClipEntry* ce = findClip(dragNode_);
      if (!ce) break;
      const double s0 = secAt(ce->v.info.start), s1 = secAt(ce->end());
      const double sec = secAt(std::max(0.0, ppq));
      if (drag_ == Drag::FadeIn) dragA_ = std::clamp(sec - s0, 0.0, std::max(0.0, (s1 - s0) - ce->v.info.fade_out_seconds));
      else dragA_ = std::clamp(s1 - sec, 0.0, std::max(0.0, (s1 - s0) - ce->v.info.fade_in_seconds));
      if (dragA_ < 0.002) dragA_ = 0;
      break;
    }
    case Drag::AutoPoint: {
      if (!moved_ && !autoNew_) break;
      const int i = downHit_.point;
      if (i < 0 || static_cast<size_t>(i) >= dragPoints_.size() || downHit_.lane < 0) break;
      autoScrollDuringDrag(e.x, e.y);
      const Lane& l = lanes_[static_cast<size_t>(downHit_.lane)];
      const AutoLane& a = l.autos[static_cast<size_t>(downHit_.autoLane)];
      AutoLane view = a;
      view.points = dragPoints_;
      const Rect r = autoLaneRect(l, a);
      double pos = snap(ppq, e.modifiers);
      const double lo = i > 0 ? dragPoints_[static_cast<size_t>(i) - 1].first : 0.0;
      const double hi = static_cast<size_t>(i) + 1 < dragPoints_.size() ? dragPoints_[static_cast<size_t>(i) + 1].first : 1e12;
      pos = std::clamp(pos, lo, hi);
      dragPoints_[static_cast<size_t>(i)] = std::make_pair(pos, autoValueAt(view, r, e.y));
      break;
    }
    case Drag::VScroll: {
      const Rect vb = vbarRect();
      const float totalH = std::max(contentH_ + 80.0f, L.h);
      const float th = std::max(24.0f, vb.h * L.h / totalH);
      scrollY_ = dragStartVal_ + dy * (totalH - L.h) / std::max(1.0f, vb.h - th);
      clampScroll();
      break;
    }
    case Drag::HScroll: {
      const Rect hb = hbarRect();
      const double total = contentEnd(), span = visibleSpan();
      const float tw = std::max(30.0f, hb.w * static_cast<float>(span / total));
      scroll_ = dragA_ + static_cast<double>(dx) * (total - span) / std::max(1.0, static_cast<double>(hb.w - tw));
      clampScroll();
      break;
    }
    case Drag::Volume:
    case Drag::Pan: {
      const Lane& l = lanes_[static_cast<size_t>(downHit_.lane)];
      const float fine = (e.modifiers & kModShift) ? 0.2f : 1.0f;
      if (drag_ == Drag::Volume) {
        const Rect vr = volumeRect(l);
        dragA_ = std::clamp(static_cast<double>(dragStartVal_) + static_cast<double>(dx * fine / std::max(1.0f, vr.w)) * 66.0, -60.0, 6.0);
        if (std::fabs(dragA_) < 0.15 && fine == 1.0f) dragA_ = 0;  // detent at 0 dB
      } else {
        const Rect pr = panRect(l);
        dragA_ = std::clamp(static_cast<double>(dragStartVal_) + static_cast<double>(dx * fine / std::max(1.0f, pr.w)) * 2.0, -1.0, 1.0);
        if (std::fabs(dragA_) < 0.03 && fine == 1.0f) dragA_ = 0;
      }
      break;
    }
    default: break;
  }
  ctx_.invalidate();
}

// ---- release ----------------------------------------------------------------------------------------
void ArrangeView::endDrag(const WidgetMouse& e) {
  if ((downHit_.lane >= 0 && static_cast<size_t>(downHit_.lane) >= lanes_.size()) ||
      (downHit_.marker >= 0 && static_cast<size_t>(downHit_.marker) >= markers_.size()) ||
      (downHit_.autoLane >= 0 && downHit_.lane >= 0 && static_cast<size_t>(downHit_.autoLane) >= lanes_[static_cast<size_t>(downHit_.lane)].autos.size())) {
    drag_ = Drag::None;  // the document changed under the gesture
    return;
  }
  const Drag d = drag_;
  drag_ = Drag::None;
  const bool ctrl = (e.modifiers & kModCtrl) != 0, shift = (e.modifiers & kModShift) != 0;
  ITransport* tr = ctx_.transportIf();
  const auto ts = ctx_.transport();
  switch (d) {
    case Drag::LoopNew:
      if (moved_ && dragB_ - dragA_ > 1e-6) {
        if (tr) tr->setLoop(dragA_, dragB_, SQ_TRUE);
      } else {
        locate(snap(downHit_.ppq, e.modifiers));
      }
      break;
    case Drag::LoopStart:
    case Drag::LoopEnd:
    case Drag::LoopMove:
      if (moved_ && tr) tr->setLoop(dragA_, dragB_, (ts.flags & SQ_TRANSPORT_LOOPING) ? SQ_TRUE : SQ_FALSE);
      break;
    case Drag::MarkerMove: {
      const auto& mk = markers_[static_cast<size_t>(downHit_.marker)].m;
      if (moved_ && std::fabs(dragA_ - mk.position) > 1e-9) ctx_.setProperty(mk.id, "position", dragA_);
      else if (!moved_) locate(mk.position);
      break;
    }
    case Drag::Rubber:
      if (moved_) {
        std::vector<sq_guid> sel;
        if (ctrl || shift) sel = es_.selection;
        for (const auto& l : lanes_) {
          const float top = laneTop(l);
          if (top > rubber_.bottom() || top + l.h < rubber_.y) continue;
          for (const auto& c : l.clips)
            if (overlaps(clipRect(l, c), rubber_) && std::find(sel.begin(), sel.end(), c.v.id) == sel.end()) sel.push_back(c.v.id);
        }
        ctx_.setSelection(sel);
      } else {
        const double at = snap(downHit_.ppq, e.modifiers);
        if (!(ts.flags & SQ_TRANSPORT_PLAYING)) locate(at);
        else ctx_.setCursor(at);
        if (!ctrl && !shift && !es_.selection.empty()) ctx_.setSelection({});
      }
      break;
    case Drag::Pending: {  // Alt+click without moving: split at the mouse
      if (const ClipEntry* ce = findClip(dragNode_)) splitAt(*ce, snap(downHit_.ppq, e.modifiers & ~static_cast<unsigned>(kModAlt)));
      break;
    }
    case Drag::ClipMove:
      if (moved_) {
        commitMove(dragDelta_, dragLaneDelta_, dragCopy_);
      } else if (selectOnUp_) {
        ctx_.setSelection({dragNode_});
      } else if (toggleOnUp_) {
        auto sel = es_.selection;
        sel.erase(std::remove(sel.begin(), sel.end(), dragNode_), sel.end());
        ctx_.setSelection(sel);
      }
      break;
    case Drag::TrimLeft:
    case Drag::TrimRight:
      if (moved_) {
        const ClipEntry* ce = findClip(dragNode_);
        if (ce && (std::fabs(dragA_ - ce->v.info.start) > 1e-9 || std::fabs(dragB_ - ce->v.info.length) > 1e-9)) {
          auto p = sq::makeAttributes();
          p->setGuid("node", &dragNode_);
          p->setDouble("start", dragA_);
          p->setDouble("length", dragB_);
          ctx_.exec(sq::doc::kResizeClip, p.get());
        }
      }
      break;
    case Drag::FadeIn:
    case Drag::FadeOut:
      if (moved_) ctx_.setProperty(dragNode_, d == Drag::FadeIn ? "fadeIn" : "fadeOut", dragA_);
      break;
    case Drag::AutoPoint:
      if ((moved_ || autoNew_) && downHit_.lane >= 0) {
        const Lane& l = lanes_[static_cast<size_t>(downHit_.lane)];
        setAutomation(l.t.id, l.autos[static_cast<size_t>(downHit_.autoLane)].target, dragPoints_);
        justAddedPoint_ = autoNew_;
      }
      break;
    case Drag::Volume:
      if (moved_) ctx_.setProperty(dragNode_, "volumeDb", dragA_);
      break;
    case Drag::Pan:
      if (moved_) ctx_.setProperty(dragNode_, "pan", dragA_);
      break;
    default: break;
  }
  dragNode_ = sq_guid{};
  moved_ = false;
  hover_ = hitTest(e.x, e.y);  // no hover events arrive while the button is captured
  ctx_.invalidate();
}

// ---- double click -----------------------------------------------------------------------------------
void ArrangeView::doubleClick(const WidgetMouse& e) {
  const Hit h = hitTest(e.x, e.y);
  drag_ = Drag::None;
  switch (h.kind) {
    case HitKind::MarkerStrip: addMarkerAt(snap(h.ppq, e.modifiers)); break;
    case HitKind::Marker: {
      const auto mk = markers_[static_cast<size_t>(h.marker)].m;
      GuiContext& ctx = ctx_;
      const sq_guid id = mk.id;
      ctx_.prompt("Rename Marker", mk.name, [&ctx, id](const std::string& n) { ctx.setPropertyString(id, "name", n); });
      break;
    }
    case HitKind::LoopZone: {
      const auto ts = ctx_.transport();
      if (ITransport* tr = ctx_.transportIf(); tr && ts.loop_end > ts.loop_start)
        tr->setLoop(ts.loop_start, ts.loop_end, (ts.flags & SQ_TRANSPORT_LOOPING) ? SQ_FALSE : SQ_TRUE);
      break;
    }
    case HitKind::Header:
    case HitKind::HeaderName: renameTrack(lanes_[static_cast<size_t>(h.lane)]); break;
    case HitKind::HeaderVolume: ctx_.setProperty(lanes_[static_cast<size_t>(h.lane)].t.id, "volumeDb", 0.0); break;
    case HitKind::HeaderPan: ctx_.setProperty(lanes_[static_cast<size_t>(h.lane)].t.id, "pan", 0.0); break;
    case HitKind::Clip:
    case HitKind::ClipEdgeL:
    case HitKind::ClipEdgeR: {
      const ClipEntry& ce = lanes_[static_cast<size_t>(h.lane)].clips[static_cast<size_t>(h.clip)];
      ctx_.setSelection({ce.v.id});
      ctx_.openClipEditor(ce.v.id);
      break;
    }
    case HitKind::LaneEmpty: {
      const Lane& l = lanes_[static_cast<size_t>(h.lane)];
      if (l.instrument() && tool_ != Tool::Draw) createMidiClip(l, h.ppq);
      else if (l.audio()) ctx_.setStatus("Drop or import audio files onto audio tracks (File > Import Audio File)");
      break;
    }
    case HitKind::AutoPoint: {
      if (justAddedPoint_) break;  // the first click of this double-click created it
      const Lane& l = lanes_[static_cast<size_t>(h.lane)];
      const AutoLane& a = l.autos[static_cast<size_t>(h.autoLane)];
      auto pts = a.points;
      pts.erase(pts.begin() + h.point);
      setAutomation(l.t.id, a.target, pts);
      break;
    }
    case HitKind::BelowTracks: addTrackMenu(Point{e.x, e.y}); break;
    default: break;
  }
  justAddedPoint_ = false;
}

// ---- right click ------------------------------------------------------------------------------------
void ArrangeView::rightClick(const WidgetMouse& e) {
  const Hit h = hitTest(e.x, e.y);
  const Point at{e.x, e.y};
  drag_ = Drag::None;
  switch (h.kind) {
    case HitKind::Clip:
    case HitKind::ClipEdgeL:
    case HitKind::ClipEdgeR:
    case HitKind::FadeInHandle:
    case HitKind::FadeOutHandle: clipMenu(h.lane, h.clip, at); break;
    case HitKind::Header:
    case HitKind::HeaderName:
    case HitKind::HeaderButton:
    case HitKind::HeaderVolume:
    case HitKind::HeaderPan:
    case HitKind::LaneBorder: trackMenu(h.lane, at); break;
    case HitKind::LaneEmpty: laneMenu(h.lane, h.ppq, at); break;
    case HitKind::BelowTracks: laneMenu(-1, h.ppq, at); break;
    case HitKind::Marker: markerMenu(h.marker, at); break;
    case HitKind::AutoHeader: autoTargetMenu(h.lane, h.autoLane, at); break;
    case HitKind::AutoPoint: {
      const Lane& l = lanes_[static_cast<size_t>(h.lane)];
      const AutoLane& a = l.autos[static_cast<size_t>(h.autoLane)];
      auto pts = a.points;
      pts.erase(pts.begin() + h.point);
      setAutomation(l.t.id, a.target, pts);
      break;
    }
    case HitKind::MarkerStrip:
    case HitKind::Ruler:
    case HitKind::LoopZone: {
      const double pos = snap(h.ppq, e.modifiers);
      const auto ts = ctx_.transport();
      std::vector<MenuItem> m;
      MenuItem mk;
      mk.label = "Add Marker Here";
      mk.action = [this, pos] { addMarkerAt(pos); };
      m.push_back(mk);
      MenuItem go;
      go.label = "Locate Here";
      go.action = [this, pos] { locate(pos); };
      m.push_back(go);
      m.push_back(MenuItem::sep());
      MenuItem loop;
      loop.label = "Loop Enabled";
      loop.checked = (ts.flags & SQ_TRANSPORT_LOOPING) != 0;
      loop.enabled = ts.loop_end > ts.loop_start;
      loop.action = [this] {
        const auto t = ctx_.transport();
        if (ITransport* tr = ctx_.transportIf()) tr->setLoop(t.loop_start, t.loop_end, (t.flags & SQ_TRANSPORT_LOOPING) ? SQ_FALSE : SQ_TRUE);
      };
      m.push_back(loop);
      MenuItem bar;
      bar.label = "Loop This Bar";
      const double bs = barStart(h.ppq), bl = barLength(h.ppq);
      bar.action = [this, bs, bl] {
        if (ITransport* tr = ctx_.transportIf()) tr->setLoop(bs, bs + bl, SQ_TRUE);
      };
      m.push_back(bar);
      if (!es_.selection.empty()) {
        MenuItem ls;
        ls.label = "Loop Selection";
        ls.action = [this] {
          double a = 1e18, b = 0;
          for (const auto& l : lanes_)
            for (const auto& c : l.clips)
              if (selected(c.v.id)) {
                a = std::min(a, c.v.info.start);
                b = std::max(b, c.end());
              }
          if (b > a)
            if (ITransport* tr = ctx_.transportIf()) tr->setLoop(a, b, SQ_TRUE);
        };
        m.push_back(ls);
      }
      ctx_.showMenu(std::move(m), at);
      break;
    }
    default: break;
  }
}

// ---- wheel ------------------------------------------------------------------------------------------
void ArrangeView::wheel(const WidgetMouse& e) {
  refresh();
  const float notches = e.wheel;
  if (e.modifiers & kModCtrl) {
    setZoom(ppx_ * std::pow(1.25, static_cast<double>(notches)), e.x);
    return;
  }
  const Rect R = rulerRect(), M = markerRect();
  if ((e.modifiers & kModShift) || R.contains(e.x, e.y) || M.contains(e.x, e.y) || hbarRect().contains(e.x, e.y)) {
    scroll_ -= static_cast<double>(notches) * visibleSpan() * 0.12;
  } else {
    scrollY_ -= notches * 52.0f;
  }
  clampScroll();
  ctx_.invalidate();
}

}  // namespace sq::app::gui::arrange
