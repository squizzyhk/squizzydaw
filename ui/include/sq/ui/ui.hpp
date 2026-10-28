// Squizzy Studio UI - backend-neutral window + renderer abstraction (in-process C++, not ABI).
//
// Backends: Direct2D/DirectWrite (Windows default), GDI (Windows fallback), and a portable
// software rasterizer used for offscreen rendering / snapshot tests. This header only speaks in
// floats, UTF-8 strings and plain colors so swapping a backend never changes callers.
// Renderer coordinates are device PIXELS; the retained widget layer (widgets.hpp) works in DIPs
// and converts through `Canvas`.
// Owner: the ui/ agent. Keep it backend-neutral.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace sq::ui {

class Image;          // image.hpp
class AccessibleNode;  // accessibility.hpp

struct Color {
  float r = 0, g = 0, b = 0, a = 1;
  static constexpr Color rgb(unsigned hex, float alpha = 1.0f) {
    return Color{((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, alpha};
  }
  // 0xRRGGBBAA
  static constexpr Color rgba(uint32_t v) {
    return Color{((v >> 24) & 0xFF) / 255.0f, ((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f};
  }
  uint32_t toRgba() const;
  Color withAlpha(float alpha) const { return Color{r, g, b, alpha}; }
  bool operator==(const Color&) const = default;
};

struct Point {
  float x = 0, y = 0;
};

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
  Rect inset(float d) const { return Rect{x + d, y + d, w - 2 * d, h - 2 * d}; }
  float right() const { return x + w; }
  float bottom() const { return y + h; }
};

struct Size {
  int width = 0, height = 0;
};

enum class TextAlign { Left, Center, Right };

// Immediate-mode drawing surface valid only inside a paint callback. Coordinates in pixels.
// Angles in degrees, 0 = 12 o'clock, clockwise positive.
class IRenderer {
 public:
  virtual ~IRenderer() = default;
  virtual const char* backendName() const = 0;
  virtual Size size() const = 0;
  virtual float scale() const = 0;  // DPI scale (1.0 = 96 dpi) x zoom
  virtual void clear(Color c) = 0;
  virtual void fillRect(const Rect& r, Color c) = 0;
  virtual void strokeRect(const Rect& r, Color c, float width) = 0;
  virtual void drawText(std::string_view utf8, const Rect& r, Color c, float sizePx, TextAlign align = TextAlign::Left) = 0;

  // M5 additions. The defaults degrade to rectangles so a minimal backend still works.
  virtual void fillRoundRect(const Rect& r, float radius, Color c) { (void)radius; fillRect(r, c); }
  virtual void strokeRoundRect(const Rect& r, float radius, Color c, float width) { (void)radius; strokeRect(r, c, width); }
  virtual void fillEllipse(const Rect& r, Color c) { fillRect(r, c); }
  virtual void strokeEllipse(const Rect& r, Color c, float width) { strokeRect(r, c, width); }
  virtual void drawLine(Point a, Point b, Color c, float width);
  // Circular arc around (cx,cy): startDeg..startDeg+sweepDeg, stroked with `width`.
  virtual void drawArc(Point center, float radius, float startDeg, float sweepDeg, Color c, float width);
  virtual void drawImage(const Image& img, const Rect& dst, float opacity = 1.0f) { (void)img, (void)dst, (void)opacity; }
  virtual void pushClip(const Rect& r) { (void)r; }
  virtual void popClip() {}
  // Width in pixels of a single line of text at sizePx (approximation allowed).
  virtual float measureText(std::string_view utf8, float sizePx) { return static_cast<float>(utf8.size()) * sizePx * 0.55f; }
  // Filled polygon (non-zero winding) and an open polyline. The defaults approximate with the
  // bounding box / line segments; the software renderer draws them anti-aliased.
  virtual void fillPolygon(const Point* pts, size_t count, Color c);
  virtual void strokePolyline(const Point* pts, size_t count, Color c, float width) {
    for (size_t i = 1; i < count; ++i) drawLine(pts[i - 1], pts[i], c, width);
  }
  // Text with a weight hint (bold = semibold UI font where the backend has one).
  virtual void drawTextStyled(std::string_view utf8, const Rect& r, Color c, float sizePx, TextAlign align, bool bold) {
    (void)bold;
    drawText(utf8, r, c, sizePx, align);
  }
};

enum class Key { Unknown, Escape, Enter, F5, Space, Left, Right, Up, Down, Tab, Home, End, PageUp, PageDown, Plus, Minus, Zero, Delete, Backspace,
                 // appended (letters/digits still arrive through onChar as well)
                 Insert, F1, F2, F3, F4, F6, F7, F8, F9, F10, F11, F12 };

enum Modifiers : unsigned { kModShift = 1u, kModCtrl = 2u, kModAlt = 4u };

// Hover: WidgetTree delivers pointer motion without a captured widget to the widget under the
// pointer (bubbling) as Hover; Move is only sent to the widget that captured the button press.
enum class MouseAction { Move, Down, Up, DoubleClick, Wheel, Leave, Hover };
enum class MouseButton { None, Left, Right, Middle };

struct MouseEvent {
  MouseAction action = MouseAction::Move;
  MouseButton button = MouseButton::None;
  float x = 0, y = 0;  // client pixels
  float wheel = 0;     // notches, positive = away from the user
  unsigned modifiers = 0;
};

struct WindowCallbacks {
  std::function<void(IRenderer&)> onPaint;
  std::function<void(Size)> onResize;
  std::function<void(Key)> onKey;
  std::function<bool()> onCloseRequest;  // return false to veto
  // M5 additions (all optional)
  std::function<void(Key, unsigned modifiers)> onKeyEx;  // called in addition to onKey
  std::function<void(const MouseEvent&)> onMouse;
  std::function<void(float dpiScale)> onDpiChanged;
  // Character input: UTF-16 code unit (or code point) + modifiers. Ctrl+letter arrives as 1..26.
  std::function<void(uint32_t ch, unsigned modifiers)> onChar;
};

// Software = the portable anti-aliased renderer with real text (soft_renderer.cpp), the default
// for every window backend; Raster = the minimal deterministic rasterizer of the snapshot tests.
enum class Backend { Auto, Direct2D, Gdi, Raster, Software };

struct WindowDesc {
  std::string title = "Squizzy Studio";
  int width = 1280;
  int height = 800;
  bool visible = true;
  // Auto = Direct2D, falling back to GDI. The env var SQ_UI_BACKEND=gdi|d2d overrides Auto.
  Backend backend = Backend::Auto;
};

class IWindow {
 public:
  virtual ~IWindow() = default;
  virtual void setCallbacks(WindowCallbacks cb) = 0;
  virtual void show() = 0;
  virtual void invalidate() = 0;  // schedule a repaint
  // Processes pending events. wait=true blocks until at least one event. Returns false once closed.
  virtual bool pumpEvents(bool wait) = 0;
  virtual void close() = 0;
  virtual void* nativeHandle() const = 0;  // HWND / NSView* / X11 Window
  // M5 additions
  virtual const char* backendName() const { return "none"; }
  virtual float dpiScale() const { return 1.0f; }
  virtual void setMouseCapture(bool capture) { (void)capture; }
  // Root of the accessibility tree exposed to screen readers (UI Automation on Windows).
  // Borrowed; pass nullptr before the tree dies.
  virtual void setAccessibilityRoot(AccessibleNode* root) { (void)root; }
  // Tell assistive tech that the tree / a node changed (structure or value).
  virtual void notifyAccessibilityChanged(AccessibleNode* node) { (void)node; }
};

// Creates a platform window with the requested renderer backend.
// Returns nullptr where no windowing backend exists (headless, macOS, Linux without X11).
std::unique_ptr<IWindow> createWindow(const WindowDesc& desc);

// Width in pixels of one line of UI text at sizePx (the software renderer's font; every window
// backend paints with it, so layouts measure with this).
float measureUiText(std::string_view utf8, float sizePx);

// Name of the default window backend ("win32", "x11", "none").
const char* defaultBackendName();

// ---- offscreen rendering ----------------------------------------------------------------------
// RGBA8, straight (non-premultiplied) alpha, row-major, top-down.
struct PixelBuffer {
  int width = 0, height = 0;
  std::vector<uint8_t> rgba;
  uint32_t pixel(int x, int y) const;  // 0xRRGGBBAA
  uint32_t checksum() const;           // CRC32 over width, height and pixels
};

// Renders `paint` into an offscreen surface. Backend::Raster is portable and deterministic
// (snapshot tests); Backend::Software is the anti-aliased renderer every window uses (screenshots).
// Auto = Raster.
bool renderOffscreen(Backend backend, int width, int height, float scale, const std::function<void(IRenderer&)>& paint,
                     PixelBuffer& out, std::string* error = nullptr);

}  // namespace sq::ui
