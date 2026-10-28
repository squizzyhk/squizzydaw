// Squizzy Studio GUI - browser "Devices" tab: built-in instruments and effects.
// (Filled from the engine's built-in device catalog once it is part of the build.)
#include "gui_panels.hpp"

namespace sq::app::gui {

struct DeviceEntry {
  std::string category, id, name;
  bool instrument = false;
};

std::vector<DeviceEntry> browserDeviceEntries() { return {}; }

void browserUseDevice(GuiContext& ctx, const DeviceEntry& d) { ctx.setStatus("cannot add " + d.name); }

std::vector<MenuItem> buildInsertMenu(GuiContext& ctx, const sq_guid& track) {
  (void)ctx, (void)track;
  return {};
}
std::vector<MenuItem> buildInstrumentMenu(GuiContext& ctx, const sq_guid& track) {
  (void)ctx, (void)track;
  return {};
}

}  // namespace sq::app::gui
