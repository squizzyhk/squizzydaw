// Squizzy Studio GUI - clip editor panel (stub; the piano roll is built by its owner).
#include "gui_panels.hpp"

namespace sq::app::gui {
namespace {
class ClipEditor final : public Widget {
 public:
  explicit ClipEditor(GuiContext& ctx) : Widget("ClipEditor"), ctx_(ctx) {}
  void paint(Canvas& c) override {
    c.fillRect(frame(), ctx_.palette().bg);
    c.drawText("Double-click a MIDI clip to edit it", Rect{frame().x + 16, frame().y + 12, frame().w - 32, 20}, ctx_.palette().dim, 13);
  }

 private:
  GuiContext& ctx_;
};
}  // namespace
std::unique_ptr<Widget> makeClipEditor(GuiContext& ctx) { return std::make_unique<ClipEditor>(ctx); }
}  // namespace sq::app::gui
