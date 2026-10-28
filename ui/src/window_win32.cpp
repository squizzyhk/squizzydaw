// Win32 window. Paints with the portable software renderer (soft_renderer.cpp, anti-aliased, real
// text; same pixels as every other platform) presented with SetDIBitsToDevice. SQ_UI_BACKEND=gdi
// selects the old GDI renderer (no alpha, no anti-aliasing).
#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "soft_renderer.hpp"

#include "sq/ui/accessibility.hpp"
#include "sq/ui/uia.hpp"
#include "sq/ui/ui.hpp"

namespace sq::ui {
namespace {

std::wstring widen(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

COLORREF toColorRef(Color c) {
  auto ch = [](float v) { return static_cast<BYTE>(std::lround((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0f)); };
  return RGB(ch(c.r), ch(c.g), ch(c.b));
}

class GdiRenderer final : public IRenderer {
 public:
  GdiRenderer(HDC dc, Size sz, float scale) : dc_(dc), size_(sz), scale_(scale) {}
  const char* backendName() const override { return "gdi"; }
  Size size() const override { return size_; }
  float scale() const override { return scale_; }
  void clear(Color c) override { fillRect(Rect{0, 0, static_cast<float>(size_.width), static_cast<float>(size_.height)}, c); }
  void fillRect(const Rect& r, Color c) override {
    RECT rc = toRect(r);
    HBRUSH b = CreateSolidBrush(toColorRef(c));
    FillRect(dc_, &rc, b);
    DeleteObject(b);
  }
  void strokeRect(const Rect& r, Color c, float width) override {
    HPEN pen = CreatePen(PS_SOLID, static_cast<int>(std::lround(width < 1 ? 1 : width)), toColorRef(c));
    HGDIOBJ oldPen = SelectObject(dc_, pen);
    HGDIOBJ oldBrush = SelectObject(dc_, GetStockObject(NULL_BRUSH));
    RECT rc = toRect(r);
    Rectangle(dc_, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(dc_, oldBrush);
    SelectObject(dc_, oldPen);
    DeleteObject(pen);
  }
  void drawText(std::string_view utf8, const Rect& r, Color c, float sizePx, TextAlign align) override {
    HFONT font = CreateFontW(-static_cast<int>(std::lround(sizePx)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc_, font);
    SetBkMode(dc_, TRANSPARENT);
    SetTextColor(dc_, toColorRef(c));
    RECT rc = toRect(r);
    std::wstring w = widen(utf8);
    UINT fmt = DT_NOPREFIX | DT_WORDBREAK | (align == TextAlign::Center ? DT_CENTER : align == TextAlign::Right ? DT_RIGHT : DT_LEFT);
    DrawTextW(dc_, w.c_str(), static_cast<int>(w.size()), &rc, fmt);
    SelectObject(dc_, old);
    DeleteObject(font);
  }
  void drawLine(Point a, Point b, Color c, float width) override {
    HPEN pen = CreatePen(PS_SOLID, static_cast<int>(std::lround(width < 1 ? 1 : width)), toColorRef(c));
    HGDIOBJ oldPen = SelectObject(dc_, pen);
    MoveToEx(dc_, static_cast<int>(std::lround(a.x)), static_cast<int>(std::lround(a.y)), nullptr);
    LineTo(dc_, static_cast<int>(std::lround(b.x)), static_cast<int>(std::lround(b.y)));
    SelectObject(dc_, oldPen);
    DeleteObject(pen);
  }
  void fillEllipse(const Rect& r, Color c) override {
    HBRUSH b = CreateSolidBrush(toColorRef(c));
    HGDIOBJ oldBrush = SelectObject(dc_, b);
    HGDIOBJ oldPen = SelectObject(dc_, GetStockObject(NULL_PEN));
    RECT rc = toRect(r);
    Ellipse(dc_, rc.left, rc.top, rc.right + 1, rc.bottom + 1);
    SelectObject(dc_, oldPen);
    SelectObject(dc_, oldBrush);
    DeleteObject(b);
  }
  void fillRoundRect(const Rect& r, float radius, Color c) override {
    HBRUSH b = CreateSolidBrush(toColorRef(c));
    HGDIOBJ oldBrush = SelectObject(dc_, b);
    HGDIOBJ oldPen = SelectObject(dc_, GetStockObject(NULL_PEN));
    RECT rc = toRect(r);
    int d = static_cast<int>(std::lround(radius * 2));
    RoundRect(dc_, rc.left, rc.top, rc.right + 1, rc.bottom + 1, d, d);
    SelectObject(dc_, oldPen);
    SelectObject(dc_, oldBrush);
    DeleteObject(b);
  }
  void pushClip(const Rect& r) override {
    SaveDC(dc_);
    RECT rc = toRect(r);
    IntersectClipRect(dc_, rc.left, rc.top, rc.right, rc.bottom);
  }
  void popClip() override { RestoreDC(dc_, -1); }
  float measureText(std::string_view utf8, float sizePx) override {
    HFONT font = CreateFontW(-static_cast<int>(std::lround(sizePx)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ old = SelectObject(dc_, font);
    std::wstring w = widen(utf8);
    SIZE sz{};
    GetTextExtentPoint32W(dc_, w.c_str(), static_cast<int>(w.size()), &sz);
    SelectObject(dc_, old);
    DeleteObject(font);
    return static_cast<float>(sz.cx);
  }

 private:
  static RECT toRect(const Rect& r) {
    return RECT{static_cast<LONG>(std::lround(r.x)), static_cast<LONG>(std::lround(r.y)), static_cast<LONG>(std::lround(r.x + r.w)),
                static_cast<LONG>(std::lround(r.y + r.h))};
  }
  HDC dc_;
  Size size_;
  float scale_;
};

const wchar_t* kClassName = L"SquizzyStudioWindow";

class Win32Window final : public IWindow {
 public:
  explicit Win32Window(const WindowDesc& d) {
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!registered) {
      WNDCLASSEXW wc{};
      wc.cbSize = sizeof wc;
      wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
      wc.lpfnWndProc = &Win32Window::wndProc;
      wc.hInstance = inst;
      wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
      wc.lpszClassName = kClassName;
      RegisterClassExW(&wc);
      registered = true;
    }
    enableDpiAwareness();
    // WindowDesc sizes are DIPs: scale by the system DPI, and fit the work area.
    UINT sysDpi = 96;
    if (HDC sdc = GetDC(nullptr)) {
      sysDpi = static_cast<UINT>(GetDeviceCaps(sdc, LOGPIXELSX));
      ReleaseDC(nullptr, sdc);
    }
    RECT work{0, 0, 1920, 1080};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = MulDiv(d.width, static_cast<int>(sysDpi), 96), h = MulDiv(d.height, static_cast<int>(sysDpi), 96);
    w = std::min<int>(w, work.right - work.left);
    h = std::min<int>(h, work.bottom - work.top);
    hwnd_ = CreateWindowExW(0, kClassName, widen(d.title).c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr,
                            inst, this);
    if (hwnd_ && d.visible) show();
  }
  ~Win32Window() override {
    if (uia_) uia_->windowDestroyed();
    if (hwnd_) {
      SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
      DestroyWindow(hwnd_);
    }
  }
  bool valid() const { return hwnd_ != nullptr; }

  void setCallbacks(WindowCallbacks cb) override { cb_ = std::move(cb); }
  void show() override {
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
  }
  void invalidate() override {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
  }
  bool pumpEvents(bool wait) override {
    MSG msg;
    if (wait && !closed_) {
      if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {
        closed_ = true;
        return false;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    while (!closed_ && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        closed_ = true;
        break;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    return !closed_;
  }
  void close() override {
    if (hwnd_) DestroyWindow(hwnd_);
  }
  void* nativeHandle() const override { return hwnd_; }
  float dpiScale() const override {
    UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
    return dpi ? static_cast<float>(dpi) / 96.0f : 1.0f;
  }
  void setMouseCapture(bool capture) override {
    if (!hwnd_) return;
    if (capture) SetCapture(hwnd_);
    else if (GetCapture() == hwnd_) ReleaseCapture();
  }
  // UI Automation: WM_GETOBJECT hands the bridge's root provider to screen readers.
  void setAccessibilityRoot(AccessibleNode* root) override {
    if (!uia_) {
      if (!root || !hwnd_) return;
      uia_ = std::make_unique<UiaBridge>(hwnd_);
    }
    uia_->setRoot(root);
  }
  void notifyAccessibilityChanged(AccessibleNode* node) override {
    if (uia_) uia_->notifyChanged(node);
  }

 private:
  static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
      case WM_ERASEBKGND: return 1;
      case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (self->cb_.onDpiChanged) self->cb_.onDpiChanged(static_cast<float>(HIWORD(wp)) / 96.0f);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
      }
      case WM_GETOBJECT:
        if (self->uia_) {
          bool handled = false;
          intptr_t r = self->uia_->handleGetObject(static_cast<uintptr_t>(wp), static_cast<intptr_t>(lp), handled);
          if (handled) return static_cast<LRESULT>(r);
        }
        return DefWindowProcW(h, msg, wp, lp);
      case WM_PAINT: self->paint(); return 0;
      case WM_SIZE:
        if (self->cb_.onResize) self->cb_.onResize(Size{LOWORD(lp), HIWORD(lp)});
        InvalidateRect(h, nullptr, FALSE);
        return 0;
      case WM_KEYDOWN:
      case WM_SYSKEYDOWN: {
        Key k = mapKey(wp);
        unsigned mods = currentModifiers();
        if (self->cb_.onKey) self->cb_.onKey(k);
        if (self->cb_.onKeyEx) self->cb_.onKeyEx(k, mods);
        // Letters/digits map to Key::Unknown; they (and ctrl+letter shortcuts) arrive via onChar.
        if (msg == WM_SYSKEYDOWN) return DefWindowProcW(h, msg, wp, lp);
        return 0;
      }
      case WM_CHAR:
        if (self->cb_.onChar && wp >= 1 && wp < 0x10000) self->cb_.onChar(static_cast<uint32_t>(wp), currentModifiers());
        return 0;
      case WM_MOUSEMOVE:
      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_RBUTTONDOWN:
      case WM_RBUTTONUP:
      case WM_MBUTTONDOWN:
      case WM_MBUTTONUP:
      case WM_LBUTTONDBLCLK:
      case WM_MOUSEHWHEEL:
      case WM_MOUSEWHEEL: {
        if (!self->cb_.onMouse) return 0;
        MouseEvent e;
        POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) ScreenToClient(h, &pt);
        e.x = static_cast<float>(pt.x);
        e.y = static_cast<float>(pt.y);
        e.modifiers = currentModifiers();
        switch (msg) {
          case WM_MOUSEMOVE: e.action = MouseAction::Move; break;
          case WM_LBUTTONDOWN: e.action = MouseAction::Down; e.button = MouseButton::Left; break;
          case WM_LBUTTONUP: e.action = MouseAction::Up; e.button = MouseButton::Left; break;
          case WM_RBUTTONDOWN: e.action = MouseAction::Down; e.button = MouseButton::Right; break;
          case WM_RBUTTONUP: e.action = MouseAction::Up; e.button = MouseButton::Right; break;
          case WM_MBUTTONDOWN: e.action = MouseAction::Down; e.button = MouseButton::Middle; break;
          case WM_MBUTTONUP: e.action = MouseAction::Up; e.button = MouseButton::Middle; break;
          case WM_LBUTTONDBLCLK: e.action = MouseAction::DoubleClick; e.button = MouseButton::Left; break;
          case WM_MOUSEHWHEEL:
            e.action = MouseAction::Wheel;
            e.wheel = -static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
            e.modifiers |= kModShift;
            break;
          default: e.action = MouseAction::Wheel; e.wheel = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA; break;
        }
        self->cb_.onMouse(e);
        return 0;
      }
      case WM_CLOSE:
        if (self->cb_.onCloseRequest && !self->cb_.onCloseRequest()) return 0;
        DestroyWindow(h);
        return 0;
      case WM_DESTROY:
        if (self->uia_) self->uia_->windowDestroyed();
        self->hwnd_ = nullptr;
        self->closed_ = true;
        PostQuitMessage(0);
        return 0;
      default: return DefWindowProcW(h, msg, wp, lp);
    }
  }

  static bool useGdi() {
    const char* b = std::getenv("SQ_UI_BACKEND");
    return b && std::strcmp(b, "gdi") == 0;
  }

  void paint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    Size sz{rc.right - rc.left, rc.bottom - rc.top};
    float scale = static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f;
    if (!(scale > 0)) scale = 1.0f;
    if (!useGdi() && sz.width > 0 && sz.height > 0) {
      const size_t stride = static_cast<size_t>(sz.width) * 4;
      pixels_.resize(stride * static_cast<size_t>(sz.height));
      SoftSurface surf{pixels_.data(), sz.width, sz.height, stride};
      auto r = makeSoftRenderer(surf, scale);
      if (cb_.onPaint) cb_.onPaint(*r);
      else r->clear(Color::rgb(0x202124));
      BITMAPINFO bi{};
      bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth = sz.width;
      bi.bmiHeader.biHeight = -sz.height;  // top-down
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      bi.bmiHeader.biCompression = BI_RGB;
      SetDIBitsToDevice(dc, 0, 0, static_cast<DWORD>(sz.width), static_cast<DWORD>(sz.height), 0, 0, 0, static_cast<UINT>(sz.height),
                        pixels_.data(), &bi, DIB_RGB_COLORS);
      EndPaint(hwnd_, &ps);
      return;
    }
    // Double buffer to avoid flicker.
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, sz.width > 0 ? sz.width : 1, sz.height > 0 ? sz.height : 1);
    HGDIOBJ old = SelectObject(mem, bmp);
    GdiRenderer r(mem, sz, scale);
    if (cb_.onPaint) cb_.onPaint(r);
    else r.clear(Color::rgb(0x202124));
    BitBlt(dc, 0, 0, sz.width, sz.height, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
  }

  // Per-monitor v2 DPI awareness (crisp rendering at 125-200% scaling, follows monitor changes).
  // Resolved dynamically: older Windows builds simply stay system-aware/unaware.
  static void enableDpiAwareness() {
    static bool done = false;
    if (done) return;
    done = true;
    if (HMODULE u = GetModuleHandleW(L"user32.dll")) {
      using SetCtx = BOOL(WINAPI*)(HANDLE);
      if (auto fn = reinterpret_cast<SetCtx>(reinterpret_cast<void*>(GetProcAddress(u, "SetProcessDpiAwarenessContext")))) {
        if (fn(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)))) return;  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
      }
      using SetAware = BOOL(WINAPI*)();
      if (auto fn = reinterpret_cast<SetAware>(reinterpret_cast<void*>(GetProcAddress(u, "SetProcessDPIAware")))) fn();
    }
  }

  static unsigned currentModifiers() {
    unsigned m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= kModShift;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= kModCtrl;
    if (GetKeyState(VK_MENU) & 0x8000) m |= kModAlt;
    return m;
  }
  static Key mapKey(WPARAM wp) {
    switch (wp) {
      case VK_ESCAPE: return Key::Escape;
      case VK_RETURN: return Key::Enter;
      case VK_F5: return Key::F5;
      case VK_SPACE: return Key::Space;
      case VK_LEFT: return Key::Left;
      case VK_RIGHT: return Key::Right;
      case VK_UP: return Key::Up;
      case VK_DOWN: return Key::Down;
      case VK_TAB: return Key::Tab;
      case VK_HOME: return Key::Home;
      case VK_END: return Key::End;
      case VK_PRIOR: return Key::PageUp;
      case VK_NEXT: return Key::PageDown;
      case VK_OEM_PLUS: case VK_ADD: return Key::Plus;
      case VK_OEM_MINUS: case VK_SUBTRACT: return Key::Minus;
      case '0': case VK_NUMPAD0: return Key::Zero;
      case VK_DELETE: return Key::Delete;
      case VK_BACK: return Key::Backspace;
      case VK_INSERT: return Key::Insert;
      case VK_F1: return Key::F1;
      case VK_F2: return Key::F2;
      case VK_F3: return Key::F3;
      case VK_F4: return Key::F4;
      case VK_F6: return Key::F6;
      case VK_F7: return Key::F7;
      case VK_F8: return Key::F8;
      case VK_F9: return Key::F9;
      case VK_F10: return Key::F10;
      case VK_F11: return Key::F11;
      case VK_F12: return Key::F12;
      default: return Key::Unknown;
    }
  }


  HWND hwnd_ = nullptr;
  std::vector<uint8_t> pixels_;
  bool closed_ = false;
  std::unique_ptr<UiaBridge> uia_;
  WindowCallbacks cb_;
};

}  // namespace

std::unique_ptr<IWindow> createWindow(const WindowDesc& desc) {
  auto w = std::make_unique<Win32Window>(desc);
  if (!w->valid()) return nullptr;
  return w;
}

const char* defaultBackendName() { return "win32"; }

}  // namespace sq::ui

#endif  // _WIN32
