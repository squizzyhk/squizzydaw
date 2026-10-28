// Squizzy Studio GUI - transport bar (stub: buttons + position; the full bar is built by its owner).
#include "gui_panels.hpp"

namespace sq::app::gui {
namespace {

class TransportBar final : public Widget {
 public:
  explicit TransportBar(GuiContext& ctx) : Widget("TransportBar"), ctx_(ctx) {}
  void paint(Canvas& c) override {
    const Palette& p = ctx_.palette();
    const Rect& f = frame();
    c.fillRect(f, p.panel);
    c.fillRect(Rect{f.x, f.bottom() - 1, f.w, 1}, p.border);
    const auto ts = ctx_.transport();
    struct B { const char* label; const char* cmd; bool on; Color col; };
    const B bs[] = {{"|<", "Transport.Rewind", false, p.accent},
                    {"Play", "Transport.Play", (ts.flags & SQ_TRANSPORT_PLAYING) != 0, p.accent},
                    {"Stop", "Transport.Stop", false, p.accent},
                    {"Rec", "Transport.Record", (ts.flags & SQ_TRANSPORT_RECORDING) != 0, p.rec},
                    {"Loop", "Transport.ToggleLoop", (ts.flags & SQ_TRANSPORT_LOOPING) != 0, p.accent},
                    {"Click", "Transport.ToggleMetronome", (ts.flags & SQ_TRANSPORT_METRONOME) != 0, p.accent}};
    float x = f.x + 10;
    rects_.clear();
    cmds_.clear();
    for (const auto& b : bs) {
      Rect r{x, f.y + 8, 52, f.h - 16};
      draw::button(c, p, r, b.label, false, b.on, b.col);
      rects_.push_back(r);
      cmds_.push_back(b.cmd);
      x += 56;
    }
    c.drawTextBold(ctx_.formatBars(ts.position), Rect{x + 20, f.y, 140, f.h}, p.accent, 18);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f BPM   %u/%u", ts.bpm, ts.numerator ? ts.numerator : 4, ts.denominator ? ts.denominator : 4);
    c.drawText(buf, Rect{x + 170, f.y, 200, f.h}, p.text, 13);
  }
  bool onMouse(const WidgetMouse& e) override {
    if (e.action != MouseAction::Down || e.button != MouseButton::Left) return false;
    for (size_t i = 0; i < rects_.size(); ++i)
      if (rects_[i].contains(e.x, e.y)) {
        ctx_.run(cmds_[i]);
        return true;
      }
    return false;
  }

 private:
  GuiContext& ctx_;
  std::vector<Rect> rects_;
  std::vector<std::string> cmds_;
};

}  // namespace

std::unique_ptr<Widget> makeTransportBar(GuiContext& ctx) { return std::make_unique<TransportBar>(ctx); }

}  // namespace sq::app::gui
