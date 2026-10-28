// X11 window backend (Linux/BSD): Xlib window presenting the portable software renderer
// (soft_renderer.cpp) with XPutImage. Built when CMake finds X11 (SQ_UI_X11); without a display
// createWindow() returns nullptr and the app says so.
#if defined(SQ_UI_X11) && !defined(_WIN32)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "soft_renderer.hpp"
#include "sq/ui/ui.hpp"
// Xlib after our headers: it defines macros (None, Status, Bool...) that clash with C++ names.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#ifdef None
#undef None
#endif

namespace sq::ui {
namespace {

uint64_t nowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

class X11Window final : public IWindow {
 public:
  bool create(const WindowDesc& d) {
    dpy_ = XOpenDisplay(nullptr);
    if (!dpy_) return false;
    const int screen = DefaultScreen(dpy_);
    Visual* vis = DefaultVisual(dpy_, screen);
    depth_ = DefaultDepth(dpy_, screen);
    if (depth_ < 24 || vis->c_class != TrueColor || vis->red_mask != 0xFF0000 || vis->green_mask != 0x00FF00 || vis->blue_mask != 0x0000FF) {
      XCloseDisplay(dpy_);
      dpy_ = nullptr;
      return false;  // only the common 24/32-bit BGRX visual is supported
    }
    visual_ = vis;
    scale_ = detectScale();
    width_ = std::max(64, static_cast<int>(std::lround(static_cast<float>(d.width) * scale_)));
    height_ = std::max(64, static_cast<int>(std::lround(static_cast<float>(d.height) * scale_)));
    XSetWindowAttributes a{};
    a.background_pixel = 0x1C1D21;
    a.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask |
                   LeaveWindowMask | FocusChangeMask;
    win_ = XCreateWindow(dpy_, RootWindow(dpy_, screen), 0, 0, static_cast<unsigned>(width_), static_cast<unsigned>(height_), 0, depth_,
                         InputOutput, visual_, CWBackPixel | CWEventMask, &a);
    wmDelete_ = XInternAtom(dpy_, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy_, win_, &wmDelete_, 1);
    setTitle(d.title);
    XClassHint* ch = XAllocClassHint();
    if (ch) {
      ch->res_name = const_cast<char*>("squizzystudio");
      ch->res_class = const_cast<char*>("SquizzyStudio");
      XSetClassHint(dpy_, win_, ch);
      XFree(ch);
    }
    gc_ = XCreateGC(dpy_, win_, 0, nullptr);
    im_ = XOpenIM(dpy_, nullptr, nullptr, nullptr);
    if (im_)
      ic_ = XCreateIC(im_, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, win_, XNFocusWindow, win_, nullptr);
    if (d.visible) show();
    dirty_ = true;
    return true;
  }
  ~X11Window() override { destroy(); }

  void setCallbacks(WindowCallbacks cb) override { cb_ = std::move(cb); }
  void show() override {
    if (!dpy_ || !win_) return;
    XMapWindow(dpy_, win_);
    XFlush(dpy_);
  }
  void invalidate() override { dirty_ = true; }
  bool pumpEvents(bool wait) override {
    if (closed_ || !dpy_) return false;
    if (wait && !dirty_ && XPending(dpy_) == 0) {
      XEvent ev;
      XPeekEvent(dpy_, &ev);
    }
    while (!closed_ && dpy_ && XPending(dpy_) > 0) {
      XEvent ev;
      XNextEvent(dpy_, &ev);
      if (XFilterEvent(&ev, 0)) continue;
      handle(ev);
    }
    if (closed_ || !dpy_) return false;
    if (dirty_) paint();
    return !closed_;
  }
  void close() override {
    if (closed_) return;
    closed_ = true;
    destroy();
  }
  void* nativeHandle() const override { return reinterpret_cast<void*>(static_cast<uintptr_t>(win_)); }
  const char* backendName() const override { return "x11"; }
  float dpiScale() const override { return scale_; }

 private:
  static float envScale() {
    const char* s = std::getenv("SQ_UI_SCALE");
    if (!s || !*s) return 0;
    const float v = static_cast<float>(std::atof(s));
    return v > 0.25f && v < 8.0f ? v : 0.0f;
  }
  float detectScale() {
    if (float e = envScale(); e > 0) return e;
    const char* rms = XResourceManagerString(dpy_);
    if (rms) {
      XrmInitialize();
      XrmDatabase db = XrmGetStringDatabase(rms);
      if (db) {
        char* type = nullptr;
        XrmValue v{};
        float dpi = 0;
        if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &v) && v.addr) dpi = static_cast<float>(std::atof(v.addr));
        XrmDestroyDatabase(db);
        if (dpi >= 48 && dpi <= 480) return dpi / 96.0f;
      }
    }
    return 1.0f;
  }
  void setTitle(const std::string& t) {
    XStoreName(dpy_, win_, t.c_str());
    Atom netName = XInternAtom(dpy_, "_NET_WM_NAME", False);
    Atom utf8 = XInternAtom(dpy_, "UTF8_STRING", False);
    XChangeProperty(dpy_, win_, netName, utf8, 8, PropModeReplace, reinterpret_cast<const unsigned char*>(t.data()), static_cast<int>(t.size()));
  }
  void destroy() {
    if (!dpy_) return;
    if (ic_) XDestroyIC(ic_);
    if (im_) XCloseIM(im_);
    ic_ = nullptr;
    im_ = nullptr;
    if (gc_) XFreeGC(dpy_, gc_);
    gc_ = nullptr;
    if (win_) XDestroyWindow(dpy_, win_);
    win_ = 0;
    XCloseDisplay(dpy_);
    dpy_ = nullptr;
  }

  static unsigned mods(unsigned state) {
    unsigned m = 0;
    if (state & ShiftMask) m |= kModShift;
    if (state & ControlMask) m |= kModCtrl;
    if (state & Mod1Mask) m |= kModAlt;
    return m;
  }
  static Key mapKey(KeySym ks) {
    switch (ks) {
      case XK_Escape: return Key::Escape;
      case XK_Return: case XK_KP_Enter: return Key::Enter;
      case XK_F1: return Key::F1;
      case XK_F2: return Key::F2;
      case XK_F3: return Key::F3;
      case XK_F4: return Key::F4;
      case XK_F5: return Key::F5;
      case XK_F6: return Key::F6;
      case XK_F7: return Key::F7;
      case XK_F8: return Key::F8;
      case XK_F9: return Key::F9;
      case XK_F10: return Key::F10;
      case XK_F11: return Key::F11;
      case XK_F12: return Key::F12;
      case XK_space: return Key::Space;
      case XK_Left: case XK_KP_Left: return Key::Left;
      case XK_Right: case XK_KP_Right: return Key::Right;
      case XK_Up: case XK_KP_Up: return Key::Up;
      case XK_Down: case XK_KP_Down: return Key::Down;
      case XK_Tab: case XK_ISO_Left_Tab: return Key::Tab;
      case XK_Home: case XK_KP_Home: return Key::Home;
      case XK_End: case XK_KP_End: return Key::End;
      case XK_Prior: case XK_KP_Prior: return Key::PageUp;
      case XK_Next: case XK_KP_Next: return Key::PageDown;
      case XK_plus: case XK_equal: case XK_KP_Add: return Key::Plus;
      case XK_minus: case XK_KP_Subtract: return Key::Minus;
      case XK_0: case XK_KP_0: return Key::Zero;
      case XK_Delete: case XK_KP_Delete: return Key::Delete;
      case XK_BackSpace: return Key::Backspace;
      case XK_Insert: case XK_KP_Insert: return Key::Insert;
      default: return Key::Unknown;
    }
  }

  void handle(XEvent& ev) {
    switch (ev.type) {
      case Expose:
        dirty_ = true;
        break;
      case ConfigureNotify:
        if (ev.xconfigure.width != width_ || ev.xconfigure.height != height_) {
          width_ = ev.xconfigure.width;
          height_ = ev.xconfigure.height;
          if (cb_.onResize) cb_.onResize(Size{width_, height_});
          dirty_ = true;
        }
        break;
      case ClientMessage:
        if (static_cast<Atom>(ev.xclient.data.l[0]) == wmDelete_) {
          if (cb_.onCloseRequest && !cb_.onCloseRequest()) break;
          close();
        }
        break;
      case KeyPress: {
        char buf[64] = {};
        KeySym ks = 0;
        int n = 0;
        if (ic_) {
          Status st = 0;
          n = Xutf8LookupString(ic_, &ev.xkey, buf, sizeof buf - 1, &ks, &st);
          if (st == XBufferOverflow) n = 0;
        } else {
          n = XLookupString(&ev.xkey, buf, sizeof buf - 1, &ks, nullptr);
        }
        const unsigned m = mods(ev.xkey.state);
        const Key k = mapKey(ks);
        if (cb_.onKey) cb_.onKey(k);
        if (cb_.onKeyEx) cb_.onKeyEx(k, m);
        if (cb_.onChar) {
          if ((m & kModCtrl) && ks >= XK_a && ks <= XK_z) {
            cb_.onChar(static_cast<uint32_t>(ks - XK_a + 1), m);  // Ctrl+letter = 1..26 like WM_CHAR
          } else if ((m & kModCtrl) && ks >= XK_A && ks <= XK_Z) {
            cb_.onChar(static_cast<uint32_t>(ks - XK_A + 1), m);
          } else if (n > 0) {
            std::string s(buf, static_cast<size_t>(n));
            for (size_t i = 0; i < s.size();) {
              const auto c = static_cast<unsigned char>(s[i]);
              uint32_t cp = c;
              int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
              cp = extra ? (c & (0x3F >> extra)) : c;
              ++i;
              for (int e = 0; e < extra && i < s.size(); ++e, ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
              if (cp == 0x7F) continue;  // Delete arrives as a key
              cb_.onChar(cp, m);
            }
          }
        }
        break;
      }
      case ButtonPress:
      case ButtonRelease: {
        MouseEvent e;
        e.x = static_cast<float>(ev.xbutton.x);
        e.y = static_cast<float>(ev.xbutton.y);
        e.modifiers = mods(ev.xbutton.state);
        const unsigned b = ev.xbutton.button;
        if (b == 4 || b == 5) {
          if (ev.type == ButtonRelease) break;
          e.action = MouseAction::Wheel;
          e.wheel = b == 4 ? 1.0f : -1.0f;
        } else if (b == 6 || b == 7) {
          if (ev.type == ButtonRelease) break;
          e.action = MouseAction::Wheel;
          e.wheel = b == 6 ? 1.0f : -1.0f;
          e.modifiers |= kModShift;  // horizontal scroll = shift+wheel
        } else {
          e.button = b == 1 ? MouseButton::Left : b == 3 ? MouseButton::Right : b == 2 ? MouseButton::Middle : MouseButton::None;
          if (e.button == MouseButton::None) break;
          e.action = ev.type == ButtonPress ? MouseAction::Down : MouseAction::Up;
          if (ev.type == ButtonPress && e.button == MouseButton::Left) {
            const uint64_t t = nowMs();
            if (t - lastClickMs_ < 400 && std::fabs(e.x - lastClickX_) < 5 && std::fabs(e.y - lastClickY_) < 5 && !lastWasDouble_) {
              e.action = MouseAction::DoubleClick;
              lastWasDouble_ = true;
            } else {
              lastWasDouble_ = false;
            }
            lastClickMs_ = t;
            lastClickX_ = e.x;
            lastClickY_ = e.y;
          }
        }
        if (cb_.onMouse) cb_.onMouse(e);
        break;
      }
      case MotionNotify: {
        // Coalesce queued motion events.
        while (XPending(dpy_) > 0) {
          XEvent next;
          XPeekEvent(dpy_, &next);
          if (next.type != MotionNotify) break;
          XNextEvent(dpy_, &ev);
        }
        MouseEvent e;
        e.action = MouseAction::Move;
        e.x = static_cast<float>(ev.xmotion.x);
        e.y = static_cast<float>(ev.xmotion.y);
        e.modifiers = mods(ev.xmotion.state);
        if (cb_.onMouse) cb_.onMouse(e);
        break;
      }
      case LeaveNotify: {
        MouseEvent e;
        e.action = MouseAction::Leave;
        e.x = static_cast<float>(ev.xcrossing.x);
        e.y = static_cast<float>(ev.xcrossing.y);
        if (cb_.onMouse) cb_.onMouse(e);
        break;
      }
      case FocusIn:
        if (ic_) XSetICFocus(ic_);
        break;
      case FocusOut:
        if (ic_) XUnsetICFocus(ic_);
        break;
      default:
        break;
    }
  }

  void paint() {
    dirty_ = false;
    if (!dpy_ || width_ <= 0 || height_ <= 0) return;
    const size_t stride = static_cast<size_t>(width_) * 4;
    pixels_.resize(stride * static_cast<size_t>(height_));
    SoftSurface s{pixels_.data(), width_, height_, stride};
    auto r = makeSoftRenderer(s, scale_);
    if (cb_.onPaint) cb_.onPaint(*r);
    else r->clear(Color::rgb(0x202124));
    XImage* img = XCreateImage(dpy_, visual_, static_cast<unsigned>(depth_), ZPixmap, 0, reinterpret_cast<char*>(pixels_.data()),
                               static_cast<unsigned>(width_), static_cast<unsigned>(height_), 32, static_cast<int>(stride));
    if (!img) return;
    img->byte_order = LSBFirst;
    XPutImage(dpy_, win_, gc_, img, 0, 0, 0, 0, static_cast<unsigned>(width_), static_cast<unsigned>(height_));
    img->data = nullptr;  // owned by pixels_
    XDestroyImage(img);
    XFlush(dpy_);
  }

  Display* dpy_ = nullptr;
  Window win_ = 0;
  Visual* visual_ = nullptr;
  int depth_ = 24;
  GC gc_ = nullptr;
  XIM im_ = nullptr;
  XIC ic_ = nullptr;
  Atom wmDelete_ = 0;
  WindowCallbacks cb_;
  std::vector<uint8_t> pixels_;
  int width_ = 0, height_ = 0;
  float scale_ = 1.0f;
  bool dirty_ = false;
  bool closed_ = false;
  uint64_t lastClickMs_ = 0;
  float lastClickX_ = 0, lastClickY_ = 0;
  bool lastWasDouble_ = false;
};

}  // namespace

std::unique_ptr<IWindow> createWindow(const WindowDesc& desc) {
  auto w = std::make_unique<X11Window>();
  if (!w->create(desc)) return nullptr;
  return w;
}
const char* defaultBackendName() { return "x11"; }

}  // namespace sq::ui
#endif
