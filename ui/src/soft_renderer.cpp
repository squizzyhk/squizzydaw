// Portable anti-aliased software renderer. See soft_renderer.hpp.
//
// Shapes become closed polygons (pixel coordinates) and go through one coverage-accumulation
// rasterizer: every edge adds its signed area to an accumulation row buffer, a running sum per row
// gives the exact coverage (non-zero winding, |sum| clamped to 1). Axis-aligned rectangles take a
// direct path with fractional edge coverage. Blending is premultiplied source-over.
#include "soft_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "sq/platform.h"
#include "sq/platform.hpp"
#include "sq/ui/image.hpp"
#include "stb_truetype.h"

namespace sq::ui {
namespace {

constexpr float kPi = 3.14159265358979f;

// ---------------------------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------------------------
struct Glyph {
  int w = 0, h = 0, xoff = 0, yoff = 0;
  std::vector<uint8_t> alpha;
};

struct Font {
  std::string data;
  stbtt_fontinfo info{};
  bool ok = false;
  float ascent = 0, descent = 0, lineGap = 0;  // font units
  int unitsPerEm = 1000;
  std::map<uint64_t, Glyph> cache;
  std::map<uint32_t, int> glyphIndex;

  bool load(std::string bytes) {
    data = std::move(bytes);
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    int off = stbtt_GetFontOffsetForIndex(p, 0);
    if (off < 0 || !stbtt_InitFont(&info, p, off)) return false;
    int a = 0, d = 0, g = 0;
    stbtt_GetFontVMetrics(&info, &a, &d, &g);
    ascent = static_cast<float>(a);
    descent = static_cast<float>(d);
    lineGap = static_cast<float>(g);
    float s = stbtt_ScaleForMappingEmToPixels(&info, 1000.0f);
    unitsPerEm = s > 0 ? static_cast<int>(std::lround(1000.0f / s)) : 1000;
    ok = true;
    return true;
  }
  float scaleFor(float px) const { return stbtt_ScaleForMappingEmToPixels(&info, px); }
  int glyph(uint32_t cp) {
    auto it = glyphIndex.find(cp);
    if (it != glyphIndex.end()) return it->second;
    int g = stbtt_FindGlyphIndex(&info, static_cast<int>(cp));
    glyphIndex[cp] = g;
    return g;
  }
  float advance(int g, float scale) {
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&info, g, &adv, &lsb);
    return static_cast<float>(adv) * scale;
  }
  float kern(int a, int b, float scale) { return static_cast<float>(stbtt_GetGlyphKernAdvance(&info, a, b)) * scale; }
  const Glyph& bitmap(int g, float sizePx, int subpixel) {
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(g)) << 32) |
                         (static_cast<uint64_t>(std::lround(sizePx * 4.0f)) << 4) | static_cast<uint64_t>(subpixel & 15);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 8192) cache.clear();
    Glyph gl;
    const float sc = scaleFor(sizePx);
    const float sx = static_cast<float>(subpixel) / 4.0f;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBoxSubpixel(&info, g, sc, sc, sx, 0.0f, &x0, &y0, &x1, &y1);
    gl.w = std::max(0, x1 - x0);
    gl.h = std::max(0, y1 - y0);
    gl.xoff = x0;
    gl.yoff = y0;
    if (gl.w > 0 && gl.h > 0 && gl.w < 2048 && gl.h < 2048) {
      gl.alpha.assign(static_cast<size_t>(gl.w) * static_cast<size_t>(gl.h), 0);
      stbtt_MakeGlyphBitmapSubpixel(&info, gl.alpha.data(), gl.w, gl.h, gl.w, sc, sc, sx, 0.0f, g);
    } else {
      gl.w = gl.h = 0;
    }
    return cache.emplace(key, std::move(gl)).first->second;
  }
};

struct FontSet {
  Font regular, bold;
  bool loaded = false;
  std::mutex mu;  // renderers on several threads (offscreen renders) share the cache
};

FontSet& fonts() {
  static FontSet fs;
  std::lock_guard<std::mutex> lock(fs.mu);
  if (fs.loaded) return fs;
  fs.loaded = true;
  std::string exeDir;
  {
    sq_buffer b{};
    if (sq_special_dir_get(SQ_DIR_EXE, &b) == SQ_OK && b.data) exeDir.assign(b.data, b.size);
    sq_buffer_free(&b);
    while (!exeDir.empty() && exeDir.back() == '\0') exeDir.pop_back();
  }
  auto tryLoad = [](Font& f, const std::vector<std::string>& paths) {
    for (const auto& p : paths) {
      if (p.empty()) continue;
      auto bytes = sq::readFile(p);
      if (bytes && !bytes->empty() && f.load(std::move(*bytes))) return true;
    }
    return false;
  };
  std::string envFont;
  {
    sq_buffer b{};
    if (sq_env_get("SQ_UI_FONT", &b) == SQ_OK && b.data) envFont.assign(b.data, b.size);
    sq_buffer_free(&b);
    while (!envFont.empty() && envFont.back() == '\0') envFont.pop_back();
  }
  const std::string fd = exeDir.empty() ? std::string("fonts/") : exeDir + "/fonts/";
  tryLoad(fs.regular, {envFont, fd + "Inter-Regular.otf", "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/opentype/inter/Inter-Regular.otf",
                       "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                       "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf"});
  if (!tryLoad(fs.bold, {fd + "Inter-SemiBold.otf", "C:/Windows/Fonts/seguisb.ttf", "/usr/share/fonts/opentype/inter/Inter-SemiBold.otf",
                         "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"}))
    fs.bold.ok = false;
  return fs;
}

uint32_t nextCodepoint(std::string_view s, size_t& i) {
  const auto c = static_cast<unsigned char>(s[i++]);
  if (c < 0x80) return c;
  int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
  uint32_t cp = c & (0x3F >> n);
  for (int k = 0; k < n && i < s.size(); ++k) {
    const auto cc = static_cast<unsigned char>(s[i]);
    if ((cc & 0xC0) != 0x80) break;
    cp = (cp << 6) | (cc & 0x3F);
    ++i;
  }
  return cp;
}

float measureWith(Font& f, std::string_view s, float sizePx) {
  if (!f.ok) return static_cast<float>(s.size()) * sizePx * 0.55f;
  const float sc = f.scaleFor(sizePx);
  float w = 0;
  int prev = -1;
  for (size_t i = 0; i < s.size();) {
    uint32_t cp = nextCodepoint(s, i);
    int g = f.glyph(cp);
    if (prev >= 0) w += f.kern(prev, g, sc);
    w += f.advance(g, sc);
    prev = g;
  }
  return w;
}

// ---------------------------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------------------------
using Contour = std::vector<Point>;

struct IRect {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool empty() const { return x1 <= x0 || y1 <= y0; }
};

class SoftRenderer final : public IRenderer {
 public:
  SoftRenderer(const SoftSurface& t, float scale) : t_(t), scale_(scale > 0 ? scale : 1.0f) {
    clip_.push_back(IRect{0, 0, t.width, t.height});
  }
  const char* backendName() const override { return "software"; }
  Size size() const override { return Size{t_.width, t_.height}; }
  float scale() const override { return scale_; }

  void clear(Color c) override {
    uint8_t px[4];
    premul(c, 1.0f, px);
    for (int y = 0; y < t_.height; ++y) {
      uint8_t* row = t_.pixels + static_cast<size_t>(y) * t_.stride;
      for (int x = 0; x < t_.width; ++x) std::memcpy(row + x * 4, px, 4);
    }
  }

  void fillRect(const Rect& r, Color c) override {
    if (!(r.w > 0) || !(r.h > 0) || c.a <= 0) return;
    const IRect& cl = clip_.back();
    const int x0 = std::max(cl.x0, static_cast<int>(std::floor(r.x)));
    const int y0 = std::max(cl.y0, static_cast<int>(std::floor(r.y)));
    const int x1 = std::min(cl.x1, static_cast<int>(std::ceil(r.x + r.w)));
    const int y1 = std::min(cl.y1, static_cast<int>(std::ceil(r.y + r.h)));
    if (x1 <= x0 || y1 <= y0) return;
    auto cov = [](float a0, float a1, int p) {
      const float lo = std::max(a0, static_cast<float>(p));
      const float hi = std::min(a1, static_cast<float>(p + 1));
      return hi > lo ? hi - lo : 0.0f;
    };
    for (int y = y0; y < y1; ++y) {
      const float cy = cov(r.y, r.y + r.h, y);
      if (cy <= 0) continue;
      uint8_t* row = t_.pixels + static_cast<size_t>(y) * t_.stride;
      const bool solidRow = cy >= 0.999f;
      for (int x = x0; x < x1; ++x) {
        const float cx = (x > x0 && x < x1 - 1) ? 1.0f : cov(r.x, r.x + r.w, x);
        const float a = solidRow ? cx : cx * cy;
        blend(row + x * 4, c, a);
      }
    }
  }

  void strokeRect(const Rect& r, Color c, float width) override {
    const float w = width < 1 ? 1 : width;
    if (r.w <= 2 * w || r.h <= 2 * w) {
      fillRect(r, c);
      return;
    }
    fillRect(Rect{r.x, r.y, r.w, w}, c);
    fillRect(Rect{r.x, r.y + r.h - w, r.w, w}, c);
    fillRect(Rect{r.x, r.y + w, w, r.h - 2 * w}, c);
    fillRect(Rect{r.x + r.w - w, r.y + w, w, r.h - 2 * w}, c);
  }

  void fillRoundRect(const Rect& r, float radius, Color c) override {
    if (radius <= 0.5f) {
      fillRect(r, c);
      return;
    }
    std::vector<Contour> p(1);
    roundRect(p[0], r, radius, false);
    fillPolys(p, c);
  }
  void strokeRoundRect(const Rect& r, float radius, Color c, float width) override {
    const float w = std::max(width, 0.5f);
    std::vector<Contour> p(2);
    roundRect(p[0], Rect{r.x, r.y, r.w, r.h}, radius, false);
    const Rect in = r.inset(w);
    if (in.w <= 0 || in.h <= 0) {
      p.pop_back();
    } else {
      roundRect(p[1], in, std::max(0.0f, radius - w), true);
    }
    fillPolys(p, c);
  }
  void fillEllipse(const Rect& r, Color c) override {
    std::vector<Contour> p(1);
    ellipse(p[0], r, false);
    fillPolys(p, c);
  }
  void strokeEllipse(const Rect& r, Color c, float width) override {
    const float w = std::max(width, 0.5f);
    std::vector<Contour> p(2);
    ellipse(p[0], Rect{r.x - w / 2, r.y - w / 2, r.w + w, r.h + w}, false);
    ellipse(p[1], Rect{r.x + w / 2, r.y + w / 2, r.w - w, r.h - w}, true);
    fillPolys(p, c);
  }
  void drawLine(Point a, Point b, Color c, float width) override {
    float w = width;
    if (w < 1.0f) {
      c.a *= std::max(w, 0.1f);
      w = 1.0f;
    }
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    if (std::fabs(dx) < 1e-4f || std::fabs(dy) < 1e-4f) {  // axis aligned: exact rect
      fillRect(Rect{std::min(a.x, b.x) - (std::fabs(dy) < 1e-4f ? 0 : w / 2), std::min(a.y, b.y) - (std::fabs(dx) < 1e-4f ? 0 : w / 2),
                    std::fabs(dx) < 1e-4f ? w : std::fabs(dx), std::fabs(dy) < 1e-4f ? w : std::fabs(dy)},
               c);
      return;
    }
    const float nx = -dy / len * w / 2, ny = dx / len * w / 2;
    std::vector<Contour> p(1);
    p[0] = {Point{a.x + nx, a.y + ny}, Point{b.x + nx, b.y + ny}, Point{b.x - nx, b.y - ny}, Point{a.x - nx, a.y - ny}};
    fillPolys(p, c);
  }
  void drawArc(Point center, float radius, float startDeg, float sweepDeg, Color c, float width) override {
    if (!(radius > 0) || !std::isfinite(sweepDeg) || !std::isfinite(startDeg) || std::fabs(sweepDeg) < 0.01f) return;
    sweepDeg = std::clamp(sweepDeg, -360.0f, 360.0f);
    const float w = std::max(width, 0.5f);
    const float ro = radius + w / 2, ri = std::max(0.0f, radius - w / 2);
    const int segs = std::clamp(static_cast<int>(std::fabs(sweepDeg) / 360.0f * std::max(12.0f, ro * 1.5f)) + 2, 2, 400);
    std::vector<Contour> p(1);
    auto at = [&](float deg, float rr) {
      const float rad = deg * kPi / 180.0f;
      return Point{center.x + rr * std::sin(rad), center.y - rr * std::cos(rad)};
    };
    for (int i = 0; i <= segs; ++i) p[0].push_back(at(startDeg + sweepDeg * static_cast<float>(i) / static_cast<float>(segs), ro));
    for (int i = segs; i >= 0; --i) p[0].push_back(at(startDeg + sweepDeg * static_cast<float>(i) / static_cast<float>(segs), ri));
    fillPolys(p, c);
  }

  void fillPolygon(const Point* pts, size_t count, Color c) override {
    if (!pts || count < 3) return;
    std::vector<Contour> p(1);
    p[0].assign(pts, pts + count);
    fillPolys(p, c);
  }
  void strokePolyline(const Point* pts, size_t count, Color c, float width) override {
    if (!pts || count < 2) return;
    // One quad per segment plus round-ish joins (small squares) keeps it simple and gap-free.
    for (size_t i = 1; i < count; ++i) drawLine(pts[i - 1], pts[i], c, width);
  }

  void pushClip(const Rect& r) override {
    IRect n{static_cast<int>(std::floor(r.x)), static_cast<int>(std::floor(r.y)), static_cast<int>(std::ceil(r.x + r.w)),
            static_cast<int>(std::ceil(r.y + r.h))};
    const IRect& c = clip_.back();
    n.x0 = std::max(n.x0, c.x0);
    n.y0 = std::max(n.y0, c.y0);
    n.x1 = std::min(n.x1, c.x1);
    n.y1 = std::min(n.y1, c.y1);
    if (n.x1 < n.x0) n.x1 = n.x0;
    if (n.y1 < n.y0) n.y1 = n.y0;
    clip_.push_back(n);
  }
  void popClip() override {
    if (clip_.size() > 1) clip_.pop_back();
  }

  float measureText(std::string_view utf8, float sizePx) override { return softMeasureText(utf8, sizePx); }

  void drawText(std::string_view utf8, const Rect& r, Color c, float sizePx, TextAlign align) override {
    drawTextStyled(utf8, r, c, sizePx, align, false);
  }

  void drawTextStyled(std::string_view utf8, const Rect& r, Color c, float sizePx, TextAlign align, bool bold) override {
    if (utf8.empty() || !(sizePx > 0.5f) || c.a <= 0 || !(r.w > 0) || !(r.h > 0)) return;
    FontSet& fs = fonts();
    std::lock_guard<std::mutex> lock(fs.mu);
    Font& f = (bold && fs.bold.ok) ? fs.bold : fs.regular;
    if (!f.ok) return;
    const float sc = f.scaleFor(sizePx);
    const float ascent = f.ascent * sc, descent = -f.descent * sc;
    const float lineH = (f.ascent - f.descent + f.lineGap) * sc;
    // Lines: explicit newlines, then word wrap when the rect has room for more than one line.
    std::vector<std::string> lines;
    const bool multi = r.h >= 2.0f * lineH;
    size_t start = 0;
    while (start <= utf8.size()) {
      size_t nl = utf8.find('\n', start);
      std::string_view para = utf8.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
      if (multi && measureWith(f, para, sizePx) > r.w) wrap(f, para, sizePx, r.w, lines);
      else lines.emplace_back(para);
      if (nl == std::string_view::npos) break;
      start = nl + 1;
    }
    if (!multi && lines.size() > 1) lines.resize(1);
    if (!multi && !lines.empty() && measureWith(f, lines[0], sizePx) > r.w + 0.5f) lines[0] = ellipsize(f, lines[0], sizePx, r.w);
    pushClip(Rect{r.x, r.y - 1, r.w, r.h + 2});
    const float total = lineH * static_cast<float>(lines.size());
    float baseline = (lines.size() == 1 || total <= r.h) && lines.size() == 1 ? r.y + (r.h - (ascent + descent)) / 2 + ascent
                                                                             : r.y + ascent;
    baseline = std::round(baseline);
    for (const auto& line : lines) {
      const float w = measureWith(f, line, sizePx);
      float x = align == TextAlign::Center ? r.x + (r.w - w) / 2 : align == TextAlign::Right ? r.x + r.w - w : r.x;
      drawLineGlyphs(f, line, x, baseline, sizePx, sc, c);
      baseline += std::round(lineH);
      if (baseline - ascent > r.y + r.h) break;
    }
    popClip();
  }

  void drawImage(const Image& img, const Rect& dst, float opacity) override {
    if (img.width() <= 0 || img.height() <= 0 || !(dst.w > 0) || !(dst.h > 0) || opacity <= 0) return;
    const IRect& cl = clip_.back();
    const int x0 = std::max(cl.x0, static_cast<int>(std::floor(dst.x)));
    const int y0 = std::max(cl.y0, static_cast<int>(std::floor(dst.y)));
    const int x1 = std::min(cl.x1, static_cast<int>(std::ceil(dst.x + dst.w)));
    const int y1 = std::min(cl.y1, static_cast<int>(std::ceil(dst.y + dst.h)));
    const float sx = static_cast<float>(img.width()) / dst.w, sy = static_cast<float>(img.height()) / dst.h;
    const uint8_t* src = img.pixels();
    const size_t sstride = img.stride();
    const int op = static_cast<int>(std::lround(std::clamp(opacity, 0.0f, 1.0f) * 256.0f));
    for (int y = y0; y < y1; ++y) {
      float fy = (static_cast<float>(y) + 0.5f - dst.y) * sy - 0.5f;
      fy = std::clamp(fy, 0.0f, static_cast<float>(img.height() - 1));
      const int iy = static_cast<int>(fy);
      const int iy1 = std::min(iy + 1, img.height() - 1);
      const int wy = static_cast<int>((fy - static_cast<float>(iy)) * 256.0f);
      uint8_t* row = t_.pixels + static_cast<size_t>(y) * t_.stride;
      for (int x = x0; x < x1; ++x) {
        float fx = (static_cast<float>(x) + 0.5f - dst.x) * sx - 0.5f;
        fx = std::clamp(fx, 0.0f, static_cast<float>(img.width() - 1));
        const int ix = static_cast<int>(fx);
        const int ix1 = std::min(ix + 1, img.width() - 1);
        const int wx = static_cast<int>((fx - static_cast<float>(ix)) * 256.0f);
        const uint8_t* p00 = src + static_cast<size_t>(iy) * sstride + static_cast<size_t>(ix) * 4;
        const uint8_t* p01 = src + static_cast<size_t>(iy) * sstride + static_cast<size_t>(ix1) * 4;
        const uint8_t* p10 = src + static_cast<size_t>(iy1) * sstride + static_cast<size_t>(ix) * 4;
        const uint8_t* p11 = src + static_cast<size_t>(iy1) * sstride + static_cast<size_t>(ix1) * 4;
        int s[4];
        for (int k = 0; k < 4; ++k) {
          const int top = p00[k] * (256 - wx) + p01[k] * wx;
          const int bot = p10[k] * (256 - wx) + p11[k] * wx;
          s[k] = ((top * (256 - wy) + bot * wy) >> 16) * op >> 8;
        }
        uint8_t* d = row + x * 4;
        const int inv = 255 - s[3];
        for (int k = 0; k < 4; ++k) d[k] = static_cast<uint8_t>(std::min(255, s[k] + (d[k] * inv + 127) / 255));
      }
    }
  }

 private:
  static void premul(Color c, float cov, uint8_t out[4]) {
    const float a = std::clamp(c.a * cov, 0.0f, 1.0f);
    out[0] = static_cast<uint8_t>(std::lround(std::clamp(c.b, 0.0f, 1.0f) * a * 255.0f));
    out[1] = static_cast<uint8_t>(std::lround(std::clamp(c.g, 0.0f, 1.0f) * a * 255.0f));
    out[2] = static_cast<uint8_t>(std::lround(std::clamp(c.r, 0.0f, 1.0f) * a * 255.0f));
    out[3] = static_cast<uint8_t>(std::lround(a * 255.0f));
  }
  static void blend(uint8_t* d, Color c, float cov) {
    if (cov <= 0.002f) return;
    uint8_t s[4];
    premul(c, cov > 1 ? 1 : cov, s);
    if (s[3] == 255) {
      std::memcpy(d, s, 4);
      return;
    }
    const int inv = 255 - s[3];
    d[0] = static_cast<uint8_t>(s[0] + (d[0] * inv + 127) / 255);
    d[1] = static_cast<uint8_t>(s[1] + (d[1] * inv + 127) / 255);
    d[2] = static_cast<uint8_t>(s[2] + (d[2] * inv + 127) / 255);
    d[3] = static_cast<uint8_t>(s[3] + (d[3] * inv + 127) / 255);
  }

  static void roundRect(Contour& out, const Rect& r, float radius, bool reverse) {
    const float rad = std::clamp(radius, 0.0f, std::min(r.w, r.h) / 2);
    const int n = std::clamp(static_cast<int>(rad * 0.8f) + 2, 2, 32);
    auto corner = [&](float cx, float cy, float a0) {
      for (int i = 0; i <= n; ++i) {
        const float a = (a0 + 90.0f * static_cast<float>(i) / static_cast<float>(n)) * kPi / 180.0f;
        out.push_back(Point{cx + rad * std::cos(a), cy + rad * std::sin(a)});
      }
    };
    corner(r.x + r.w - rad, r.y + rad, -90.0f);
    corner(r.x + r.w - rad, r.y + r.h - rad, 0.0f);
    corner(r.x + rad, r.y + r.h - rad, 90.0f);
    corner(r.x + rad, r.y + rad, 180.0f);
    if (reverse) std::reverse(out.begin(), out.end());
  }
  static void ellipse(Contour& out, const Rect& r, bool reverse) {
    if (r.w <= 0 || r.h <= 0) return;
    const float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    const int n = std::clamp(static_cast<int>((r.w + r.h) * 0.9f) + 8, 12, 400);
    for (int i = 0; i < n; ++i) {
      const float a = 2 * kPi * static_cast<float>(i) / static_cast<float>(n);
      out.push_back(Point{cx + r.w / 2 * std::cos(a), cy + r.h / 2 * std::sin(a)});
    }
    if (reverse) std::reverse(out.begin(), out.end());
  }

  // Accumulates one edge (coordinates relative to the raster box, x already within [0, w]).
  void accLine(float x0, float y0, float x1, float y1, int w, int h) {
    if (std::fabs(y0 - y1) <= 1e-6f) return;
    float dir = 1.0f;
    if (y0 > y1) {
      std::swap(x0, x1);
      std::swap(y0, y1);
      dir = -1.0f;
    }
    const float dxdy = (x1 - x0) / (y1 - y0);
    float x = x0;
    int ys = static_cast<int>(std::floor(y0));
    if (y0 < 0) {
      x -= y0 * dxdy;
      ys = 0;
    }
    const int ye = std::min(h, static_cast<int>(std::ceil(y1)));
    const int W = w + 2;
    for (int y = ys; y < ye; ++y) {
      float* line = acc_.data() + static_cast<size_t>(y) * W;
      const float dy = std::min(static_cast<float>(y + 1), y1) - std::max(static_cast<float>(y), y0);
      const float xnext = x + dxdy * dy;
      const float d = dy * dir;
      float xa = x, xb = xnext;
      if (xa > xb) std::swap(xa, xb);
      xa = std::clamp(xa, 0.0f, static_cast<float>(w));
      xb = std::clamp(xb, 0.0f, static_cast<float>(w));
      const float xaf = std::floor(xa);
      const int xai = static_cast<int>(xaf);
      const float xbc = std::ceil(xb);
      const int xbi = static_cast<int>(xbc);
      if (xbi <= xai + 1) {
        const float xm = 0.5f * (xa + xb) - xaf;
        line[xai] += d - d * xm;
        line[xai + 1] += d * xm;
      } else {
        const float s = 1.0f / (xb - xa);
        const float x0f = xa - xaf;
        const float a0 = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
        const float x1f = xb - xbc + 1.0f;
        const float am = 0.5f * s * x1f * x1f;
        line[xai] += d * a0;
        if (xbi == xai + 2) {
          line[xai + 1] += d * (1.0f - a0 - am);
        } else {
          const float a1 = s * (1.5f - x0f);
          line[xai + 1] += d * (a1 - a0);
          for (int xi = xai + 2; xi < xbi - 1; ++xi) line[xi] += d * s;
          const float a2 = a1 + static_cast<float>(xbi - xai - 3) * s;
          line[xbi - 1] += d * (1.0f - a2 - am);
        }
        line[xbi] += d * am;
      }
      x = xnext;
    }
  }

  // Splits an edge at the vertical box boundaries x=0 / x=w; the outside parts become vertical
  // segments on the boundary (they still contribute winding).
  void addEdge(Point a, Point b, int w, int h) {
    const float W = static_cast<float>(w);
    int n = 0;
    float px[4], py[4];
    px[n] = a.x;
    py[n++] = a.y;
    auto cross = [&](float bound) {
      if ((a.x < bound) != (b.x < bound) && a.x != b.x) {
        const float t = (bound - a.x) / (b.x - a.x);
        px[n] = bound;
        py[n++] = a.y + (b.y - a.y) * t;
      }
    };
    if (a.x <= b.x) {
      cross(0);
      cross(W);
    } else {
      cross(W);
      cross(0);
    }
    px[n] = b.x;
    py[n++] = b.y;
    for (int i = 0; i + 1 < n; ++i) accLine(std::clamp(px[i], 0.0f, W), py[i], std::clamp(px[i + 1], 0.0f, W), py[i + 1], w, h);
  }

  void fillPolys(const std::vector<Contour>& polys, Color c) {
    if (c.a <= 0) return;
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (const auto& p : polys)
      for (const auto& pt : p) {
        if (!std::isfinite(pt.x) || !std::isfinite(pt.y)) return;
        minx = std::min(minx, pt.x);
        miny = std::min(miny, pt.y);
        maxx = std::max(maxx, pt.x);
        maxy = std::max(maxy, pt.y);
      }
    if (minx > maxx) return;
    const IRect& cl = clip_.back();
    IRect box{std::max(cl.x0, static_cast<int>(std::floor(minx))), std::max(cl.y0, static_cast<int>(std::floor(miny))),
              std::min(cl.x1, static_cast<int>(std::ceil(maxx)) + 1), std::min(cl.y1, static_cast<int>(std::ceil(maxy)) + 1)};
    if (box.empty()) return;
    const int w = box.x1 - box.x0, h = box.y1 - box.y0;
    acc_.assign(static_cast<size_t>(w + 2) * static_cast<size_t>(h), 0.0f);
    const float ox = static_cast<float>(box.x0), oy = static_cast<float>(box.y0);
    for (const auto& p : polys) {
      if (p.size() < 3) continue;
      for (size_t i = 0; i < p.size(); ++i) {
        const Point& a = p[i];
        const Point& b = p[(i + 1) % p.size()];
        addEdge(Point{a.x - ox, a.y - oy}, Point{b.x - ox, b.y - oy}, w, h);
      }
    }
    for (int y = 0; y < h; ++y) {
      const float* line = acc_.data() + static_cast<size_t>(y) * (w + 2);
      uint8_t* row = t_.pixels + static_cast<size_t>(y + box.y0) * t_.stride + static_cast<size_t>(box.x0) * 4;
      float sum = 0;
      for (int x = 0; x < w; ++x) {
        sum += line[x];
        const float cov = std::min(1.0f, std::fabs(sum));
        if (cov > 0.002f) blend(row + x * 4, c, cov);
      }
    }
  }

  void wrap(Font& f, std::string_view para, float sizePx, float width, std::vector<std::string>& lines) {
    std::string cur;
    size_t i = 0;
    while (i < para.size()) {
      size_t sp = para.find(' ', i);
      std::string_view word = para.substr(i, sp == std::string_view::npos ? std::string_view::npos : sp - i);
      std::string cand = cur.empty() ? std::string(word) : cur + " " + std::string(word);
      if (!cur.empty() && measureWith(f, cand, sizePx) > width) {
        lines.push_back(cur);
        cur = std::string(word);
      } else {
        cur = std::move(cand);
      }
      if (sp == std::string_view::npos) break;
      i = sp + 1;
    }
    lines.push_back(cur);
  }
  std::string ellipsize(Font& f, const std::string& s, float sizePx, float width) {
    static const std::string kEll = "\xE2\x80\xA6";
    const float ew = measureWith(f, kEll, sizePx);
    if (ew > width) return std::string();
    std::vector<size_t> cuts;  // codepoint boundaries
    for (size_t i = 0; i < s.size();) {
      nextCodepoint(s, i);
      cuts.push_back(i);
    }
    size_t lo = 0, hi = cuts.size();
    while (lo < hi) {  // largest prefix that fits
      const size_t mid = (lo + hi + 1) / 2;
      if (measureWith(f, std::string_view(s).substr(0, cuts[mid - 1]), sizePx) + ew <= width) lo = mid;
      else hi = mid - 1;
    }
    std::string out = lo ? s.substr(0, cuts[lo - 1]) : std::string();
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out + kEll;
  }

  void drawLineGlyphs(Font& f, std::string_view s, float x, float baseline, float sizePx, float sc, Color c) {
    const IRect& cl = clip_.back();
    int prev = -1;
    const int by = static_cast<int>(baseline);
    for (size_t i = 0; i < s.size();) {
      const uint32_t cp = nextCodepoint(s, i);
      const int g = f.glyph(cp);
      if (prev >= 0) x += f.kern(prev, g, sc);
      prev = g;
      const float fx = std::floor(x);
      const int sub = std::clamp(static_cast<int>((x - fx) * 4.0f), 0, 3);
      const Glyph& gl = f.bitmap(g, sizePx, sub);
      const int gx = static_cast<int>(fx) + gl.xoff, gy = by + gl.yoff;
      for (int yy = 0; yy < gl.h; ++yy) {
        const int py = gy + yy;
        if (py < cl.y0 || py >= cl.y1) continue;
        uint8_t* row = t_.pixels + static_cast<size_t>(py) * t_.stride;
        const uint8_t* a = gl.alpha.data() + static_cast<size_t>(yy) * gl.w;
        for (int xx = 0; xx < gl.w; ++xx) {
          const int px = gx + xx;
          if (px < cl.x0 || px >= cl.x1 || !a[xx]) continue;
          blend(row + px * 4, c, static_cast<float>(a[xx]) / 255.0f);
        }
      }
      x += f.advance(g, sc);
    }
  }

  SoftSurface t_;
  float scale_;
  std::vector<IRect> clip_;
  std::vector<float> acc_;
};

}  // namespace

std::unique_ptr<IRenderer> makeSoftRenderer(const SoftSurface& target, float scale) { return std::make_unique<SoftRenderer>(target, scale); }

bool softFontsAvailable() { return fonts().regular.ok; }

float measureUiText(std::string_view utf8, float sizePx) { return softMeasureText(utf8, sizePx); }

float softMeasureText(std::string_view utf8, float sizePx) {
  FontSet& fs = fonts();
  std::lock_guard<std::mutex> lock(fs.mu);
  return measureWith(fs.regular, utf8, sizePx);
}

}  // namespace sq::ui
