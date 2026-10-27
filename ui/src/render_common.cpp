// Backend-neutral pieces of the UI layer: default line/arc drawing, colors, pixel buffers,
// accessibility lookup, the Image container and a portable (stored-deflate) PNG encoder.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "sq/platform.h"
#include "sq/ui/accessibility.hpp"
#include "sq/ui/image.hpp"
#include "sq/ui/ui.hpp"

namespace sq::ui {

namespace {
uint8_t to8(float v) { return static_cast<uint8_t>(std::lround((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0f)); }
constexpr float kPi = 3.14159265358979f;
}  // namespace

uint32_t Color::toRgba() const {
  return (uint32_t{to8(r)} << 24) | (uint32_t{to8(g)} << 16) | (uint32_t{to8(b)} << 8) | uint32_t{to8(a)};
}

// Default line: a chain of small squares along the segment. Backends with native line support
// override this; the default keeps a minimal backend (fillRect only) usable.
void IRenderer::drawLine(Point a, Point b, Color c, float width) {
  float w = width < 1.0f ? 1.0f : width;
  float dx = b.x - a.x, dy = b.y - a.y;
  if (std::fabs(dx) < 0.01f) {  // vertical
    fillRect(Rect{a.x - w / 2, std::min(a.y, b.y), w, std::fabs(dy)}, c);
    return;
  }
  if (std::fabs(dy) < 0.01f) {  // horizontal
    fillRect(Rect{std::min(a.x, b.x), a.y - w / 2, std::fabs(dx), w}, c);
    return;
  }
  float len = std::sqrt(dx * dx + dy * dy);
  int steps = static_cast<int>(std::ceil(len / std::max(1.0f, w * 0.5f)));
  steps = std::clamp(steps, 1, 4096);
  for (int i = 0; i <= steps; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(steps);
    fillRect(Rect{a.x + dx * t - w / 2, a.y + dy * t - w / 2, w, w}, c);
  }
}

void IRenderer::drawArc(Point center, float radius, float startDeg, float sweepDeg, Color c, float width) {
  if (!(radius > 0) || !std::isfinite(sweepDeg) || !std::isfinite(startDeg)) return;
  int segs = std::clamp(static_cast<int>(std::fabs(sweepDeg) / 6.0f) + 1, 1, 120);
  auto at = [&](float deg) {
    float rad = (deg - 90.0f) * kPi / 180.0f;  // 0 deg = 12 o'clock, clockwise
    return Point{center.x + radius * std::cos(rad), center.y + radius * std::sin(rad)};
  };
  Point prev = at(startDeg);
  for (int i = 1; i <= segs; ++i) {
    Point p = at(startDeg + sweepDeg * static_cast<float>(i) / static_cast<float>(segs));
    drawLine(prev, p, c, width);
    prev = p;
  }
}

uint32_t PixelBuffer::pixel(int x, int y) const {
  if (x < 0 || y < 0 || x >= width || y >= height) return 0;
  size_t i = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
  if (i + 3 >= rgba.size()) return 0;
  return (uint32_t{rgba[i]} << 24) | (uint32_t{rgba[i + 1]} << 16) | (uint32_t{rgba[i + 2]} << 8) | uint32_t{rgba[i + 3]};
}

uint32_t PixelBuffer::checksum() const {
  int32_t dims[2] = {width, height};
  uint32_t c = sq_crc32(0, dims, sizeof dims);
  return sq_crc32(c, rgba.data(), rgba.size());
}


namespace {
void put32(std::string& s, uint32_t v) {
  s.push_back(static_cast<char>(v >> 24));
  s.push_back(static_cast<char>(v >> 16));
  s.push_back(static_cast<char>(v >> 8));
  s.push_back(static_cast<char>(v));
}
void chunk(std::string& out, const char* type, const std::string& data) {
  put32(out, static_cast<uint32_t>(data.size()));
  std::string td(type, 4);
  td += data;
  out += td;
  put32(out, sq_crc32(0, td.data(), td.size()));
}
}  // namespace

// Portable PNG writer: RGBA8, filter 0, zlib stream made of stored (uncompressed) deflate blocks.
// Larger than a compressed PNG but needs no codec and is valid everywhere.
bool encodePng(const PixelBuffer& px, std::string& out, std::string* error) {
  if (px.width <= 0 || px.height <= 0 || px.rgba.size() != static_cast<size_t>(px.width) * static_cast<size_t>(px.height) * 4) {
    if (error) *error = "empty or inconsistent pixel buffer";
    return false;
  }
  std::string raw;
  size_t row = static_cast<size_t>(px.width) * 4;
  raw.reserve((row + 1) * static_cast<size_t>(px.height));
  for (int y = 0; y < px.height; ++y) {
    raw.push_back('\0');
    raw.append(reinterpret_cast<const char*>(px.rgba.data()) + row * static_cast<size_t>(y), row);
  }
  std::string z;
  z.push_back(static_cast<char>(0x78));
  z.push_back(static_cast<char>(0x01));
  size_t pos = 0;
  do {
    size_t n = std::min<size_t>(65535, raw.size() - pos);
    bool last = pos + n >= raw.size();
    z.push_back(static_cast<char>(last ? 1 : 0));
    z.push_back(static_cast<char>(n & 0xFF));
    z.push_back(static_cast<char>(n >> 8));
    z.push_back(static_cast<char>(~n & 0xFF));
    z.push_back(static_cast<char>((~n >> 8) & 0xFF));
    z.append(raw, pos, n);
    pos += n;
  } while (pos < raw.size());
  uint32_t a = 1, b = 0;
  for (unsigned char ch : raw) {
    a = (a + ch) % 65521u;
    b = (b + a) % 65521u;
  }
  put32(z, (b << 16) | a);

  out.assign("\x89PNG\r\n\x1a\n", 8);
  std::string ihdr;
  put32(ihdr, static_cast<uint32_t>(px.width));
  put32(ihdr, static_cast<uint32_t>(px.height));
  ihdr += std::string("\x08\x06\x00\x00\x00", 5);  // 8 bit, RGBA, deflate, filter 0, no interlace
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", std::string());
  return true;
}

}  // namespace sq::ui
