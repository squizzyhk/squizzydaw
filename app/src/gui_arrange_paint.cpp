// Squizzy Studio GUI - arrange view painting (see gui_arrange.hpp).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "gui_arrange.hpp"

namespace sq::app::gui::arrange {
namespace {

float luminance(Color c) { return 0.299f * c.r + 0.587f * c.g + 0.114f * c.b; }
Color onColor(Color bg) { return luminance(bg) > 0.58f ? Color::rgb(0x15161A) : Color::rgb(0xFFFFFF); }

Rect intersect(const Rect& a, const Rect& b) {
  const float x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
  const float x1 = std::min(a.right(), b.right()), y1 = std::min(a.bottom(), b.bottom());
  return Rect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
}

double volNorm(double db) { return std::clamp((db + 60.0) / 66.0, 0.0, 1.0); }

// Tiny glyphs for the track kind.
void kindIcon(Canvas& c, const Rect& r, int32_t kind, Color col) {
  switch (kind) {
    case SQ_TRACK_AUDIO: {
      const float hs[] = {0.35f, 0.8f, 0.55f, 1.0f, 0.45f, 0.7f, 0.3f};
      const float bw = r.w / 7.0f;
      for (int i = 0; i < 7; ++i) {
        const float h = r.h * hs[i];
        c.fillRect(Rect{r.x + static_cast<float>(i) * bw + 0.5f, r.y + (r.h - h) / 2, bw - 1.0f, h}, col);
      }
      break;
    }
    case SQ_TRACK_INSTRUMENT: {
      c.strokeRoundRect(r, 1.5f, col, 1.2f);
      for (int i = 1; i < 4; ++i) {
        const float x = r.x + r.w * static_cast<float>(i) / 4.0f;
        c.fillRect(Rect{x - 1.0f, r.y + 1, 2.0f, r.h * 0.55f}, col);
      }
      break;
    }
    case SQ_TRACK_FOLDER: {
      c.fillRoundRect(Rect{r.x, r.y + 1, r.w * 0.45f, 3}, 1, col);
      c.strokeRoundRect(Rect{r.x, r.y + 3, r.w, r.h - 3}, 1.5f, col, 1.2f);
      break;
    }
    default: {  // bus / fx: summing arrows
      c.strokeEllipse(r.inset(1), col, 1.2f);
      c.drawLine(Point{r.x + 3, r.y + r.h / 2}, Point{r.right() - 3, r.y + r.h / 2}, col, 1.2f);
      c.drawLine(Point{r.x + r.w / 2, r.y + 3}, Point{r.x + r.w / 2, r.bottom() - 3}, col, 1.2f);
      break;
    }
  }
}

void toolIcon(Canvas& c, const Rect& r, int tool, Color col) {
  const float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
  if (tool == 0) {  // pointer arrow
    c.fillPolygon({Point{cx - 4, cy - 7}, Point{cx + 5, cy + 1}, Point{cx + 0.5f, cy + 1.5f}, Point{cx + 3, cy + 6.5f}, Point{cx + 1, cy + 7.5f},
                   Point{cx - 1.5f, cy + 2.5f}, Point{cx - 4, cy + 5}},
                  col);
  } else if (tool == 1) {  // razor: a cut line through a clip
    c.strokeRoundRect(Rect{cx - 7, cy - 4, 14, 8}, 1.5f, col, 1.2f);
    c.fillRect(Rect{cx - 0.75f, cy - 8, 1.5f, 16}, col);
  } else {  // pencil
    c.fillPolygon({Point{cx - 6, cy + 6}, Point{cx - 5, cy + 2}, Point{cx + 3, cy - 6}, Point{cx + 6, cy - 3}, Point{cx - 2, cy + 5}}, col);
  }
}

}  // namespace

// ---- main paint -------------------------------------------------------------------------------------
void ArrangeView::paint(Canvas& c) {
  refresh();
  peaksPending_ = false;
  const Palette& p = ctx_.palette();
  const Rect& f = frame();
  if (f.w < 50 || f.h < 80) {
    c.fillRect(f, p.trackBg);
    return;
  }
  headerW_ = std::clamp(headerW_, 150.0f, std::max(150.0f, std::min(440.0f, f.w * 0.5f)));
  clampScroll();

  // Follow the playhead page-wise while playing.
  const auto ts = ctx_.transport();
  const bool playing = (ts.flags & SQ_TRANSPORT_PLAYING) != 0;
  if (playing && follow_ && drag_ == Drag::None) {
    const double span = visibleSpan();
    if (ts.position > scroll_ + span * 0.97 || ts.position < scroll_) scroll_ = std::max(0.0, ts.position - span * 0.03);
  }
  wasPlaying_ = playing;
  trackRecording();

  c.pushClip(f);
  c.fillRect(f, p.trackBg);
  const Rect L = lanesRect();

  // lanes + headers
  c.pushClip(Rect{f.x, L.y, f.w, L.h});
  paintGrid(c, L);
  for (size_t i = 0; i < lanes_.size(); ++i) {
    const Lane& l = lanes_[i];
    const float top = laneTop(l);
    if (top > L.bottom() || top + l.total < L.y) continue;
    paintLane(c, static_cast<int>(i));
  }
  paintOverlayLines(c);
  paintDragPreview(c);
  for (size_t i = 0; i < lanes_.size(); ++i) {
    const Lane& l = lanes_[i];
    const float top = laneTop(l);
    if (top > L.bottom() || top + l.total < L.y) continue;
    paintHeader(c, static_cast<int>(i));
  }
  // area below the last track
  const float endY = L.y + contentH_ - scrollY_;
  if (endY < L.bottom()) {
    const Rect hb{f.x, endY, headerW_, L.bottom() - endY};
    c.fillRect(hb, draw::mix(p.panel, p.bg, 0.4f));
    if (lanes_.empty()) {
      c.drawText("No tracks yet", Rect{L.x, L.y + L.h * 0.38f, L.w, 22}, p.dim, 15, TextAlign::Center);
      c.drawText("Drop audio or MIDI files here, or use  + Track  in the toolbar", Rect{L.x, L.y + L.h * 0.38f + 24, L.w, 18}, p.faint, 12,
                 TextAlign::Center);
    } else if (hb.h > 30) {
      c.drawText("Double-click to add a track", Rect{hb.x, endY + 8, hb.w, 16}, p.faint, 11, TextAlign::Center);
    }
  }
  c.fillRect(Rect{f.x + headerW_ - 1, L.y, 1, L.h}, p.border);
  if (hover_.kind == HitKind::HeaderBorder || drag_ == Drag::HeaderResize) c.fillRect(Rect{f.x + headerW_ - 2, L.y, 3, L.h}, p.accent.withAlpha(0.7f));
  // rubber band
  if (drag_ == Drag::Rubber && moved_) {
    c.fillRect(rubber_, p.accent.withAlpha(0.12f));
    c.strokeRect(rubber_, p.accent.withAlpha(0.8f), 1);
  }
  // file drop feedback
  if (!ctx_.draggedFile().empty()) {
    c.strokeRect(L.inset(1), p.accent, 2);
    if (dropX_ >= L.x && dropY_ >= L.y) {
      const double at = snap(ppqAt(dropX_), 0);
      const float x = xOf(at);
      c.fillRect(Rect{x - 1, L.y, 2, L.h}, p.accent);
      const std::string nm = ctx_.draggedFile().substr(ctx_.draggedFile().find_last_of("/\\") + 1);
      const float tw = c.measureText(nm, 11.5f) + 14;
      c.fillRoundRect(Rect{x + 4, dropY_ - 11, tw, 22}, 4, p.accent);
      c.drawText(nm, Rect{x + 11, dropY_ - 11, tw - 14, 22}, onColor(p.accent), 11.5f);
    }
  }
  c.popClip();

  paintScrollbars(c);
  paintRuler(c);
  paintMarkers(c);
  paintCorner(c);
  paintToolbar(c);
  c.popClip();

  // keep repainting while waveforms load
  if (peaksPending_ || peaks_.busy()) ctx_.invalidate();
}

// ---- toolbar ----------------------------------------------------------------------------------------
void ArrangeView::paintToolbar(Canvas& c) {
  const Palette& p = ctx_.palette();
  const Rect r = toolbarRect();
  c.fillRect(r, p.panel);
  c.fillRect(Rect{r.x, r.bottom() - 1, r.w, 1}, p.border);
  toolButtons_.clear();
  const float y = r.y + 5, h = r.h - 10;
  float x = r.x + 10;
  auto btn = [&](int id, float w, std::string_view label, bool on, bool accentOn = true) {
    const Rect b{x, y, w, h};
    toolButtons_.push_back(ToolButton{b, id});
    const bool hot = hotTool_ == id;
    if (on) c.fillRoundRect(b, 4, accentOn ? p.accent : p.panel3);
    else c.fillRoundRect(b, 4, hot ? p.panel3 : p.panel2);
    if (!on) c.strokeRoundRect(b, 4, p.border, 1);
    if (!label.empty()) c.drawText(label, b, on && accentOn ? onColor(p.accent) : p.text, 11.5f, TextAlign::Center);
    x += w;
    return b;
  };
  // tool segment
  c.drawText("Tool", Rect{x, y, 30, h}, p.dim, 11);
  x += 32;
  const char* toolNames[] = {"Select", "Split", "Draw"};
  for (int i = 0; i < 3; ++i) {
    const bool on = static_cast<int>(tool_) == i;
    const Rect b = btn(kTPointer + i, 68, "", on);
    const Color fg = on ? onColor(p.accent) : p.text;
    toolIcon(c, Rect{b.x + 4, b.y, 18, b.h}, i, fg);
    c.drawText(toolNames[i], Rect{b.x + 22, b.y, b.w - 24, b.h}, fg, 11.5f);
    x += 2;
  }
  x += 14;
  c.fillRect(Rect{x - 8, y + 2, 1, h - 4}, p.border);
  // snap + grid
  btn(kTSnap, 50, "Snap", snapOn());
  x += 4;
  {
    std::string g;
    const double gv = es_.grid;
    const double bar = barLength(0);
    struct N {
      double v;
      const char* n;
    };
    const N names[] = {{bar, "Bar"},          {2.0, "1/2"},         {1.0, "1/4"},         {0.5, "1/8"},       {0.25, "1/16"},
                       {0.125, "1/32"},       {2.0 / 3.0, "1/4 T"}, {1.0 / 3.0, "1/8 T"}, {1.0 / 6.0, "1/16 T"}};
    for (const auto& n : names)
      if (g.empty() && std::fabs(gv - n.v) < 1e-6) g = n.n;
    if (g.empty()) {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%.3g q", gv);
      g = buf;
    }
    const Rect b = btn(kTGrid, 70, "", false);
    c.drawText(g, Rect{b.x + 8, b.y, b.w - 22, b.h}, snapOn() ? p.text : p.dim, 11.5f);
    draw::triangle(c, Rect{b.right() - 14, b.y + b.h / 2 - 2, 7, 4}, 2, p.dim);
  }
  x += 18;
  c.fillRect(Rect{x - 8, y + 2, 1, h - 4}, p.border);
  // zoom
  {
    const Rect zo = btn(kTZoomOut, 26, "", false);
    c.fillRect(Rect{zo.x + 8, zo.y + zo.h / 2 - 0.75f, 10, 1.5f}, p.text);
    x += 2;
    const Rect zi = btn(kTZoomIn, 26, "", false);
    c.fillRect(Rect{zi.x + 8, zi.y + zi.h / 2 - 0.75f, 10, 1.5f}, p.text);
    c.fillRect(Rect{zi.x + 12.25f, zi.y + zi.h / 2 - 5, 1.5f, 10}, p.text);
    x += 2;
    btn(kTFit, 36, "Fit", false);
  }
  x += 14;
  c.fillRect(Rect{x - 8, y + 2, 1, h - 4}, p.border);
  btn(kTFollow, 56, "Follow", follow_);
  // right side: add track
  x = r.right() - 96;
  if (x > r.x + 560) {
    const Rect b = btn(kTAddTrack, 86, "", false);
    c.drawText("+ Track", Rect{b.x + 10, b.y, b.w - 24, b.h}, p.text, 11.5f);
    draw::triangle(c, Rect{b.right() - 14, b.y + b.h / 2 - 2, 7, 4}, 2, p.dim);
  } else {
    x = r.x + 560;
    if (x + 86 < r.right()) btn(kTAddTrack, 86, "+ Track", false);
  }
  // tool hint in the middle
  const float hintX = r.x + 590;
  if (r.right() - 110 - hintX > 200) {
    const char* hint = tool_ == Tool::Split ? "Click a clip to split it at the mouse"
                       : tool_ == Tool::Draw ? "Click an instrument track to create a 1-bar MIDI clip"
                                             : "Drag: move  Ctrl/Alt+drag: copy  Shift: no snap  Alt+click: split";
    if (c.measureText(hint, 11) < r.right() - 110 - hintX) c.drawText(hint, Rect{hintX, y, r.right() - 110 - hintX, h}, p.faint, 11, TextAlign::Center);
  }
}

// ---- corner: position readout ------------------------------------------------------------------------
void ArrangeView::paintCorner(Canvas& c) {
  const Palette& p = ctx_.palette();
  const Rect r = cornerRect();
  c.fillRect(r, p.panel);
  c.fillRect(Rect{r.x, r.bottom() - 1, r.w, 1}, p.border);
  c.fillRect(Rect{r.right() - 1, r.y, 1, r.h}, p.border);
  const auto ts = ctx_.transport();
  const bool rec = (ts.flags & SQ_TRANSPORT_RECORDING) != 0;
  c.drawTextBold(ctx_.formatBars(ts.position), Rect{r.x + 12, r.y + 3, r.w - 20, 20}, rec ? p.rec : p.text, 14);
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.2f bpm", ts.bpm);
  c.drawText(GuiContext::formatTime(ts.seconds), Rect{r.x + 12, r.y + 22, r.w - 20, 16}, p.dim, 11.5f);
  c.drawText(buf, Rect{r.x + 12, r.y + 22, r.w - 22, 16}, p.faint, 11, TextAlign::Right);
}

// ---- ruler ------------------------------------------------------------------------------------------
void ArrangeView::paintRuler(Canvas& c) {
  const Palette& p = ctx_.palette();
  const Rect r = rulerRect();
  c.fillRect(Rect{r.x, r.y, r.w + kScrollW, r.h}, p.panel);
  c.pushClip(r);
  const float mid = r.y + r.h * 0.52f;
  // loop range (lower half)
  const auto ts = ctx_.transport();
  double ls = ts.loop_start, le = ts.loop_end;
  if (drag_ == Drag::LoopNew || drag_ == Drag::LoopStart || drag_ == Drag::LoopEnd || drag_ == Drag::LoopMove) {
    ls = dragA_;
    le = dragB_;
  }
  const bool looping = (ts.flags & SQ_TRANSPORT_LOOPING) != 0;
  if (le > ls) {
    const float x0 = xOf(ls), x1 = xOf(le);
    const Rect band{x0, mid + 1, x1 - x0, r.bottom() - mid - 3};
    c.fillRoundRect(band, 3, looping ? p.accent.withAlpha(0.55f) : p.faint.withAlpha(0.45f));
    c.fillRect(Rect{x0, mid + 1, 2, band.h}, looping ? p.accent : p.dim);
    c.fillRect(Rect{x1 - 2, mid + 1, 2, band.h}, looping ? p.accent : p.dim);
  }
  if (hover_.kind == HitKind::LoopZone && drag_ == Drag::None) c.fillRect(Rect{r.x, mid, r.w, r.bottom() - mid}, p.text.withAlpha(0.03f));
  // ticks + labels: pick a bar step so labels are >= 56 px apart
  const double barPx = barLength(scroll_) * ppx_;
  int step = 1;
  while (barPx * step < 56 && step < 4096) step *= 2;
  const double from = ppqAt(r.x), to = ppqAt(r.right());
  forEachBar(from, to + barLength(to), [&](int64_t idx, double pos, double bar, double beat, uint32_t num) {
    const float x = xOf(pos);
    const bool labeled = idx % step == 0;
    c.fillRect(Rect{x, labeled ? r.y + 2 : mid - 5, 1, labeled ? mid - r.y - 2 : 5}, labeled ? p.dim : p.faint);
    if (labeled) c.drawText(std::to_string(idx + 1), Rect{x + 4, r.y + 1, 50, mid - r.y - 1}, p.text, 11.5f);
    // beats
    const double beatPx = beat * ppx_;
    if (beatPx >= 7) {
      for (uint32_t b = 1; b < num; ++b) {
        const float bx = xOf(pos + beat * b);
        if (bx < r.x - 60 || bx > r.right()) continue;
        c.fillRect(Rect{bx, mid - 4, 1, 4}, p.faint);
        if (beatPx >= 46) {
          char buf[48];
          std::snprintf(buf, sizeof buf, "%lld.%u", static_cast<long long>(idx + 1), b + 1);
          c.drawText(buf, Rect{bx + 3, r.y + 3, 40, mid - r.y - 3}, p.faint, 10);
        }
      }
    }
    // sub-beat ticks (and bar.beat.tick labels) when zoomed in far enough
    const double sub = subdivision(beat, 12.0);
    if (sub < beat - 1e-12) {
      const double labelSub = subdivision(beat, 76.0);
      const double s0 = std::max(pos, pos + std::floor((from - pos) / sub) * sub);
      for (double s = s0 + sub; s < std::min(pos + bar, to) - 1e-9; s += sub) {
        const double inBeat = std::fmod(s - pos, beat);
        if (inBeat < 1e-9 || beat - inBeat < 1e-9) continue;
        const float sx = xOf(s);
        const double li = std::fmod(inBeat, labelSub);
        const bool lab = labelSub < beat - 1e-12 && (li < 1e-9 || labelSub - li < 1e-9);
        c.fillRect(Rect{sx, lab ? mid - 4 : mid - 2, 1, lab ? 4.0f : 2.0f}, p.faint.withAlpha(lab ? 1.0f : 0.7f));
        if (lab) {
          const auto beatNo = static_cast<unsigned>(std::floor((s - pos) / beat + 1e-9)) + 1u;
          const int tick = std::clamp(static_cast<int>(std::lround(inBeat / beat * 960.0)), 0, 959);
          char buf[48];
          std::snprintf(buf, sizeof buf, "%lld.%u.%03d", static_cast<long long>(idx + 1), beatNo, tick);
          c.drawText(buf, Rect{sx + 3, r.y + 3, 70, mid - r.y - 3}, p.faint, 10);
        }
      }
    }
  });
  // edit cursor + playhead heads
  const float cx = xOf(es_.cursor);
  draw::triangle(c, Rect{cx - 5, r.bottom() - 6, 10, 6}, 0, p.cursor);
  const float px = xOf(ts.position);
  const bool rec = (ts.flags & SQ_TRANSPORT_RECORDING) != 0;
  draw::triangle(c, Rect{px - 6, r.y, 12, 8}, 2, rec ? p.rec : p.playhead);
  c.fillRect(Rect{px - 0.5f, r.y, 1.5f, r.h}, rec ? p.rec : p.playhead);
  // hover line
  if ((hover_.kind == HitKind::Ruler || hover_.kind == HitKind::LoopZone) && mouseX_ >= r.x) c.fillRect(Rect{mouseX_, r.y, 1, r.h}, p.text.withAlpha(0.35f));
  c.popClip();
  c.fillRect(Rect{r.x, r.bottom() - 1, r.w + kScrollW, 1}, p.border);
}

void ArrangeView::paintMarkers(Canvas& c) {
  const Palette& p = ctx_.palette();
  const Rect r = markerRect();
  c.fillRect(Rect{r.x, r.y, r.w + kScrollW, r.h}, draw::mix(p.panel, p.bg, 0.35f));
  c.fillRect(Rect{r.x, r.bottom() - 1, r.w + kScrollW, 1}, p.border);
  c.pushClip(r);
  for (size_t i = 0; i < markers_.size(); ++i) {
    auto& mv = markers_[i];
    double pos = mv.m.position;
    if (drag_ == Drag::MarkerMove && downHit_.marker == static_cast<int>(i)) pos = dragA_;
    const float x = xOf(pos);
    const Color col = mv.m.color && (mv.m.color & 0xFF) ? Color::rgba(mv.m.color) : p.accent2;
    const float tw = std::min(160.0f, c.measureText(mv.m.name, 11) + 12);
    mv.x = x;
    mv.w = tw;
    if (x + tw < r.x || x > r.right()) continue;
    if (mv.m.length > 0) c.fillRect(Rect{x, r.bottom() - 4, static_cast<float>(mv.m.length * ppx_), 3}, col.withAlpha(0.6f));
    const bool hot = hover_.kind == HitKind::Marker && hover_.marker == static_cast<int>(i);
    c.fillRoundRect(Rect{x, r.y + 2, tw, r.h - 5}, 3, hot ? draw::lighten(col, 0.2f) : col);
    c.fillRect(Rect{x, r.y + 2, 2, r.h - 3}, draw::lighten(col, 0.4f));
    c.drawText(mv.m.name, Rect{x + 6, r.y + 2, tw - 8, r.h - 5}, onColor(col), 11);
  }
  if (hover_.kind == HitKind::MarkerStrip && mouseX_ >= r.x) {
    const float x = xOf(snap(ppqAt(mouseX_), 0));
    c.fillRect(Rect{x, r.y + 2, 1, r.h - 4}, p.dim.withAlpha(0.6f));
  }
  c.popClip();
}

// ---- grid -------------------------------------------------------------------------------------------
void ArrangeView::paintGrid(Canvas& c, const Rect& L) {
  const Palette& p = ctx_.palette();
  // lane backgrounds
  for (size_t i = 0; i < lanes_.size(); ++i) {
    const Lane& l = lanes_[i];
    const float top = laneTop(l);
    if (top > L.bottom() || top + l.total < L.y) continue;
    const bool selT = l.t.id == es_.track;
    Color bg = (i % 2) ? draw::mix(p.trackBg, p.bg, 0.25f) : p.trackBg;
    if (selT) bg = draw::mix(bg, p.trackSel, 0.6f);
    c.fillRect(Rect{L.x, top, L.w, l.h}, bg);
    for (const auto& a : l.autos) c.fillRect(Rect{L.x, L.y + a.y - scrollY_, L.w, a.h}, draw::mix(p.trackBg, p.bg, 0.55f));
  }
  const float endY = std::min(L.bottom(), L.y + contentH_ - scrollY_);
  if (endY < L.bottom()) c.fillRect(Rect{L.x, endY, L.w, L.bottom() - endY}, draw::mix(p.trackBg, p.bg, 0.5f));
  // loop shading
  const auto ts = ctx_.transport();
  if ((ts.flags & SQ_TRANSPORT_LOOPING) && ts.loop_end > ts.loop_start) {
    const float x0 = std::max(L.x, xOf(ts.loop_start)), x1 = std::min(L.right(), xOf(ts.loop_end));
    if (x1 > x0) c.fillRect(Rect{x0, L.y, x1 - x0, L.h}, p.accent.withAlpha(0.06f));
  }
  // vertical lines
  const double from = ppqAt(L.x), to = ppqAt(L.right());
  const double barPx = barLength(scroll_) * ppx_;
  int step = 1;
  while (barPx * step < 12 && step < 4096) step *= 2;
  const double g = es_.grid > 0 ? es_.grid : 1.0;
  forEachBar(from, to + 1e-9, [&](int64_t idx, double pos, double bar, double beat, uint32_t num) {
    if (idx % step == 0) c.fillRect(Rect{xOf(pos), L.y, 1, L.h}, p.gridBar);
    if (step > 1) return;
    if (beat * ppx_ >= 14) {
      for (uint32_t b = 1; b < num; ++b) c.fillRect(Rect{xOf(pos + beat * b), L.y, 1, L.h}, p.grid);
    }
    // grid subdivisions: the snap grid when it is finer than a beat, and at high zoom at least an
    // adaptive power-of-two subdivision so the lanes never go blank between beats
    double sg = g < beat - 1e-9 && g * ppx_ >= 9 ? g : beat;
    const double adaptive = subdivision(beat, 24.0);
    if (adaptive < sg - 1e-12 && beat * ppx_ >= 96) sg = adaptive;
    if (sg < beat - 1e-9) {
      const Color sub = draw::mix(p.trackBg, p.grid, 0.55f);
      const double s0 = std::max(pos, pos + std::floor((from - pos) / sg) * sg);
      for (double s = s0 + sg; s < std::min(pos + bar, to + sg) - 1e-9; s += sg) {
        const double inBeat = std::fmod(s - pos, beat);
        if (inBeat < 1e-6 || beat - inBeat < 1e-6) continue;
        c.fillRect(Rect{xOf(s), L.y, 1, L.h}, sub);
      }
    }
  });
}

// ---- one lane (clips + automation) --------------------------------------------------------------
void ArrangeView::paintLane(Canvas& c, int idx) {
  const Palette& p = ctx_.palette();
  const Lane& l = lanes_[static_cast<size_t>(idx)];
  const Rect L = lanesRect();
  const float top = laneTop(l);
  c.fillRect(Rect{L.x, top + l.h - 1, L.w, 1}, p.border);
  const double from = ppqAt(L.x), to = ppqAt(L.right());
  const bool moving = drag_ == Drag::ClipMove && moved_;
  c.pushClip(intersect(Rect{L.x, top, L.w, l.h}, L));
  for (size_t ci = 0; ci < l.clips.size(); ++ci) {
    const ClipEntry& ce = l.clips[ci];
    if (ce.v.info.start > to) break;
    if (ce.end() < from) continue;
    const bool sel = selected(ce.v.id);
    // live previews of edge/fade drags
    if (ce.v.id == dragNode_ && moved_ && (drag_ == Drag::TrimLeft || drag_ == Drag::TrimRight || drag_ == Drag::FadeIn || drag_ == Drag::FadeOut)) {
      ClipEntry tmp = ce;
      if (drag_ == Drag::TrimLeft || drag_ == Drag::TrimRight) {
        if (tmp.v.info.kind == SQ_CLIP_AUDIO) tmp.v.info.source_offset_seconds += secAt(dragA_) - secAt(ce.v.info.start);
        else
          for (auto& n : tmp.notes) n.pos -= static_cast<float>(dragA_ - ce.v.info.start);
        tmp.v.info.start = dragA_;
        tmp.v.info.length = dragB_;
      } else if (drag_ == Drag::FadeIn) {
        tmp.v.info.fade_in_seconds = dragA_;
      } else {
        tmp.v.info.fade_out_seconds = dragA_;
      }
      paintClip(c, l, tmp, clipRect(l, tmp), sel, false);
      continue;
    }
    const bool dim = moving && sel && !dragCopy_;
    paintClip(c, l, ce, clipRect(l, ce), sel, dim);
  }
  paintRecordRegion(c, l);
  c.popClip();
  if (l.showAuto)
    for (size_t ai = 0; ai < l.autos.size(); ++ai) paintAutoLane(c, l, static_cast<int>(ai));
}

// Remembers where the running take started: the stopped position when recording starts from
// stop, else the position at which recording was first seen (punch-in on the fly).
void ArrangeView::trackRecording() {
  const auto ts = ctx_.transport();
  const bool rec = (ts.flags & SQ_TRANSPORT_RECORDING) != 0;
  const bool playing = (ts.flags & SQ_TRANSPORT_PLAYING) != 0;
  if (rec && !wasRecording_) recStart_ = lastPlaying_ ? ts.position : std::min(lastPos_, ts.position);
  wasRecording_ = rec;
  lastPlaying_ = playing || rec;
  lastPos_ = ts.position;
}

// Finest power-of-two subdivision of a beat whose spacing is at least minPx (beat itself: none).
double ArrangeView::subdivision(double beat, double minPx) const {
  double s = beat;
  while (s / 2 * ppx_ >= minPx && s > beat / 256) s /= 2;
  return s;
}

// The take being recorded on an armed track: a growing red region from the record start (or the
// punch-in) to the playhead.
void ArrangeView::paintRecordRegion(Canvas& c, const Lane& l) {
  if (!wasRecording_ || !(l.t.info.flags & SQ_TRACK_ARMED) || !(l.audio() || l.instrument())) return;
  const Palette& p = ctx_.palette();
  const auto ts = ctx_.transport();
  double a = recStart_, b = ts.position;
  if ((ts.flags & SQ_TRANSPORT_PUNCH) && punchOut_ > punchIn_) {
    a = std::max(a, punchIn_);
    b = std::min(b, punchOut_);
  }
  if (b <= a) return;
  const float x0 = xOf(a), x1 = std::max(xOf(b), x0 + 2);
  const Rect r{x0, laneTop(l) + 2, x1 - x0, l.h - 5};
  c.fillRoundRect(r, std::min(4.0f, r.w / 2), p.rec.withAlpha(0.28f));
  c.strokeRoundRect(r, std::min(4.0f, r.w / 2), p.rec, 1.2f);
  if (r.h >= 30 && r.w > 40) {
    c.fillRect(Rect{r.x + 1, r.y + 1, r.w - 2, 15}, p.rec.withAlpha(0.55f));
    c.drawText("Recording", Rect{r.x + 6, r.y + 1, r.w - 10, 15}, onColor(p.rec), 10.5f);
  }
}

void ArrangeView::paintClip(Canvas& c, const Lane& l, const ClipEntry& ce, const Rect& r, bool sel, bool ghost) {
  const Palette& p = ctx_.palette();
  const auto& ci = ce.v.info;
  const bool muted = (ci.flags & SQ_CLIP_MUTED) != 0;
  Color base = ci.color && (ci.color & 0xFF) ? Color::rgba(ci.color) : l.color;
  if (muted) base = draw::mix(base, p.faint, 0.7f);
  const bool trackMuted = (l.t.info.flags & SQ_TRACK_MUTE) != 0;
  const float alpha = ghost ? 0.35f : (muted ? 0.55f : (trackMuted ? 0.6f : 1.0f));
  const Color laneBg = l.t.id == es_.track ? p.trackSel : p.trackBg;
  const Color body = draw::mix(laneBg, base, p.dark ? 0.42f : 0.35f).withAlpha(alpha);
  const Color head = draw::mix(laneBg, base, sel ? 0.95f : 0.8f).withAlpha(alpha);
  const float rad = std::min(4.0f, r.w / 2);
  c.fillRoundRect(r, rad, body);
  const bool showHead = r.h >= 30;
  const float headH = showHead ? 15.0f : 0.0f;
  if (showHead) {
    c.pushClip(Rect{r.x, r.y, r.w, headH});
    c.fillRoundRect(Rect{r.x, r.y, r.w, headH + rad}, rad, head);
    c.popClip();
  }
  const Rect bodyR{r.x, r.y + headH, r.w, r.h - headH};
  const Color ink = p.dark ? draw::lighten(base, 0.55f).withAlpha(0.9f * alpha) : draw::mix(base, Color::rgb(0x000000), 0.45f).withAlpha(0.85f * alpha);
  if (ci.kind == SQ_CLIP_AUDIO) paintWaveform(c, ce, bodyR, ink);
  else paintNotes(c, ce, bodyR, ink);

  // fades (audio)
  if (ci.kind == SQ_CLIP_AUDIO) {
    const float fi = std::min(fadeInX(ce), r.right()), fo = std::max(fadeOutX(ce), r.x);
    const Color shade = Color::rgb(0x000000, p.dark ? 0.28f : 0.14f);
    if (ci.fade_in_seconds > 0 && fi > r.x + 1) {
      c.fillPolygon({Point{r.x, bodyR.y}, Point{fi, bodyR.y}, Point{r.x, r.bottom()}}, shade);
      c.drawLine(Point{r.x, r.bottom()}, Point{fi, bodyR.y}, p.text.withAlpha(0.7f * alpha), 1.2f);
    }
    if (ci.fade_out_seconds > 0 && fo < r.right() - 1) {
      c.fillPolygon({Point{fo, bodyR.y}, Point{r.right(), bodyR.y}, Point{r.right(), r.bottom()}}, shade);
      c.drawLine(Point{fo, bodyR.y}, Point{r.right(), r.bottom()}, p.text.withAlpha(0.7f * alpha), 1.2f);
    }
    const bool hotClip = hover_.lane >= 0 && hover_.clip >= 0 && static_cast<size_t>(hover_.lane) < lanes_.size() &&
                         static_cast<size_t>(hover_.clip) < lanes_[static_cast<size_t>(hover_.lane)].clips.size() &&
                         lanes_[static_cast<size_t>(hover_.lane)].clips[static_cast<size_t>(hover_.clip)].v.id == ce.v.id;
    if ((hotClip || sel || ce.v.id == dragNode_) && r.w > 24 && !ghost) {
      const bool hi = hotClip && hover_.kind == HitKind::FadeInHandle, ho = hotClip && hover_.kind == HitKind::FadeOutHandle;
      const float hy = r.y + (showHead ? headH : 0.0f);
      const Rect a{std::clamp(fi, r.x, r.right() - 8) - (fi > r.x + 4 ? 4.0f : 0.0f), hy - 4, 8, 8};
      const Rect b{std::clamp(fo, r.x + 8, r.right()) - (fo < r.right() - 4 ? 4.0f : 8.0f), hy - 4, 8, 8};
      c.fillRoundRect(a, 2, hi ? p.accent : p.text.withAlpha(0.85f));
      c.strokeRoundRect(a, 2, p.border, 1);
      c.fillRoundRect(b, 2, ho ? p.accent : p.text.withAlpha(0.85f));
      c.strokeRoundRect(b, 2, p.border, 1);
    }
  }
  // name
  if (r.w > 18) {
    std::string nm = ce.v.name.empty() ? (ci.kind == SQ_CLIP_MIDI ? "MIDI" : "Audio") : ce.v.name;
    if (muted) nm += "  (muted)";
    char gb[32] = "";
    if (ci.kind == SQ_CLIP_AUDIO && std::fabs(ci.gain_db) > 0.05) std::snprintf(gb, sizeof gb, "%+.1f dB", ci.gain_db);
    const Rect tr{r.x + 6, r.y, r.w - 10, showHead ? headH : std::min(16.0f, r.h)};
    c.pushClip(intersect(r, lanesRect()));
    c.drawTextBold(nm, tr, showHead ? onColor(draw::mix(laneBg, base, 0.8f)).withAlpha(alpha) : p.text.withAlpha(alpha), 11);
    if (gb[0] && r.w > 120) c.drawText(gb, tr, (showHead ? onColor(draw::mix(laneBg, base, 0.8f)) : p.text).withAlpha(0.75f * alpha), 10, TextAlign::Right);
    c.popClip();
  }
  // outline: selection / hover / edges
  const bool hot = !ghost && hover_.lane >= 0 && hover_.clip >= 0 && static_cast<size_t>(hover_.lane) < lanes_.size() &&
                   static_cast<size_t>(hover_.clip) < lanes_[static_cast<size_t>(hover_.lane)].clips.size() &&
                   lanes_[static_cast<size_t>(hover_.lane)].clips[static_cast<size_t>(hover_.clip)].v.id == ce.v.id && drag_ == Drag::None;
  if (sel) c.strokeRoundRect(r, rad, p.dark ? Color::rgb(0xFFFFFF, alpha) : Color::rgb(0x111111, alpha), 1.6f);
  else if (hot) c.strokeRoundRect(r, rad, draw::lighten(base, 0.45f), 1.2f);
  else c.strokeRoundRect(r, rad, draw::mix(base, Color::rgb(0x000000), 0.35f).withAlpha(0.8f * alpha), 1);
  if (hot && (hover_.kind == HitKind::ClipEdgeL || hover_.kind == HitKind::ClipEdgeR)) {
    const float ex = hover_.kind == HitKind::ClipEdgeL ? r.x : r.right() - 3;
    c.fillRoundRect(Rect{ex, r.y + 2, 3, r.h - 4}, 1.5f, p.text);
  }
  if (hot && tool_ == Tool::Split && mouseX_ > r.x && mouseX_ < r.right()) {
    const float sx = xOf(snap(ppqAt(mouseX_), downMods_));
    c.fillRect(Rect{sx, r.y, 1.5f, r.h}, p.rec);
  }
}

void ArrangeView::paintWaveform(Canvas& c, const ClipEntry& ce, const Rect& body, Color col) {
  const auto& ci = ce.v.info;
  if (ce.v.source.empty() || body.h < 6) return;
  auto pd = peaks_.request(ce.v.source);
  const Rect L = lanesRect();
  const float x0 = std::max(body.x, L.x), x1 = std::min(body.right(), L.right());
  if (x1 <= x0) return;
  if (!pd || pd->state() != PeakData::Ready) {
    // loading / missing: hatch
    c.pushClip(Rect{x0, body.y, x1 - x0, body.h});
    const bool failed = pd && pd->state() == PeakData::Failed;
    const Color hc = col.withAlpha(failed ? 0.25f : 0.35f);
    const float step = 9;
    const float start = std::floor((x0 - body.h) / step) * step;
    for (float x = start; x < x1; x += step) c.drawLine(Point{x, body.bottom()}, Point{x + body.h, body.y}, hc, 1);
    c.popClip();
    if (body.h >= 14)
      c.drawText(failed ? "media offline" : "loading...", Rect{std::max(body.x, L.x) + 6, body.y, 120, std::min(body.h, 18.0f)}, col, 10.5f);
    if (!failed) peaksPending_ = true;
    return;
  }
  const AudioFileInfo& info = pd->info();
  const double sr = info.sampleRate;
  const double clipSec0 = secAt(ci.start);
  const double clipLenSec = secAt(ci.start + ci.length) - clipSec0;
  const double gain = std::pow(10.0, ci.gain_db / 20.0);
  const bool stereo = info.channels >= 2 && body.h >= 40;
  const int rows = stereo ? 2 : 1;
  const float rowH = body.h / static_cast<float>(rows);
  for (int row = 0; row < rows; ++row) {
    const float cy = body.y + rowH * (static_cast<float>(row) + 0.5f);
    c.fillRect(Rect{x0, cy - 0.5f, x1 - x0, 1}, col.withAlpha(0.25f));
  }
  double secPrev = secAt(ppqAt(x0)) - clipSec0;
  for (float x = std::floor(x0); x < x1; x += 1.0f) {
    const double secNext = secAt(ppqAt(x + 1.0f)) - clipSec0;
    const double s0 = secPrev, s1 = secNext;
    secPrev = secNext;
    if (s1 <= 0 || s0 >= clipLenSec) continue;
    double env = gain;
    const double mids = 0.5 * (s0 + s1);
    if (ci.fade_in_seconds > 0 && mids < ci.fade_in_seconds) env *= std::max(0.0, mids / ci.fade_in_seconds);
    if (ci.fade_out_seconds > 0 && mids > clipLenSec - ci.fade_out_seconds) env *= std::max(0.0, (clipLenSec - mids) / ci.fade_out_seconds);
    const double f0 = (s0 + ci.source_offset_seconds) * sr, f1 = (s1 + ci.source_offset_seconds) * sr;
    for (int row = 0; row < rows; ++row) {
      float mn = 0, mx = 0;
      if (!pd->range(stereo ? static_cast<uint32_t>(row) : 0xFFFFu, f0, f1, &mn, &mx)) continue;
      const float half = rowH * 0.5f - 1.0f;
      const float cy = body.y + rowH * (static_cast<float>(row) + 0.5f);
      const float top = cy - std::clamp(static_cast<float>(mx * env), -1.0f, 1.0f) * half;
      const float bot = cy - std::clamp(static_cast<float>(mn * env), -1.0f, 1.0f) * half;
      c.fillRect(Rect{x, top, 1.0f, std::max(1.0f, bot - top)}, col);
    }
  }
}

void ArrangeView::paintNotes(Canvas& c, const ClipEntry& ce, const Rect& body, Color col) {
  if (ce.notes.empty() || body.h < 6) return;
  const Rect L = lanesRect();
  const Rect clip = intersect(body, L);
  if (clip.w <= 0) return;
  c.pushClip(clip);
  const float inner = body.h - 4;
  const int span = std::max(1, ce.hi - ce.lo + 1);
  const float nh = std::max(1.5f, std::min(6.0f, inner / static_cast<float>(span)));
  const float usedH = nh * static_cast<float>(span);
  const float y0 = body.y + 2 + (inner - usedH) / 2;
  const double start = ce.v.info.start;
  for (const auto& n : ce.notes) {
    if (n.pos >= ce.v.info.length || n.pos + n.len <= 0) continue;
    const float x = xOf(start + n.pos);
    if (x > clip.right()) break;
    const float w = std::max(1.5f, static_cast<float>(n.len * ppx_) - 1);
    if (x + w < clip.x) continue;
    const float y = y0 + static_cast<float>(ce.hi - n.pitch) * nh;
    c.fillRect(Rect{x, y, w, std::max(1.0f, nh - (nh > 3 ? 1.0f : 0.0f))}, col.withAlpha(col.a * (0.55f + 0.45f * static_cast<float>(n.vel) / 127.0f)));
  }
  c.popClip();
}

// ---- automation lanes -------------------------------------------------------------------------------
void ArrangeView::paintAutoLane(Canvas& c, const Lane& l, int ai) {
  const Palette& p = ctx_.palette();
  const AutoLane& a = l.autos[static_cast<size_t>(ai)];
  const Rect r = autoLaneRect(l, a);
  const Rect L = lanesRect();
  if (r.bottom() < L.y || r.y > L.bottom()) return;
  c.pushClip(intersect(r, L));
  c.fillRect(Rect{r.x, r.bottom() - 1, r.w, 1}, p.border);
  const bool editing = drag_ == Drag::AutoPoint && downHit_.lane >= 0 && &lanes_[static_cast<size_t>(downHit_.lane)] == &l && downHit_.autoLane == ai;
  const auto& pts = editing ? dragPoints_ : a.points;
  AutoLane view = a;
  if (editing) view.points = dragPoints_;
  // reference line (0 dB / center)
  const AutoRange rg = autoRange(a.target, view.points, a.param);
  if (a.target == "volume") {
    const float y0 = autoValueY(view, r, 0.0);
    c.fillRect(Rect{r.x, y0, r.w, 1}, p.grid);
  } else if (a.target == "pan") {
    c.fillRect(Rect{r.x, autoValueY(view, r, 0.0), r.w, 1}, p.grid);
  }
  const Color col = l.color;
  if (pts.empty()) {
    const double def = a.target == "volume" ? l.t.info.volume_db : a.target == "pan" ? l.t.info.pan : a.param ? static_cast<double>(a.param->def) : rg.lo;
    const float y = autoValueY(view, r, def);
    c.fillRect(Rect{r.x, y - 0.5f, r.w, 1.5f}, col.withAlpha(0.4f));
    c.drawText("Click to add automation points", Rect{r.x + 10, r.bottom() - 17, 300, 14}, p.faint, 10.5f);
  } else {
    std::vector<Point> line;
    line.reserve(pts.size() * 2 + 4);
    line.push_back(Point{r.x - 2, autoValueY(view, r, pts.front().second)});
    for (size_t i = 0; i < pts.size(); ++i) {
      const float x = xOf(pts[i].first), y = autoValueY(view, r, pts[i].second);
      if (rg.stepped && i > 0) line.push_back(Point{x, line.back().y});
      line.push_back(Point{x, y});
    }
    line.push_back(Point{r.right() + 2, line.back().y});
    std::vector<Point> fill = line;
    fill.push_back(Point{r.right() + 2, r.bottom()});
    fill.push_back(Point{r.x - 2, r.bottom()});
    c.fillPolygon(fill, col.withAlpha(0.13f));
    c.strokePolyline(line, draw::lighten(col, p.dark ? 0.25f : 0.0f), 1.6f);
    const bool hotLane = hover_.lane >= 0 && &lanes_[static_cast<size_t>(hover_.lane)] == &l && hover_.autoLane == ai;
    for (size_t i = 0; i < pts.size(); ++i) {
      const float x = xOf(pts[i].first);
      if (x < r.x - 6 || x > r.right() + 6) continue;
      const float y = autoValueY(view, r, pts[i].second);
      const bool hot = (hotLane && hover_.kind == HitKind::AutoPoint && hover_.point == static_cast<int>(i)) ||
                       (editing && downHit_.point == static_cast<int>(i));
      const float rad = hot ? 5.0f : 3.5f;
      c.fillEllipse(Rect{x - rad, y - rad, 2 * rad, 2 * rad}, hot ? p.text : draw::lighten(col, 0.35f));
      c.strokeEllipse(Rect{x - rad, y - rad, 2 * rad, 2 * rad}, p.border, 1);
      if (hot) {
        const std::string v = formatAutoValue(a.target, pts[i].second, a.param);
        const float tw = c.measureText(v, 10.5f) + 10;
        const float ty = y - 22 < r.y ? y + 8 : y - 22;
        c.fillRoundRect(Rect{x + 6, ty, tw, 16}, 3, p.panel3);
        c.drawText(v, Rect{x + 11, ty, tw - 10, 16}, p.text, 10.5f);
      }
    }
  }
  c.popClip();
}

void ArrangeView::paintAutoHeader(Canvas& c, const Lane& l, int ai) {
  const Palette& p = ctx_.palette();
  const AutoLane& a = l.autos[static_cast<size_t>(ai)];
  const Rect L = lanesRect();
  const Rect r{frame().x, L.y + a.y - scrollY_, headerW_ - 1, a.h};
  c.fillRect(r, draw::mix(p.panel2, p.bg, 0.35f));
  c.fillRect(Rect{r.x, r.y, 4, r.h}, l.color.withAlpha(0.45f));
  c.fillRect(Rect{r.x + 16, r.y, 1, r.h}, p.border);
  c.fillRect(Rect{r.x, r.bottom() - 1, r.w, 1}, p.border);
  const bool hot = hover_.kind == HitKind::AutoHeader && hover_.lane >= 0 && &lanes_[static_cast<size_t>(hover_.lane)] == &l && hover_.autoLane == ai;
  const Rect sel{r.x + 24, r.y + 7, std::min(r.w - 34, 150.0f), 20};
  c.fillRoundRect(sel, 4, hot ? p.panel3 : p.panel2);
  c.strokeRoundRect(sel, 4, p.border, 1);
  c.drawText(a.label.empty() ? autoLabel(a.target) : a.label, Rect{sel.x + 8, sel.y, sel.w - 24, sel.h}, a.stored ? p.text : p.dim, 11.5f);
  draw::triangle(c, Rect{sel.right() - 14, sel.y + sel.h / 2 - 2, 7, 4}, 2, p.dim);
  // value at the playhead
  const double pos = ctx_.transport().position;
  const double def = a.target == "volume" ? l.t.info.volume_db : a.target == "pan" ? l.t.info.pan : a.param ? static_cast<double>(a.param->def) : 0.0;
  double v = def;
  if (!a.points.empty()) {
    v = a.points.front().second;
    for (size_t i = 0; i < a.points.size(); ++i) {
      if (a.points[i].first <= pos) v = a.points[i].second;
      if (i + 1 < a.points.size() && a.points[i].first <= pos && a.points[i + 1].first > pos && a.target != "mute" &&
          !(a.param && a.param->kind != engine::audio::ParamKind::Continuous)) {
        const double t = (pos - a.points[i].first) / (a.points[i + 1].first - a.points[i].first);
        v = a.points[i].second + (a.points[i + 1].second - a.points[i].second) * t;
      }
    }
  }
  c.drawText(formatAutoValue(a.target, v, a.param), Rect{r.x + 24, r.y + 32, r.w - 34, 16}, p.dim, 11);
  c.drawText(std::to_string(a.points.size()) + " pts", Rect{r.x + 24, r.y + 32, r.w - 40, 16}, p.faint, 10.5f, TextAlign::Right);
}

// ---- track header -----------------------------------------------------------------------------------
void ArrangeView::paintHeader(Canvas& c, int idx) {
  const Palette& p = ctx_.palette();
  const Lane& l = lanes_[static_cast<size_t>(idx)];
  const float top = laneTop(l);
  const Rect r{frame().x, top, headerW_ - 1, l.h};
  const bool selT = l.t.id == es_.track;
  const bool hot = hover_.lane == idx && (hover_.kind == HitKind::Header || hover_.kind == HitKind::HeaderName || hover_.kind == HitKind::HeaderButton ||
                                          hover_.kind == HitKind::HeaderVolume || hover_.kind == HitKind::HeaderPan);
  Color bg = selT ? draw::mix(p.panel2, p.accent, p.dark ? 0.13f : 0.10f) : p.panel2;
  if (hot && !selT) bg = draw::mix(bg, p.panel3, 0.5f);
  c.fillRect(r, bg);
  c.fillRect(Rect{r.x, r.bottom() - 1, r.w, 1}, p.border);
  c.fillRect(Rect{r.x, r.y, 5, r.h - 1}, l.color);
  if (selT) c.fillRect(Rect{r.x + 5, r.y, 1.5f, r.h - 1}, draw::lighten(l.color, 0.3f));
  // number + icon + name
  const bool compact = l.h < 50;
  const float rowY = compact ? r.y + (l.h - 18) / 2 : r.y + 5;
  c.drawText(std::to_string(l.number), Rect{r.x + 8, rowY, 16, 18}, p.dim, 10.5f, TextAlign::Center);
  kindIcon(c, Rect{r.x + 27, rowY + 3, 13, 12}, l.t.info.kind, selT ? draw::lighten(l.color, 0.2f) : p.dim);
  const Rect nr = nameRect(l);
  c.pushClip(nr);
  c.drawTextBold(l.t.name, nr, p.text, 12.5f);
  c.popClip();
  // buttons
  static const char* kLabels[kBtnCount] = {"M", "S", "R", "I", "A"};
  for (int b = 0; b < kBtnCount; ++b) {
    if (b == kBtnArm && !(l.audio() || l.instrument())) continue;
    if (b == kBtnMonitor && !(l.audio() || l.instrument())) continue;
    const Rect br = headerButtonRect(l, b);
    if (br.y + br.h > r.bottom()) continue;
    bool on = false;
    Color onCol = p.accent;
    switch (b) {
      case kBtnMute: on = (l.t.info.flags & SQ_TRACK_MUTE) != 0; onCol = p.mute; break;
      case kBtnSolo: on = (l.t.info.flags & SQ_TRACK_SOLO) != 0; onCol = p.solo; break;
      case kBtnArm: on = (l.t.info.flags & SQ_TRACK_ARMED) != 0; onCol = p.rec; break;
      case kBtnMonitor: on = (l.t.info.flags & SQ_TRACK_MONITOR) != 0; onCol = p.accent; break;
      case kBtnAuto: on = l.showAuto; onCol = p.accent2; break;
      default: break;
    }
    const bool bh = hover_.kind == HitKind::HeaderButton && hover_.lane == idx && hover_.button == b;
    c.fillRoundRect(br, 3, on ? onCol : (bh ? p.panel3 : draw::mix(p.panel2, p.bg, 0.3f)));
    if (!on) c.strokeRoundRect(br, 3, p.border, 1);
    if (b == kBtnArm && !on) c.fillEllipse(Rect{br.x + br.w / 2 - 3.5f, br.y + br.h / 2 - 3.5f, 7, 7}, p.rec.withAlpha(0.75f));
    else c.drawTextBold(kLabels[b], br, on ? onColor(onCol) : p.dim, 10.5f, TextAlign::Center);
  }
  // volume / pan readout
  double vol = l.t.info.volume_db, pan = l.t.info.pan;
  if (drag_ == Drag::Volume && downHit_.lane == idx) vol = dragA_;
  if (drag_ == Drag::Pan && downHit_.lane == idx) pan = dragA_;
  const bool panHot = (hover_.kind == HitKind::HeaderPan && hover_.lane == idx) || (drag_ == Drag::Pan && downHit_.lane == idx);
  const bool volHot = (hover_.kind == HitKind::HeaderVolume && hover_.lane == idx) || (drag_ == Drag::Volume && downHit_.lane == idx);
  if (!compact) {
    char buf[48];
    if (panHot) {
      const int pp = static_cast<int>(std::lround(pan * 100));
      if (pp == 0) std::snprintf(buf, sizeof buf, "Pan C");
      else std::snprintf(buf, sizeof buf, "Pan %c%d", pp < 0 ? 'L' : 'R', std::abs(pp));
    } else if (vol <= -59.95) {
      std::snprintf(buf, sizeof buf, "-inf dB");
    } else {
      std::snprintf(buf, sizeof buf, "%+.1f dB", vol);
    }
    const Rect last = headerButtonRect(l, kBtnCount - 1);
    c.drawText(buf, Rect{last.right() + 4, last.y, r.right() - 18 - last.right() - 4, last.h}, panHot || volHot ? p.text : p.dim, 10.5f,
               TextAlign::Right);
  }
  const Rect vr = volumeRect(l);
  if (vr.w > 0) {
    const bool vh = volHot;
    c.fillRoundRect(vr, 3, draw::mix(p.panel2, p.bg, 0.55f));
    const float w = vr.w * static_cast<float>(volNorm(vol));
    c.fillRoundRect(Rect{vr.x, vr.y, w, vr.h}, 3, vh ? draw::lighten(p.accent, 0.15f) : p.accent.withAlpha(0.8f));
    const float zx = vr.x + vr.w * static_cast<float>(volNorm(0.0));
    c.fillRect(Rect{zx, vr.y - 1, 1, vr.h + 2}, p.dim);
    const Rect pr = panRect(l);
    const bool ph = panHot;
    c.fillRoundRect(pr, 3, draw::mix(p.panel2, p.bg, 0.55f));
    const float cx = pr.x + pr.w / 2, px = pr.x + pr.w * static_cast<float>((std::clamp(pan, -1.0, 1.0) + 1.0) / 2.0);
    c.fillRect(Rect{std::min(cx, px), pr.y, std::max(2.0f, std::fabs(px - cx)), pr.h}, ph ? draw::lighten(p.solo, 0.15f) : p.solo.withAlpha(0.8f));
    c.fillRect(Rect{cx - 0.5f, pr.y - 1, 1, pr.h + 2}, p.dim);
  }
  // meter
  float peaks[2] = {0, 0};
  const uint32_t n = ctx_.meter(l.t.id, peaks, nullptr, 2);
  float& hold = meterHold_[l.t.id];
  const float pk = n > 0 ? std::max(peaks[0], n > 1 ? peaks[1] : 0.0f) : 0.0f;
  hold = std::max(pk, hold * 0.92f);
  const Rect mr{r.right() - 11, r.y + 5, 6, r.h - 11};
  if (n >= 2) {
    draw::meter(c, p, Rect{mr.x, mr.y, 2.5f, mr.h}, peaks[0], 0);
    draw::meter(c, p, Rect{mr.x + 3.5f, mr.y, 2.5f, mr.h}, peaks[1], 0);
  } else {
    draw::meter(c, p, mr, n > 0 ? peaks[0] : 0.0f, hold);
  }
  // lane resize handle feedback
  if ((hover_.kind == HitKind::LaneBorder && hover_.lane == idx) || (drag_ == Drag::LaneResize && downHit_.lane == idx))
    c.fillRect(Rect{r.x, r.bottom() - 2, frame().w, 3}, p.accent.withAlpha(0.7f));
  if (l.showAuto)
    for (size_t ai = 0; ai < l.autos.size(); ++ai) paintAutoHeader(c, l, static_cast<int>(ai));
}

// ---- overlay lines: markers, cursor, playhead ------------------------------------------------------
void ArrangeView::paintOverlayLines(Canvas& c) {
  const Palette& p = ctx_.palette();
  const Rect L = lanesRect();
  for (const auto& mv : markers_) {
    const float x = xOf(mv.m.position);
    if (x >= L.x && x <= L.right()) c.fillRect(Rect{x, L.y, 1, L.h}, p.accent2.withAlpha(0.35f));
  }
  const float cx = xOf(es_.cursor);
  if (cx >= L.x && cx <= L.right()) c.fillRect(Rect{cx, L.y, 1, L.h}, p.cursor.withAlpha(0.85f));
  const auto ts = ctx_.transport();
  const float px = xOf(ts.position);
  const bool rec = (ts.flags & SQ_TRANSPORT_RECORDING) != 0;
  if (px >= L.x && px <= L.right()) c.fillRect(Rect{px - 0.5f, L.y, 1.5f, L.h}, rec ? p.rec : p.playhead);
  // hover position line in the lanes (pointer tool, empty space)
  if (drag_ == Drag::None && hover_.kind == HitKind::LaneEmpty && mouseX_ >= L.x && tool_ != Tool::Pointer) {
    const float hx = xOf(snap(ppqAt(mouseX_), 0));
    c.fillRect(Rect{hx, L.y, 1, L.h}, p.text.withAlpha(0.18f));
  }
}

// ---- drag preview (clip move ghosts) ----------------------------------------------------------------
void ArrangeView::paintDragPreview(Canvas& c) {
  if (drag_ != Drag::ClipMove || !moved_) return;
  const Palette& p = ctx_.palette();
  const Rect L = lanesRect();
  c.pushClip(L);
  for (size_t i = 0; i < lanes_.size(); ++i) {
    for (const auto& ce : lanes_[i].clips) {
      if (!selected(ce.v.id)) continue;
      int tgt = std::clamp(static_cast<int>(i) + dragLaneDelta_, 0, static_cast<int>(lanes_.size()) - 1);
      const Lane& tl = lanes_[static_cast<size_t>(tgt)];
      const bool fits = (ce.v.info.kind == SQ_CLIP_AUDIO && tl.audio()) || (ce.v.info.kind == SQ_CLIP_MIDI && tl.instrument());
      if (!fits) tgt = static_cast<int>(i);
      const Lane& dl = lanes_[static_cast<size_t>(tgt)];
      ClipEntry tmp = ce;
      tmp.v.info.start = std::max(0.0, ce.v.info.start + dragDelta_);
      const Rect r = clipRect(dl, tmp);
      paintClip(c, dl, tmp, r, true, false);
    }
  }
  // time readout at the anchor
  const ClipEntry* anchor = findClip(dragNode_);
  if (anchor) {
    const double ns = std::max(0.0, anchor->v.info.start + dragDelta_);
    std::string s = ctx_.formatBars(ns);
    if (dragCopy_) s = "Copy  " + s;
    const float tw = c.measureText(s, 11) + 12;
    const float x = std::clamp(mouseX_ + 12, L.x, L.right() - tw);
    const float y = std::clamp(mouseY_ - 26, L.y + 2, L.bottom() - 20);
    c.fillRoundRect(Rect{x, y, tw, 18}, 4, p.panel3);
    c.drawText(s, Rect{x + 6, y, tw - 8, 18}, p.text, 11);
  }
  c.popClip();
}

// ---- scrollbars -------------------------------------------------------------------------------------
void ArrangeView::paintScrollbars(Canvas& c) {
  const Palette& p = ctx_.palette();
  const Rect vb = vbarRect(), hb = hbarRect(), L = lanesRect();
  c.fillRect(vb, p.panel);
  c.fillRect(hb, p.panel);
  c.fillRect(Rect{frame().x, hb.y, headerW_, hb.h}, p.panel);
  c.fillRect(Rect{vb.x, hb.y, vb.w, hb.h}, p.panel);
  // vertical
  const float totalH = std::max(contentH_ + 80.0f, L.h);
  if (totalH > L.h + 1) {
    const float th = std::max(24.0f, vb.h * L.h / totalH);
    const float ty = vb.y + (vb.h - th) * (scrollY_ / std::max(1.0f, totalH - L.h));
    const bool hot = hover_.kind == HitKind::VScrollbar || drag_ == Drag::VScroll;
    c.fillRoundRect(Rect{vb.x + 2, ty + 1, vb.w - 4, th - 2}, 3, hot ? draw::lighten(p.panel3, 0.12f) : p.panel3);
  }
  // horizontal
  const double total = contentEnd();
  const double span = visibleSpan();
  if (total > 0) {
    const float tw = std::max(30.0f, hb.w * static_cast<float>(span / total));
    const float tx = hb.x + (hb.w - tw) * static_cast<float>(scroll_ / std::max(1e-9, total - span));
    const bool hot = hover_.kind == HitKind::HScrollbar || drag_ == Drag::HScroll;
    c.fillRoundRect(Rect{tx, hb.y + 2, tw, hb.h - 4}, 3, hot ? draw::lighten(p.panel3, 0.12f) : p.panel3);
  }
  // zoom readout under the headers
  char buf[48];
  const double barPx = barLength(scroll_) * ppx_;
  std::snprintf(buf, sizeof buf, "%d tracks   %.0f px/bar", static_cast<int>(lanes_.size()), barPx);
  c.drawText(buf, Rect{frame().x + 10, hb.y - 1, headerW_ - 14, hb.h + 2}, p.faint, 9.5f);
  c.fillRect(Rect{frame().x, hb.y, frame().w, 1}, p.border);
}

}  // namespace sq::app::gui::arrange
