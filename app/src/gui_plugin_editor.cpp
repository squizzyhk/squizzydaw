// Squizzy Studio GUI - native plugin editor windows (gui_plugin_editor.hpp): Win32 implementation
// and the stub for the other platforms.
//
// Window protocol (Windows): one WNDCLASS "SquizzyPluginEditorWindow"; every editor is a top-level
// window owned by the main window (WS_OVERLAPPEDWINDOW without maximize). The plugin's view is a
// child of it (IPluginInstance::openEditor(hwnd, "win32")), so keyboard input reaches the plugin
// once its child has the focus. Sizes: the plugin reports its editor size; the frame is sized so
// the CLIENT area matches. A user resize is offered to the plugin (setEditorSize) and a plugin that
// answers SQ_E_UNSUPPORTED gets a fixed frame; a plugin-driven resize (onEditorResize) resizes the
// frame without echoing back. WM_DESTROY never posts WM_QUIT: only the main window ends the loop.
#include "gui_plugin_editor.hpp"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "sq/engine/audio/engine.hpp"
#include "sq/platform.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace sq::app::gui {

#ifdef _WIN32

namespace {

const wchar_t* kEditorClass = L"SquizzyPluginEditorWindow";
constexpr uint32_t kDefaultW = 400, kDefaultH = 300;  // when a plugin reports a 0 size
constexpr int kCascade = 60;                           // new windows step down-right by this much

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

// Plugin name for titles and messages ("the plugin" when the adapter reports none).
std::string pluginName(IPluginInstance* inst) {
  sq_plugin_desc d{};
  d.struct_size = sizeof d;
  if (inst && inst->getDesc(&d) == SQ_OK && d.name[0]) {
    d.name[sizeof d.name - 1] = 0;
    return d.name;
  }
  return "the plugin";
}

}  // namespace

struct PluginEditorHost::Impl {
  struct Editor {
    Impl* host = nullptr;
    HWND hwnd = nullptr;
    sq::Ptr<IPluginInstance> inst;
    sq_guid node{};
    int32_t slot = 0;
    std::string nodeTitle;                  // "<track name>" part of the window title
    uint32_t w = kDefaultW, h = kDefaultH;  // client size the plugin accepted last
    bool resizable = true;                  // WS_THICKFRAME present
    bool hostResize = false;                // inside a host-driven SetWindowPos: WM_SIZE must not reach the plugin
    bool dead = false;                      // WM_DESTROY seen; purged by the next pump
  };

  GuiContext& ctx;
  Shell& shell;
  std::vector<std::unique_ptr<Editor>> editors;  // stable addresses: GWLP_USERDATA points at them
  uint64_t lastCompile = 0, lastSerial = 0;
  bool classRegistered = false;

  Impl(GuiContext& c, Shell& s) : ctx(c), shell(s) {}

  static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* ed = reinterpret_cast<Editor*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!ed) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
      case WM_CLOSE:
        ed->host->closeFromUser(*ed);
        return 0;
      case WM_DESTROY:
        // No PostQuitMessage here: that is the main window's job.
        ed->dead = true;
        ed->hwnd = nullptr;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
      case WM_SIZE:
        if (wp != SIZE_MINIMIZED) ed->host->userResized(*ed, static_cast<uint32_t>(LOWORD(lp)), static_cast<uint32_t>(HIWORD(lp)));
        return 0;
      case WM_ACTIVATE:
        // Keyboard goes to the plugin's view (our only child), not to the empty frame.
        if (LOWORD(wp) != WA_INACTIVE)
          if (HWND child = GetWindow(h, GW_CHILD)) SetFocus(child);
        break;
      default: break;  // WM_DPICHANGED and everything else: DefWindowProc
    }
    return DefWindowProcW(h, msg, wp, lp);
  }

  bool registerClass() {
    if (classRegistered) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &Impl::wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH));
    wc.lpszClassName = kEditorClass;
    // The class is process-wide: a second host (or a re-run) finds it registered already.
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    classRegistered = true;
    return true;
  }

  Editor* findLive(const sq_guid& node, int32_t slot) {
    for (auto& e : editors)
      if (!e->dead && e->hwnd && e->node == node && e->slot == slot) return e.get();
    return nullptr;
  }
  Editor* findInstance(IPluginInstance* inst) {
    if (!inst) return nullptr;
    for (auto& e : editors)
      if (!e->dead && e->hwnd && e->inst.get() == inst) return e.get();
    return nullptr;
  }
  size_t liveCount() const {
    size_t n = 0;
    for (const auto& e : editors)
      if (!e->dead && e->hwnd) ++n;
    return n;
  }
  void purge() {
    editors.erase(std::remove_if(editors.begin(), editors.end(), [](const std::unique_ptr<Editor>& e) { return e->dead || !e->hwnd; }),
                  editors.end());
  }

  // Frame size (pixels) whose client area is w x h at the window's DPI.
  static SIZE frameFor(HWND hwnd, uint32_t w, uint32_t h) {
    RECT rc{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD ex = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    AdjustWindowRectExForDpi(&rc, style, FALSE, ex, GetDpiForWindow(hwnd));
    return SIZE{rc.right - rc.left, rc.bottom - rc.top};
  }

  // Resizes the frame (in place) so the client area is w x h. Host-driven: WM_SIZE is swallowed.
  void fitClient(Editor& ed, uint32_t w, uint32_t h) {
    if (!ed.hwnd) return;
    const SIZE sz = frameFor(ed.hwnd, w, h);
    const bool was = ed.hostResize;
    ed.hostResize = true;
    SetWindowPos(ed.hwnd, nullptr, 0, 0, sz.cx, sz.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ed.hostResize = was;
  }

  // First placement: cascaded from the owner's top-left (kCascade per open editor, one step in
  // from the corner so the first one does not cover the owner's menu bar), kept on the work area
  // of the owner's monitor, client area ed.w x ed.h.
  void place(Editor& ed, HWND owner, size_t index) {
    if (!ed.hwnd) return;
    const SIZE sz = frameFor(ed.hwnd, ed.w, ed.h);
    RECT own{0, 0, 0, 0};
    if (!owner || !GetWindowRect(owner, &own)) own = RECT{0, 0, 0, 0};
    const int off = kCascade * static_cast<int>((index % 10) + 1);
    int x = own.left + off, y = own.top + off;
    RECT work{0, 0, 1920, 1080};
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (HMONITOR mon = MonitorFromWindow(owner ? owner : ed.hwnd, MONITOR_DEFAULTTONEAREST); mon && GetMonitorInfoW(mon, &mi)) work = mi.rcWork;
    x = std::max<int>(work.left, std::min<int>(x, work.right - sz.cx));
    y = std::max<int>(work.top, std::min<int>(y, work.bottom - sz.cy));
    const bool was = ed.hostResize;
    ed.hostResize = true;
    SetWindowPos(ed.hwnd, nullptr, x, y, sz.cx, sz.cy, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ed.hostResize = was;
  }

  // Adds / removes the sizing border; the client area keeps the plugin's size.
  void setResizable(Editor& ed, bool on) {
    if (!ed.hwnd || ed.resizable == on) return;
    ed.resizable = on;
    const LONG_PTR style = GetWindowLongPtrW(ed.hwnd, GWL_STYLE);
    const LONG_PTR thick = static_cast<LONG_PTR>(WS_THICKFRAME);
    SetWindowLongPtrW(ed.hwnd, GWL_STYLE, on ? (style | thick) : (style & ~thick));
    fitClient(ed, ed.w, ed.h);
  }

  // WM_SIZE: a resize by the user (or by Windows) is offered to the plugin.
  void userResized(Editor& ed, uint32_t w, uint32_t h) {
    if (ed.hostResize || ed.dead || !ed.inst || !w || !h) return;
    if (w == ed.w && h == ed.h) return;
    const sq_result r = ed.inst->setEditorSize(w, h);
    if (r == SQ_E_UNSUPPORTED) {
      setResizable(ed, false);  // snaps the frame back to the plugin's size
      return;
    }
    ed.w = w;
    ed.h = h;
  }

  void setTitle(Editor& ed) {
    if (ed.hwnd) SetWindowTextW(ed.hwnd, widen(ed.nodeTitle + " - " + pluginName(ed.inst.get())).c_str());
  }

  void destroy(Editor& ed) {
    if (ed.hwnd) DestroyWindow(ed.hwnd);  // WM_DESTROY marks it dead
    ed.dead = true;
    ed.hwnd = nullptr;
  }

  // WM_CLOSE: close the plugin's view, drop the window, and keep the plugin's state in the song.
  void closeFromUser(Editor& ed) {
    const sq_guid node = ed.node;
    const int32_t slot = ed.slot;
    const bool had = static_cast<bool>(ed.inst);
    if (ed.inst) {
      ed.inst->closeEditor();
      ed.inst = nullptr;
    }
    destroy(ed);
    if (had) shell.capturePluginStates(&node, slot);
  }
};

PluginEditorHost::PluginEditorHost(GuiContext& ctx, Shell& shell) : impl_(std::make_unique<Impl>(ctx, shell)) {}

PluginEditorHost::~PluginEditorHost() { closeAll(); }

bool PluginEditorHost::open(const sq_guid& node, int32_t slot, const std::string& nodeTitle, void* ownerHwnd) {
  Impl& im = *impl_;
  im.purge();
  engine::audio::AudioEngine* eng = im.shell.audio();
  if (!eng) {
    im.ctx.setStatus("plugin editors need the audio engine");
    return false;
  }
  if (Impl::Editor* e = im.findLive(node, slot)) {
    if (IsIconic(e->hwnd)) ShowWindow(e->hwnd, SW_RESTORE);
    SetForegroundWindow(e->hwnd);
    return true;
  }
  sq::Ptr<IPluginInstance> inst = eng->pluginInstance(node, slot);
  if (!inst) {
    im.ctx.setStatus("that insert is not running (bypassed, unavailable or a built-in device)");
    return false;
  }
  if (!inst->hasEditor()) {
    im.ctx.setStatus(pluginName(inst.get()) + " has no editor window");
    return false;
  }
  if (!im.registerClass()) {
    im.ctx.setStatus("could not register the plugin editor window class");
    return false;
  }
  auto ed = std::make_unique<Impl::Editor>();
  ed->host = &im;
  ed->node = node;
  ed->slot = slot;
  ed->nodeTitle = nodeTitle;
  ed->hostResize = true;  // nothing reaches the plugin until the frame is placed
  HWND owner = static_cast<HWND>(ownerHwnd);
  const std::string title = nodeTitle + " - " + pluginName(inst.get());
  const DWORD style = WS_OVERLAPPEDWINDOW & ~static_cast<DWORD>(WS_MAXIMIZEBOX);
  // Created near the owner so the DPI the plugin sees is the owner's monitor's; place() refines.
  int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
  RECT own{};
  if (owner && GetWindowRect(owner, &own)) {
    x = own.left + kCascade;
    y = own.top + kCascade;
  }
  HWND hwnd = CreateWindowExW(0, kEditorClass, widen(title).c_str(), style, x, y, static_cast<int>(kDefaultW), static_cast<int>(kDefaultH),
                              owner, nullptr, GetModuleHandleW(nullptr), ed.get());
  if (!hwnd) {
    im.ctx.setStatus("could not create a window for " + pluginName(inst.get()));
    return false;
  }
  ed->hwnd = hwnd;
  uint32_t w = 0, h = 0;
  const sq_result r = inst->openEditor(hwnd, "win32", &w, &h);
  if (SQ_FAILED(r)) {
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    im.ctx.setStatus("could not open the editor of " + pluginName(inst.get()) + ": " + sq_result_string(r));
    return false;
  }
  ed->inst = inst;
  ed->w = w ? w : kDefaultW;
  ed->h = h ? h : kDefaultH;
  im.place(*ed, owner, im.liveCount());
  ShowWindow(hwnd, SW_SHOWNORMAL);
  ed->hostResize = false;
  if (im.editors.empty()) {
    im.lastCompile = eng->compileCount();
    im.lastSerial = im.ctx.docSerial();
  }
  im.editors.push_back(std::move(ed));
  im.ctx.setStatus(title);
  return true;
}

void PluginEditorHost::closeAll() {
  Impl& im = *impl_;
  for (auto& e : im.editors) {
    if (e->inst) {
      e->inst->closeEditor();
      e->inst = nullptr;
    }
    im.destroy(*e);
  }
  im.editors.clear();
}

void PluginEditorHost::idle() {
  Impl& im = *impl_;
  im.purge();
  if (im.editors.empty()) return;
  engine::audio::AudioEngine* eng = im.shell.audio();
  if (!eng) {
    closeAll();
    return;
  }
  const uint64_t cc = eng->compileCount(), ds = im.ctx.docSerial();
  if (cc == im.lastCompile && ds == im.lastSerial) return;
  im.lastCompile = cc;
  im.lastSerial = ds;
  for (auto& e : im.editors) {
    Impl::Editor& ed = *e;
    if (ed.dead || !ed.hwnd) continue;
    sq::Ptr<IPluginInstance> cur = eng->pluginInstance(ed.node, ed.slot);
    if (cur.get() == ed.inst.get()) continue;
    // The graph was recompiled (new instance behind the insert) or the insert is gone.
    if (ed.inst) {
      ed.inst->closeEditor();
      ed.inst = nullptr;
    }
    if (cur && cur->hasEditor()) {
      uint32_t w = 0, h = 0;
      ed.hostResize = true;
      const sq_result r = cur->openEditor(ed.hwnd, "win32", &w, &h);
      if (SQ_SUCCEEDED(r)) {
        ed.inst = cur;
        im.setResizable(ed, true);  // the new instance decides again whether it can be resized
        if (w && h && (w != ed.w || h != ed.h)) {
          ed.w = w;
          ed.h = h;
          im.fitClient(ed, w, h);
        }
        im.setTitle(ed);
        ed.hostResize = false;
        continue;
      }
      ed.hostResize = false;
    }
    im.destroy(ed);
  }
  im.purge();
}

bool PluginEditorHost::onEditorResize(IPluginInstance* inst, uint32_t w, uint32_t h) {
  Impl& im = *impl_;
  Impl::Editor* ed = im.findInstance(inst);
  if (!ed || !w || !h) return false;
  ed->w = w;
  ed->h = h;
  im.fitClient(*ed, w, h);
  return true;
}

size_t PluginEditorHost::count() const { return impl_->liveCount(); }

#else  // !_WIN32

struct PluginEditorHost::Impl {
  GuiContext& ctx;
  Shell& shell;
  Impl(GuiContext& c, Shell& s) : ctx(c), shell(s) {}
};

PluginEditorHost::PluginEditorHost(GuiContext& ctx, Shell& shell) : impl_(std::make_unique<Impl>(ctx, shell)) {}
PluginEditorHost::~PluginEditorHost() = default;

bool PluginEditorHost::open(const sq_guid& node, int32_t slot, const std::string& nodeTitle, void* ownerHwnd) {
  (void)node;
  (void)slot;
  (void)nodeTitle;
  (void)ownerHwnd;
  impl_->ctx.setStatus("Plugin editor windows are only available on Windows yet");
  return false;
}
void PluginEditorHost::closeAll() {}
void PluginEditorHost::idle() {}
bool PluginEditorHost::onEditorResize(IPluginInstance* inst, uint32_t w, uint32_t h) {
  (void)inst;
  (void)w;
  (void)h;
  return false;
}
size_t PluginEditorHost::count() const { return 0; }

#endif  // _WIN32

}  // namespace sq::app::gui
