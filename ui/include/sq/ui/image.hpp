// Squizzy Studio UI - decoded bitmap images (theme assets). Backend-neutral.
// Pixels are BGRA8 premultiplied (the native layout of D2D, GDI DIBs and WIC PBGRA).
//
// Decoders:
//   * WIC on Windows (PNG, JPEG, BMP, GIF, TIFF and whatever codecs the OS has) - decodeImageWic.
//   * Portable PNG decoder everywhere (all PNG color types and bit depths, Adam7, tRNS; inflate via
//     the vendored miniz) - decodeImagePortable.
// decodeImage() uses WIC where available and falls back to the portable decoder (Windows without
// a usable WIC, and every other OS). Decoding is bounded: at most 16384 px per side and
// kMaxImagePixels in total, and inflate never writes past the size IHDR implies. PNG input is
// always preflighted first (pngPreflight): chunk CRCs, layout, and a streaming count proving the
// image data inflates to exactly what IHDR implies - so nothing sized by the header is allocated
// for a truncated or forged file, and WIC (which zero-fills truncated PNG data) never sees one.
// Other formats go to WIC unchecked: it allocates the frame size the header claims (bounded by
// kMaxImagePixels) before decoding.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sq/ui/ui.hpp"

namespace sq::ui {

class Image {
 public:
  Image(int w, int h, std::vector<uint8_t> bgraPremul, float density = 1.0f);
  int width() const { return width_; }
  int height() const { return height_; }
  // Pixel density of the asset variant this came from (1 = base, 2 = @2x ...).
  float density() const { return density_; }
  const uint8_t* pixels() const { return pixels_.data(); }
  size_t stride() const { return static_cast<size_t>(width_) * 4; }
  // Process-unique id; backends cache uploaded textures by it.
  uint64_t id() const { return id_; }
  // 0xRRGGBBAA of a pixel, un-premultiplied (0 outside the image). For tests and tools.
  uint32_t pixelRgba(int x, int y) const;

 private:
  int width_, height_;
  float density_;
  std::vector<uint8_t> pixels_;
  uint64_t id_;
};
using ImagePtr = std::shared_ptr<const Image>;

inline constexpr uint64_t kMaxImagePixels = 64ull * 1024 * 1024;

enum class ImageFormat { Unknown, Png, Jpeg, Bmp, Gif };
ImageFormat sniffImageFormat(const void* data, size_t size);

// Best available decoder (see above). nullptr + error on failure.
ImagePtr decodeImage(const void* data, size_t size, float density = 1.0f, std::string* error = nullptr);
// Full structural + data check of a PNG without decoding pixels (memory: the IDAT bytes + 32 KB).
bool pngPreflight(const void* data, size_t size, std::string* error = nullptr);
// Portable PNG decoder (every platform).
ImagePtr decodeImagePortable(const void* data, size_t size, float density = 1.0f, std::string* error = nullptr);
// Windows Imaging Component; nullptr + "WIC is not available" elsewhere.
ImagePtr decodeImageWic(const void* data, size_t size, float density = 1.0f, std::string* error = nullptr);
// Name of the decoder decodeImage prefers on this platform: "wic" or "portable".
const char* imageDecoderName();

// PNG encoder for snapshots and previews: portable, stored (uncompressed) deflate blocks.
bool encodePng(const PixelBuffer& pixels, std::string& out, std::string* error = nullptr);
// Portable compressed PNG (miniz deflate, adaptive per-row filters). Deterministic output.
bool encodePngCompressed(const PixelBuffer& pixels, std::string& out, std::string* error = nullptr);
// JPEG through WIC (quality 0..1). false + error where WIC is unavailable.
bool encodeJpegWic(const PixelBuffer& pixels, float quality, std::string& out, std::string* error = nullptr);

// Header probes (no decoding): true if `data` is that format and fills the pixel size.
bool pngSize(const void* data, size_t size, uint32_t* width, uint32_t* height);
bool jpegSize(const void* data, size_t size, uint32_t* width, uint32_t* height);
// PNG, JPEG, BMP or GIF.
bool imageSize(const void* data, size_t size, uint32_t* width, uint32_t* height);

}  // namespace sq::ui
