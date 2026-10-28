// Squizzy Studio GUI - panel factories. Each panel lives in its own file and is placed by the
// main skin form (app/resources/default.skin.xml) under the element name in brackets.
#pragma once

#include <memory>
#include <string>

#include "gui_context.hpp"

namespace sq::app::gui {

std::unique_ptr<Widget> makeMenuBar(GuiContext& ctx);       // <MenuBar>      gui_menubar.cpp
std::unique_ptr<Widget> makeTransportBar(GuiContext& ctx);  // <TransportBar> gui_transport.cpp
std::unique_ptr<Widget> makeBrowser(GuiContext& ctx);       // <Browser>      gui_browser.cpp
std::unique_ptr<Widget> makeArrange(GuiContext& ctx);       // <Arrange>      gui_arrange.cpp
std::unique_ptr<Widget> makeClipEditor(GuiContext& ctx);    // <ClipEditor>   gui_editor.cpp (piano roll)
std::unique_ptr<Widget> makeMixer(GuiContext& ctx);         // <Mixer>        gui_mixer.cpp
std::unique_ptr<Widget> makeDeviceRack(GuiContext& ctx);    // <DeviceRack>   gui_devices.cpp

// Menu entries shared by the menu bar and context menus (gui_menubar.cpp).
std::vector<MenuItem> buildMainMenu(GuiContext& ctx, const std::string& title);
// Global keyboard shortcuts not consumed by a focused panel (gui_menubar.cpp). True if handled.
bool handleShortcutKey(GuiContext& ctx, Key k, unsigned mods);
bool handleShortcutChar(GuiContext& ctx, uint32_t ch, unsigned mods);

// File drop onto the arrange view (gui_arrange.cpp), called by the browser on mouse up over the
// arrange widget (root->findByName("arrange")). Audio files become a clip at the snapped position on
// the audio track under the point (or a new audio track); .mid files are imported as a MIDI clip.
bool arrangeDropFile(Widget* arrange, float x, float y, const std::string& path);  // x,y in DIPs (tree coords); true if consumed
// Optional hover feedback while a browser file is dragged over the arrange (x,y in DIPs).
void arrangeDragOver(Widget* arrange, float x, float y);

}  // namespace sq::app::gui
