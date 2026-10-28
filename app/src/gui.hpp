// Squizzy Studio GUI entry (window + skin-driven panels over the Shell).
#pragma once

#include <string>

#include "sq/app/shell.hpp"

namespace sq::app {
// Runs the window loop until it is closed (or quitAfterMs elapsed, >= 0). Returns an exit code.
int runGui(Shell& shell, int quitAfterMs);

// Headless GUI: builds the same window contents offscreen (software renderer), replays a UI
// script and/or writes a PNG screenshot. No display needed.
//   script: file path or inline text; one op per line or ';'-separated:
//     click X Y [ctrl|shift|alt]   rclick X Y   dclick X Y   move X Y   down X Y   up X Y
//     drag X1 Y1 X2 Y2 [mods]      wheel X Y NOTCHES [mods]   key NAME [mods]   char C [mods]
//     type TEXT   run Cat.Name k=v ...   idle MS   shot FILE.png   print
//   coordinates in DIPs (window pixels at scale 1).
struct GuiRunOptions {
  std::string screenshot;  // final screenshot (optional)
  std::string script;
  int width = 1600, height = 1000;
  float scale = 1.0f;
};
int runGuiOffscreen(Shell& shell, const GuiRunOptions& opt);
}  // namespace sq::app
