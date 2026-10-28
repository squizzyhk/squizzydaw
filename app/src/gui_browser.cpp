// Squizzy Studio GUI - browser panel (stub; the full browser is built by its owner).
#include "gui_panels.hpp"

namespace sq::app::gui {
namespace {
class Browser final : public Widget {
 public:
  explicit Browser(GuiContext& ctx) : Widget("Browser"), ctx_(ctx) {}
  void paint(Canvas& c) override {
    const Palette& p = ctx_.palette();
    c.fillRect(frame(), p.panel);
    c.drawTextBold("Browser", Rect{frame().x + 10, frame().y + 6, frame().w - 20, 20}, p.text, 13);
  }

 private:
  GuiContext& ctx_;
};
}  // namespace
std::unique_ptr<Widget> makeBrowser(GuiContext& ctx) { return std::make_unique<Browser>(ctx); }
}  // namespace sq::app::gui
