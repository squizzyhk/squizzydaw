// Squizzy Studio GUI - native plugin editor windows (gui_plugin_editor.hpp): Win32 implementation
// and the stub for the other platforms.
//
// Window protocol (Windows): one WNDCLASS "SquizzyPluginEditorWindow"; every editor is a top-level
// window owned by the main window (WS_OVERLAPPEDWINDOW without maximize). The plugin's view is a
// child of it (IPluginInstance::openEditor(hwnd, "win32")), so keyboard input reaches the plugin
// once its child has the focus. Sizes: the plugin reports its editor size; the frame is sized so
// the CLIENT area matches. A user resize is offered to the plugin (setEditorSize) and a plugin that
// answers SQ_E_UNSUPPORTED gets a fixed frame; what the plugin accepted (adapters constrain sizes)
// is read back from its view; a plugin-driven resize (onEditorResize) resizes the frame without
// echoing back. A DPI change keeps the client at the plugin's size at the suggested position.
// Teardown: WM_DESTROY closes the plugin's view (closeEditor) while that child still exists, which
// also covers the owner being destroyed first (quit); it never posts WM_QUIT: only the main window
// ends the loop.
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
constexpr uint64_t kKeepActiveMs = 2000;               // see holdActivation()

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

// The app icon (IDI_APP, app/resources/app.rc) at the system size the two metrics name; null in an
// executable without that resource (tests), which leaves Windows' default icon.
HICON appIcon(int cxMetric, int cyMetric) {
  return static_cast<HICON>(
      LoadImageW(GetModuleHandleW(nullptr), L"IDI_APP", IMAGE_ICON, GetSystemMetrics(cxMetric), GetSystemMetrics(cyMetric), 0));
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
    IPluginInstance* sizing = nullptr;      // the instance whose setEditorSize runs (userResized); see closeView()
    uint64_t attachedIn = 0;                // the idle() pass (attachPass) that re-created its view; 0 = none
    bool clickActivated = false;            // its last activation was a mouse click
  };

  // Raised around every plugin call made while an Editor is held. A plugin may run a message loop
  // inside openEditor / closeEditor / setEditorSize (a licence box, say), and that loop dispatches
  // our messages: the main window may close (closeAll) or the user may close an editor. While busy
  // no Editor is freed (purge() waits) and none is added (open() refuses), so a caller only has to
  // re-check ed.dead / ed.hwnd after each plugin call. No view is closed under a call still running
  // on it either: closeView() takes the instance out of ed first, and leaves a view inside
  // setEditorSize to userResized.
  struct Busy {
    Impl& im;
    explicit Busy(Impl& i) : im(i) { ++im.busy; }
    ~Busy() { --im.busy; }
    Busy(const Busy&) = delete;
    Busy& operator=(const Busy&) = delete;
  };

  GuiContext& ctx;
  Shell& shell;
  std::vector<std::unique_ptr<Editor>> editors;  // stable addresses: GWLP_USERDATA points at them
  uint64_t lastCompile = 0, lastSerial = 0;
  bool classRegistered = false;
  int busy = 0;               // Busy depth
  HWND keepActive = nullptr;  // holdActivation(): the window the user was in, until keepUntil
  uint64_t keepUntil = 0;
  uint64_t attachPass = 0;    // idle() passes that re-created at least one view

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
        // Children are destroyed after their parent's WM_DESTROY, so the plugin's view is still
        // alive here: close it now. Reached with inst already dropped from destroy() (closeFromUser,
        // closeAll), and with it still set when the owner (main window) is destroyed first. The
        // editor is marked gone first: messages pumped inside closeEditor no longer reach it.
        // No PostQuitMessage here: that is the main window's job.
        ed->dead = true;
        ed->hwnd = nullptr;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        ed->host->closeView(*ed);
        return 0;
      case WM_SIZE:
        if (wp != SIZE_MINIMIZED) ed->host->userResized(*ed, static_cast<uint32_t>(LOWORD(lp)), static_cast<uint32_t>(HIWORD(lp)));
        return 0;
      case WM_DPICHANGED:
        ed->host->dpiChanged(*ed, *reinterpret_cast<const RECT*>(lp));
        return 0;
      case WM_ACTIVATE:
        ed->clickActivated = LOWORD(wp) == WA_CLICKACTIVE;
        // Keyboard goes to the plugin's view (our only child), not to the empty frame.
        if (LOWORD(wp) != WA_INACTIVE)
          if (HWND child = GetWindow(h, GW_CHILD)) SetFocus(child);
        break;
      case WM_SETFOCUS:
        // Same when the focus lands on the frame itself (e.g. after the view was swapped).
        if (HWND child = GetWindow(h, GW_CHILD)) {
          SetFocus(child);
          return 0;
        }
        break;
      default: break;  // everything else: DefWindowProc
    }
    return DefWindowProcW(h, msg, wp, lp);
  }

  bool registerClass() {
    if (classRegistered) return true;
    // Loaded once per process, like the class they belong to.
    static const HICON bigIcon = appIcon(SM_CXICON, SM_CYICON), smallIcon = appIcon(SM_CXSMICON, SM_CYSMICON);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &Impl::wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = bigIcon;
    wc.hIconSm = smallIcon;
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
  Editor* findWindow(HWND hwnd) {
    if (!hwnd) return nullptr;
    for (auto& e : editors)
      if (!e->dead && e->hwnd == hwnd) return e.get();
    return nullptr;
  }
  size_t liveCount() const {
    size_t n = 0;
    for (const auto& e : editors)
      if (!e->dead && e->hwnd) ++n;
    return n;
  }
  void purge() {
    if (busy) return;  // an Editor may be held up the stack (see Busy): the next top-level pump frees it
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

  // WM_DPICHANGED: Windows suggests a frame rectangle for the new DPI. We take its position but keep
  // the client area at the plugin's size (ed.w x ed.h, measured with the new DPI's frame metrics);
  // the plugin's view scales itself. Host-driven: the WM_SIZE is swallowed.
  void dpiChanged(Editor& ed, const RECT& suggested) {
    if (!ed.hwnd) return;
    const SIZE sz = frameFor(ed.hwnd, ed.w, ed.h);
    const bool was = ed.hostResize;
    ed.hostResize = true;
    SetWindowPos(ed.hwnd, nullptr, suggested.left, suggested.top, sz.cx, sz.cy, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ed.hostResize = was;
  }

  // Size (pixels) of the plugin's view, our first child; false when there is none (or it is empty).
  static bool viewSize(HWND frame, uint32_t* w, uint32_t* h) {
    HWND child = frame ? GetWindow(frame, GW_CHILD) : nullptr;
    RECT rc{};
    if (!child || !GetWindowRect(child, &rc) || rc.right <= rc.left || rc.bottom <= rc.top) return false;
    *w = static_cast<uint32_t>(rc.right - rc.left);
    *h = static_cast<uint32_t>(rc.bottom - rc.top);
    return true;
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

  // WM_SIZE: a resize by the user (or by Windows) is offered to the plugin. The plugin may refuse,
  // may answer through onEditorResize while we are inside setEditorSize (ed.w/ed.h change under
  // us), or may accept a constrained size: IPluginInstance has no getEditorSize, so the size of its
  // view (our only child) is the best signal of what it took, and the frame is fitted to that.
  // ed.inst stays set during the call (onEditorResize finds the editor by it); a close that comes
  // in meanwhile only takes it out of ed (closeView), and the view is closed here once the plugin
  // has returned from setEditorSize, by the outermost call when such calls nest.
  void userResized(Editor& ed, uint32_t w, uint32_t h) {
    if (ed.hostResize || ed.dead || !ed.inst || !w || !h) return;
    if (w == ed.w && h == ed.h) return;
    Busy guard(*this);
    const uint32_t oldW = ed.w, oldH = ed.h;
    const sq::Ptr<IPluginInstance> inst = ed.inst;
    IPluginInstance* const outer = std::exchange(ed.sizing, inst.get());
    const sq_result r = inst->setEditorSize(w, h);
    ed.sizing = outer;
    if (ed.inst.get() != inst.get()) {  // closed (or swapped by idle) while the plugin ran
      if (outer != inst.get()) inst->closeEditor();
      return;
    }
    if (ed.dead || !ed.hwnd) return;
    if (r == SQ_E_UNSUPPORTED) {
      // Fixed-size plugin: drop the sizing border and snap back. fitClient runs every time: the
      // modal size loop keeps sending WM_SIZE after WS_THICKFRAME is gone (setResizable is a no-op
      // by then), and ed.w/ed.h is the plugin's size whether or not onEditorResize touched it.
      setResizable(ed, false);
      fitClient(ed, ed.w, ed.h);
      return;
    }
    if (SQ_FAILED(r)) {
      fitClient(ed, ed.w, ed.h);  // refused: back to the size it has (old, or onEditorResize's)
      return;
    }
    if (ed.w == oldW && ed.h == oldH) {  // onEditorResize did not already answer with a size
      ed.w = w;
      ed.h = h;
    }
    uint32_t vw = 0, vh = 0;
    if (viewSize(ed.hwnd, &vw, &vh) && (vw != ed.w || vh != ed.h)) {
      ed.w = vw;  // the adapter constrained the size (VST3 checkSizeConstraint, CLAP adjust_size)
      ed.h = vh;
    }
    if (ed.w != w || ed.h != h) fitClient(ed, ed.w, ed.h);
  }

  // Some plugins activate their own window a moment after attach (Surge XT does, from a timer).
  // For kKeepActiveMs after views were re-created, an activation of an editor re-attached by that
  // pass (attachedIn == attachPass; later passes that re-create nothing keep it) that was not a
  // click goes back to the window the user was in. A click anywhere, or our own activation of an
  // editor, ends the hold.
  void holdActivation() {
    if (!keepActive) return;
    // Minimized, hidden or disabled (a modal dialog is up): no longer a window to hand back to.
    if (GetTickCount64() > keepUntil || !IsWindow(keepActive) || IsIconic(keepActive) || !IsWindowVisible(keepActive) ||
        !IsWindowEnabled(keepActive)) {
      keepActive = nullptr;
      return;
    }
    HWND now = GetActiveWindow();
    if (!now || now == keepActive) return;  // nothing to undo (or the app is in the background)
    const Editor* e = findWindow(now);
    if (!e || e->attachedIn != attachPass || e->clickActivated) {
      keepActive = nullptr;  // the user moved on
      return;
    }
    SetActiveWindow(keepActive);  // WM_ACTIVATE hands an editor's view the keyboard
  }

  void setTitle(Editor& ed) {
    if (ed.hwnd) SetWindowTextW(ed.hwnd, widen(ed.nodeTitle + " - " + pluginName(ed.inst.get())).c_str());
  }

  void destroy(Editor& ed) {
    if (ed.hwnd) DestroyWindow(ed.hwnd);  // WM_DESTROY marks it dead
    ed.dead = true;
    ed.hwnd = nullptr;
  }

  // Closes the plugin's view in ed's frame. The instance leaves ed before the call, so a frame
  // destroyed while the plugin runs (WM_DESTROY) does not close the same view a second time. A view
  // whose setEditorSize is still running (we got here from a message loop the plugin runs inside
  // it) is not closed under it: the instance only leaves ed, and userResized closes the view once
  // that call has returned.
  void closeView(Editor& ed) {
    if (ed.inst && ed.inst.get() == ed.sizing) {
      ed.inst = nullptr;
      return;
    }
    if (sq::Ptr<IPluginInstance> inst = std::move(ed.inst)) {
      Busy guard(*this);
      inst->closeEditor();
    }
  }

  // WM_CLOSE: close the plugin's view, drop the window, and keep the plugin's state in the song.
  void closeFromUser(Editor& ed) {
    Busy guard(*this);
    const sq_guid node = ed.node;
    const int32_t slot = ed.slot;
    const bool had = static_cast<bool>(ed.inst);
    closeView(ed);
    destroy(ed);
    if (had) shell.capturePluginStates(&node, slot);
  }
};

PluginEditorHost::PluginEditorHost(GuiContext& ctx, Shell& shell) : impl_(std::make_unique<Impl>(ctx, shell)) {}

PluginEditorHost::~PluginEditorHost() { closeAll(); }

bool PluginEditorHost::open(const sq_guid& node, int32_t slot, const std::string& nodeTitle, void* ownerHwnd) {
  Impl& im = *impl_;
  if (im.busy) {  // reached from a message loop a plugin runs inside one of our calls (see Impl::Busy)
    im.ctx.setStatus("a plugin editor is busy (a dialog of the plugin is open?): try again once it is done");
    return false;
  }
  im.purge();
  engine::audio::AudioEngine* eng = im.shell.audio();
  if (!eng) {
    im.ctx.setStatus("plugin editors need the audio engine");
    return false;
  }
  // idle()'s baseline for a first editor, taken before the instance is looked up: a recompile while
  // openEditor runs (a plugin's dialog loop) then still reaches idle(), which moves the view to the
  // new instance (the retired one is only deactivated, its view is ours to close).
  const uint64_t compileBefore = eng->compileCount(), serialBefore = im.ctx.docSerial();
  im.keepActive = nullptr;  // the user asked for an editor: it may take the activation
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
  sq_result r = SQ_OK;
  {
    Impl::Busy guard(im);
    r = inst->openEditor(hwnd, "win32", &w, &h);
    if (!ed->hwnd) {  // the frame went away while the plugin ran (its owner, the main window, closed)
      if (SQ_SUCCEEDED(r)) inst->closeEditor();
      return false;
    }
  }
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
    im.lastCompile = compileBefore;
    im.lastSerial = serialBefore;
  }
  im.editors.push_back(std::move(ed));
  im.ctx.setStatus(title);
  return true;
}

void PluginEditorHost::closeAll() {
  Impl& im = *impl_;
  {
    Impl::Busy guard(im);
    for (size_t i = 0; i < im.editors.size(); ++i) {
      Impl::Editor& ed = *im.editors[i];
      im.closeView(ed);
      im.destroy(ed);
    }
  }
  // All are dead now. Freed here unless this closeAll runs inside a plugin call that holds one
  // (the main window closed from a plugin's message loop): then the next top-level pump frees them.
  im.purge();
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
  im.holdActivation();
  const uint64_t cc = eng->compileCount(), ds = im.ctx.docSerial();
  if (cc == im.lastCompile && ds == im.lastSerial) return;
  im.lastCompile = cc;
  im.lastSerial = ds;
  // Taken before any view is re-created: a plugin's attach may activate its own window (Surge XT
  // does), and the window the user was in gets the activation back once all views are in.
  HWND active = GetActiveWindow();
  const uint64_t pass = im.attachPass + 1;
  bool swapped = false;
  {
    // Plugin calls below may pump messages (see Impl::Busy): ed stays allocated, and its frame is
    // checked again after each call.
    Impl::Busy guard(im);
    for (size_t i = 0; i < im.editors.size(); ++i) {
      Impl::Editor& ed = *im.editors[i];
      if (ed.dead || !ed.hwnd) continue;
      sq::Ptr<IPluginInstance> cur = eng->pluginInstance(ed.node, ed.slot);
      if (cur.get() == ed.inst.get()) continue;
      // The graph was recompiled (new instance behind the insert) or the insert is gone.
      const bool reopen = cur && cur->hasEditor();
      im.closeView(ed);
      if (ed.dead || !ed.hwnd) continue;
      if (reopen) {
        uint32_t w = 0, h = 0;
        ed.hostResize = true;
        const sq_result r = cur->openEditor(ed.hwnd, "win32", &w, &h);
        if (ed.dead || !ed.hwnd) {  // the frame went away while the plugin ran, and the view with it
          if (SQ_SUCCEEDED(r)) cur->closeEditor();
          continue;
        }
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
          ed.attachedIn = pass;
          swapped = true;
          continue;
        }
        ed.hostResize = false;
      }
      im.destroy(ed);
    }
  }
  im.purge();
  if (!swapped) return;  // a pass that re-created no view (a docSerial change) leaves the hold alone
  im.attachPass = pass;
  if (!active || !IsWindow(active)) return;
  im.keepActive = active;
  im.keepUntil = GetTickCount64() + kKeepActiveMs;
  if (GetActiveWindow() != active) {
    SetActiveWindow(active);  // WM_ACTIVATE hands an editor's view the keyboard
    return;
  }
  // Still active, so no WM_ACTIVATE: its new view gets the keyboard unless it already has it.
  if (im.findWindow(active))
    if (HWND child = GetWindow(active, GW_CHILD))
      if (HWND f = GetFocus(); f != child && !IsChild(child, f)) SetFocus(child);
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
