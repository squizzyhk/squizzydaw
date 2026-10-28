// Squizzy Studio GUI - arrange view (stub port of the original: lanes, clips, playhead, cursor).
#include <algorithm>
#include <cmath>

#include "gui_panels.hpp"

namespace sq::app::gui {
namespace {

class ArrangeView final : public Widget {
 public:
  explicit ArrangeView(GuiContext& ctx) : Widget("Arrange"), ctx_(ctx) { hitTestable = true; }
  double pxPerBeat = 24;
  float laneH = 56, headerW = 180, rulerH = 24;

  void paint(Canvas& c) override {
    const Palette& p = ctx_.palette();
    const Rect& f = frame();
    c.fillRect(f, p.trackBg);
    c.pushClip(f);
    c.fillRect(Rect{f.x, f.y, f.w, rulerH}, p.panel);
    for (int bar = 0; bar < 1024; ++bar) {
      float x = f.x + headerW + static_cast<float>(bar * 4 * pxPerBeat);
      if (x > f.right()) break;
      c.drawLine(Point{x, f.y + rulerH}, Point{x, f.bottom()}, p.gridBar, 1);
      c.drawText(std::to_string(bar + 1), Rect{x + 3, f.y + 2, 40, rulerH - 4}, p.dim, 11);
    }
    const auto es = ctx_.editState();
    auto tracks = ctx_.tracks();
    for (size_t i = 0; i < tracks.size(); ++i) {
      const auto& t = tracks[i];
      float y = f.y + rulerH + static_cast<float>(i) * laneH;
      if (y > f.bottom()) break;
      const bool selT = t.id == es.track;
      c.fillRect(Rect{f.x, y, headerW - 1, laneH - 1}, selT ? p.trackSel : p.panel2);
      const Color col = ctx_.trackColor(t);
      c.fillRect(Rect{f.x, y, 4, laneH - 1}, col);
      c.drawText(t.name, Rect{f.x + 12, y + 6, headerW - 20, 18}, p.text, 12.5f);
      c.fillRect(Rect{f.x + headerW, y + laneH - 1, f.w, 1}, p.border);
      for (const auto& cl : ctx_.clips(t)) {
        Rect r{f.x + headerW + static_cast<float>(cl.info.start * pxPerBeat), y + 3, static_cast<float>(cl.info.length * pxPerBeat), laneH - 7};
        const bool sel = std::find(es.selection.begin(), es.selection.end(), cl.id) != es.selection.end();
        c.fillRoundRect(r, 4, col.withAlpha((cl.info.flags & SQ_CLIP_MUTED) ? 0.25f : 0.75f));
        if (sel) c.strokeRoundRect(r, 4, p.text, 2);
        c.drawText(cl.name, Rect{r.x + 5, r.y + 2, r.w - 10, 16}, Color::rgb(0x101114), 11);
      }
    }
    const float cx = f.x + headerW + static_cast<float>(es.cursor * pxPerBeat);
    c.drawLine(Point{cx, f.y}, Point{cx, f.bottom()}, p.cursor, 1);
    const auto ts = ctx_.transport();
    const float px = f.x + headerW + static_cast<float>(ts.position * pxPerBeat);
    c.drawLine(Point{px, f.y}, Point{px, f.bottom()}, (ts.flags & SQ_TRANSPORT_RECORDING) ? p.rec : p.playhead, 1.5f);
    c.popClip();
  }

  bool onMouse(const WidgetMouse& e) override {
    const Rect& f = frame();
    if (e.action == MouseAction::Wheel) {
      pxPerBeat = std::clamp(pxPerBeat * (e.wheel > 0 ? 1.2 : 1 / 1.2), 2.0, 400.0);
      ctx_.invalidate();
      return true;
    }
    if (e.action == MouseAction::DoubleClick) {
      for (const auto& id : ctx_.editState().selection) {
        ctx_.openClipEditor(id);
        return true;
      }
      return true;
    }
    if (e.action != MouseAction::Down || e.button != MouseButton::Left) return false;
    const double ppq = std::max(0.0, (e.x - f.x - headerW) / pxPerBeat);
    const int lane = static_cast<int>((e.y - f.y - rulerH) / laneH);
    auto tracks = ctx_.tracks();
    if (lane < 0 || lane >= static_cast<int>(tracks.size())) return true;
    const auto& t = tracks[static_cast<size_t>(lane)];
    ctx_.selectTrack(t.id);
    std::vector<sq_guid> sel;
    for (const auto& cl : ctx_.clips(t))
      if (ppq >= cl.info.start && ppq < cl.info.start + cl.info.length) sel.push_back(cl.id);
    ctx_.setSelection(sel);
    if (e.x > f.x + headerW) ctx_.setCursor(ppq);
    return true;
  }

 private:
  GuiContext& ctx_;
};

}  // namespace

std::unique_ptr<Widget> makeArrange(GuiContext& ctx) { return std::make_unique<ArrangeView>(ctx); }

}  // namespace sq::app::gui
