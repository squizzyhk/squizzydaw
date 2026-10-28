// Squizzy Studio GUI - device rack (stub; instrument + insert chain panel built by its owner).
#include "gui_panels.hpp"

namespace sq::app::gui {
namespace {
class DeviceRack final : public Widget {
 public:
  explicit DeviceRack(GuiContext& ctx) : Widget("DeviceRack"), ctx_(ctx) {}
  void paint(Canvas& c) override {
    c.fillRect(frame(), ctx_.palette().bg);
    c.drawText("Devices of the selected track", Rect{frame().x + 16, frame().y + 12, frame().w - 32, 20}, ctx_.palette().dim, 13);
  }

 private:
  GuiContext& ctx_;
};
}  // namespace
std::unique_ptr<Widget> makeDeviceRack(GuiContext& ctx) { return std::make_unique<DeviceRack>(ctx); }
}  // namespace sq::app::gui
