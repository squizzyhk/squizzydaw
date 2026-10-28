// No windowing backend (non-Windows without X11; macOS has no native window yet).
#if !defined(_WIN32) && !defined(SQ_UI_X11)
#include "sq/ui/ui.hpp"

namespace sq::ui {

std::unique_ptr<IWindow> createWindow(const WindowDesc&) { return nullptr; }
const char* defaultBackendName() { return "none"; }

}  // namespace sq::ui
#endif
