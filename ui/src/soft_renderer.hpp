// Portable anti-aliased software renderer (Backend::Software): premultiplied BGRA8 target,
// coverage-accumulation polygon rasterizer (rects, rounded rects, ellipses, lines, arcs),
// rectangular clip stack, bilinear image blits and real text through stb_truetype with a glyph
// cache. Every window backend (Win32, X11) presents its pixels, so all platforms render the same
// image, and offscreen renders (screenshots, snapshot tests) match what the window shows.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "sq/ui/ui.hpp"

namespace sq::ui {

// Target pixels: premultiplied BGRA8 (B at the lowest address), `stride` bytes per row.
struct SoftSurface {
  uint8_t* pixels = nullptr;
  int width = 0, height = 0;
  size_t stride = 0;
};

std::unique_ptr<IRenderer> makeSoftRenderer(const SoftSurface& target, float scale);

// UI fonts: <exe dir>/fonts/Inter-*.otf, then common system fonts. Loaded once, process-wide.
// Returns false if no font could be loaded (text then draws nothing; measureText approximates).
bool softFontsAvailable();
// Width in pixels of one line of text at sizePx with the UI font (approximation without a font).
float softMeasureText(std::string_view utf8, float sizePx);

}  // namespace sq::ui
