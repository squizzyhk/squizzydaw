// Squizzy Studio GUI - mixer panel (stub: one strip per track with name, volume and meter).
#include <cmath>

#include "gui_panels.hpp"

namespace sq::app::gui {
namespace {
class Mixer final : public Widget {
 public:
  explicit Mixer(GuiContext& ctx) : Widget("Mixer"), ctx_(ctx) {}
  void paint(Canvas& c) override {
    const Palette& p = ctx_.palette();
    const Rect& f = frame();
    c.fillRect(f, p.bg);
    float x = f.x + 8;
    for (const auto& t : ctx_.tracks()) {
      Rect s{x, f.y + 8, 84, f.h - 16};
      c.fillRoundRect(s, 4, p.panel2);
      c.fillRect(Rect{s.x, s.y, s.w, 3}, ctx_.trackColor(t));
      c.drawText(t.name, Rect{s.x + 4, s.y + 6, s.w - 8, 18}, p.text, 11.5f, TextAlign::Center);
      char buf[32];
      std::snprintf(buf, sizeof buf, "%.1f dB", t.info.volume_db);
      c.drawText(buf, Rect{s.x, s.bottom() - 22, s.w, 18}, p.dim, 11, TextAlign::Center);
      float pk[2] = {0, 0};
      ctx_.meter(t.id, pk, nullptr, 2);
      draw::meter(c, p, Rect{s.x + s.w / 2 - 6, s.y + 30, 5, s.h - 60}, pk[0], 0);
      draw::meter(c, p, Rect{s.x + s.w / 2 + 1, s.y + 30, 5, s.h - 60}, pk[1], 0);
      x += 90;
    }
  }

 private:
  GuiContext& ctx_;
};
}  // namespace
std::unique_ptr<Widget> makeMixer(GuiContext& ctx) { return std::make_unique<Mixer>(ctx); }
}  // namespace sq::app::gui
