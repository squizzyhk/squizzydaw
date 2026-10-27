// Non-Windows platforms have no windowing backend in M0 (Skia + native window is planned).
#ifndef _WIN32
#include "sq/ui/ui.hpp"

namespace sq::ui {

std::unique_ptr<IWindow> createWindow(const WindowDesc&) { return nullptr; }
const char* defaultBackendName() { return "none"; }

}  // namespace sq::ui
#endif
