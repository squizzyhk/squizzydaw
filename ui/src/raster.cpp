// Portable software rasterizer (Backend::Raster): deterministic offscreen rendering for snapshot
// tests and headless previews. Straight-alpha RGBA8 target, source-over blending, no
// anti-aliasing. Text is approximated by one filled cell per glyph (no font rasterizer in the
// portable backend yet); the native window backends draw real text.
#include <algorithm>
#include <cmath>
#include <vector>

#include "sq/ui/image.hpp"
#include "sq/ui/ui.hpp"

namespace sq::ui {
namespace {

class RasterRenderer final : public IRenderer {
 public:
  RasterRenderer(PixelBuffer& pb, float scale) : pb_(pb), scale_(scale > 0 ? scale : 1.0f) {
    clip_.push_back(Rect{0, 0, static_cast<float>(pb.width), static_cast<float>(pb.height)});
  }
  const char* backendName() const override { return "raster"; }
  Size size() const override { return Size{pb_.width, pb_.height}; }
  float scale() const override { return scale_; }

  void clear(Color c) override {
    for (size_t i = 0; i + 3 < pb_.rgba.size(); i += 4) {
      pb_.rgba[i] = to8(c.r);
      pb_.rgba[i + 1] = to8(c.g);
      pb_.rgba[i + 2] = to8(c.b);
      pb_.rgba[i + 3] = to8(c.a);
    }
  }
  void fillRect(const Rect& r, Color c) override {
    const Rect& cl = clip_.back();
    int x0 = std::max(static_cast<int>(std::lround(r.x)), static_cast<int>(std::lround(cl.x)));
    int y0 = std::max(static_cast<int>(std::lround(r.y)), static_cast<int>(std::lround(cl.y)));
    int x1 = std::min(static_cast<int>(std::lround(r.x + r.w)), static_cast<int>(std::lround(cl.x + cl.w)));
    int y1 = std::min(static_cast<int>(std::lround(r.y + r.h)), static_cast<int>(std::lround(cl.y + cl.h)));
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) blend(x, y, c);
  }
  void strokeRect(const Rect& r, Color c, float width) override {
    float w = width < 1 ? 1 : width;
    fillRect(Rect{r.x, r.y, r.w, w}, c);
    fillRect(Rect{r.x, r.y + r.h - w, r.w, w}, c);
    fillRect(Rect{r.x, r.y + w, w, r.h - 2 * w}, c);
    fillRect(Rect{r.x + r.w - w, r.y + w, w, r.h - 2 * w}, c);
  }
  void fillEllipse(const Rect& r, Color c) override {
    float cx = r.x + r.w / 2, cy = r.y + r.h / 2, rx = r.w / 2, ry = r.h / 2;
    if (rx <= 0 || ry <= 0) return;
    for (int y = static_cast<int>(r.y); y < static_cast<int>(std::ceil(r.y + r.h)); ++y) {
      float ny = (static_cast<float>(y) + 0.5f - cy) / ry;
      if (ny * ny > 1) continue;
      float half = rx * std::sqrt(1 - ny * ny);
      fillRect(Rect{cx - half, static_cast<float>(y), 2 * half, 1}, c);
    }
  }
  void drawText(std::string_view utf8, const Rect& r, Color c, float sizePx, TextAlign align) override {
    float cell = sizePx * 0.55f;
    size_t glyphs = 0;
    for (unsigned char ch : utf8)
      if ((ch & 0xC0) != 0x80) ++glyphs;
    float total = cell * static_cast<float>(glyphs);
    float x = align == TextAlign::Center ? r.x + (r.w - total) / 2 : align == TextAlign::Right ? r.x + r.w - total : r.x;
    float y = r.y + std::max(0.0f, (std::min(r.h, sizePx * 1.3f) - sizePx * 0.7f) / 2);
    pushClip(r);
    for (size_t i = 0, g = 0; i < utf8.size(); ++i) {
      auto ch = static_cast<unsigned char>(utf8[i]);
      if ((ch & 0xC0) == 0x80) continue;
      if (ch != ' ') fillRect(Rect{x + cell * static_cast<float>(g) + cell * 0.1f, y, cell * 0.8f, sizePx * 0.7f}, c.withAlpha(c.a * 0.8f));
      ++g;
    }
    popClip();
  }
  void drawImage(const Image& img, const Rect& dst, float opacity) override {
    if (img.width() <= 0 || img.height() <= 0 || dst.w <= 0 || dst.h <= 0) return;
    for (int y = 0; y < static_cast<int>(dst.h); ++y)
      for (int x = 0; x < static_cast<int>(dst.w); ++x) {
        int sx = std::min(img.width() - 1, static_cast<int>(static_cast<float>(x) * static_cast<float>(img.width()) / dst.w));
        int sy = std::min(img.height() - 1, static_cast<int>(static_cast<float>(y) * static_cast<float>(img.height()) / dst.h));
        const uint8_t* p = img.pixels() + static_cast<size_t>(sy) * img.stride() + static_cast<size_t>(sx) * 4;
        float a = p[3] / 255.0f;
        if (a <= 0) continue;
        Color c{p[2] / 255.0f / a, p[1] / 255.0f / a, p[0] / 255.0f / a, a * opacity};
        fillRect(Rect{dst.x + static_cast<float>(x), dst.y + static_cast<float>(y), 1, 1}, c);
      }
  }
  void pushClip(const Rect& r) override {
    const Rect& c = clip_.back();
    float x0 = std::max(c.x, r.x), y0 = std::max(c.y, r.y);
    float x1 = std::min(c.right(), r.right()), y1 = std::min(c.bottom(), r.bottom());
    clip_.push_back(Rect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)});
  }
  void popClip() override {
    if (clip_.size() > 1) clip_.pop_back();
  }

 private:
  static uint8_t to8(float v) { return static_cast<uint8_t>(std::lround((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0f)); }
  void blend(int x, int y, Color c) {
    if (x < 0 || y < 0 || x >= pb_.width || y >= pb_.height) return;
    uint8_t* p = &pb_.rgba[(static_cast<size_t>(y) * static_cast<size_t>(pb_.width) + static_cast<size_t>(x)) * 4];
    float sa = std::clamp(c.a, 0.0f, 1.0f);
    float da = p[3] / 255.0f;
    float oa = sa + da * (1 - sa);
    auto mix = [&](float s, uint8_t d) {
      if (oa <= 0) return uint8_t{0};
      return to8((s * sa + (d / 255.0f) * da * (1 - sa)) / oa);
    };
    p[0] = mix(c.r, p[0]);
    p[1] = mix(c.g, p[1]);
    p[2] = mix(c.b, p[2]);
    p[3] = to8(oa);
  }
  PixelBuffer& pb_;
  float scale_;
  std::vector<Rect> clip_;
};

}  // namespace

bool renderOffscreen(Backend backend, int width, int height, float scale, const std::function<void(IRenderer&)>& paint, PixelBuffer& out,
                     std::string* error) {
  if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
    if (error) *error = "invalid size";
    return false;
  }
  if (backend != Backend::Raster && backend != Backend::Auto) {
    if (error) *error = "only the raster backend renders offscreen in this build";
    return false;
  }
  out.width = width;
  out.height = height;
  out.rgba.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
  RasterRenderer r(out, scale);
  if (paint) paint(r);
  return true;
}

}  // namespace sq::ui
