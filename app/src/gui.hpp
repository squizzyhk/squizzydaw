// Squizzy Studio GUI entry (window + skin-driven arrange/mixer over the Shell).
#pragma once

#include "sq/app/shell.hpp"

namespace sq::app {
// Runs the window loop until it is closed (or quitAfterMs elapsed, >= 0). Returns an exit code.
int runGui(Shell& shell, int quitAfterMs);
}  // namespace sq::app
