// ui/ image decoding: WIC (Windows) and the portable PNG decoder on generated images of every
// common layout (RGBA, filtered + compressed, interlaced, palette + tRNS, gray + alpha, 16-bit),
// JPEG through WIC, header probes and malformed input.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "miniz.h"
#include "sq/platform.h"
#include "sq/platform.hpp"
#include "sq/ui/image.hpp"
#include "sqtest.hpp"

using namespace sq::ui;

namespace {

PixelBuffer testPattern(int w, int h) {
  PixelBuffer pb;
  pb.width = w;
  pb.height = h;
  pb.rgba.resize(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      uint8_t* p = &pb.rgba[(static_cast<size_t>(y) * w + x) * 4];
      p[0] = static_cast<uint8_t>(x * 255 / (w - 1));
      p[1] = static_cast<uint8_t>(y * 255 / (h - 1));
      p[2] = static_cast<uint8_t>((x ^ y) & 0xFF);
      p[3] = static_cast<uint8_t>(x < w / 2 ? 255 : 128 + (y & 63));
    }
  return pb;
}

void put32(std::string& s, uint32_t v) {
  for (int i = 3; i >= 0; --i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void chunk(std::string& out, const char* type, const std::string& data) {
  put32(out, static_cast<uint32_t>(data.size()));
  std::string td(type, 4);
  td += data;
  out += td;
  put32(out, sq_crc32(0, td.data(), td.size()));
}
std::string zlib(const std::string& raw) {
  mz_ulong n = mz_compressBound(static_cast<mz_ulong>(raw.size()));
  std::string z(n, '\0');
  mz_compress2(reinterpret_cast<unsigned char*>(z.data()), &n, reinterpret_cast<const unsigned char*>(raw.data()),
               static_cast<mz_ulong>(raw.size()), 6);
  z.resize(n);
  return z;
}
// PNG from already-serialised scanlines (filter byte 0 each) + optional extra chunks.
std::string makePng(uint32_t w, uint32_t h, int depth, int ctype, int interlace, const std::string& raw,
                    const std::vector<std::pair<const char*, std::string>>& extra = {}) {
  std::string out("\x89PNG\r\n\x1a\n", 8), ihdr;
  put32(ihdr, w);
  put32(ihdr, h);
  ihdr.push_back(static_cast<char>(depth));
  ihdr.push_back(static_cast<char>(ctype));
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(static_cast<char>(interlace));
  chunk(out, "IHDR", ihdr);
  for (const auto& [t, d] : extra) chunk(out, t, d);
  chunk(out, "IDAT", zlib(raw));
  chunk(out, "IEND", "");
  return out;
}

// Straight RGBA8 -> Adam7 interlaced scanlines (filter 0).
std::string adam7(const PixelBuffer& pb) {
  static const int P[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
  std::string raw;
  for (const auto& p : P)
    for (int y = p[1]; y < pb.height; y += p[3]) {
      std::string row;
      for (int x = p[0]; x < pb.width; x += p[2]) row.append(reinterpret_cast<const char*>(&pb.rgba[(static_cast<size_t>(y) * pb.width + x) * 4]), 4);
      if (!row.empty()) raw += '\0' + row;
    }
  return raw;
}

// |a - b| per channel of two 0xRRGGBBAA values <= tol.
bool near(uint32_t a, uint32_t b, int tol) {
  for (int s = 0; s < 32; s += 8) {
    int d = static_cast<int>((a >> s) & 0xFF) - static_cast<int>((b >> s) & 0xFF);
    if (d < -tol || d > tol) return false;
  }
  return true;
}

uint32_t srcPixel(const PixelBuffer& pb, int x, int y) { return pb.pixel(x, y); }

bool sameImage(const Image& a, const Image& b) {
  return a.width() == b.width() && a.height() == b.height() &&
         std::memcmp(a.pixels(), b.pixels(), a.stride() * static_cast<size_t>(a.height())) == 0;
}

}  // namespace

SQ_TEST("image: WIC decodes a generated compressed PNG (filters + deflate) exactly") {
#ifndef _WIN32
  SQ_SKIP("WIC is Windows-only");
#else
  PixelBuffer pb = testPattern(61, 37);
  std::string png;
  REQUIRE(encodePngCompressed(pb, png));
  CHECK(png.size() < pb.rgba.size());  // really compressed
  std::string err;
  ImagePtr wic = decodeImageWic(png.data(), png.size(), 2.0f, &err);
  if (!wic && err.find("CoCreateInstance") != std::string::npos) SQ_SKIP("WIC imaging factory unavailable: " + err);
  REQUIRE(wic);
  CHECK_EQ(wic->width(), 61);
  CHECK_EQ(wic->height(), 37);
  CHECK_EQ(wic->density(), 2.0f);
  // Opaque pixels are exact; premultiplied translucent ones within rounding.
  CHECK_EQ(wic->pixelRgba(3, 4), srcPixel(pb, 3, 4));
  CHECK_EQ(wic->pixelRgba(0, 36), srcPixel(pb, 0, 36));
  for (int y = 0; y < 37; y += 6)
    for (int x = 0; x < 61; x += 5) CHECK(near(wic->pixelRgba(x, y), srcPixel(pb, x, y), 2));
  CHECK_EQ(std::string(imageDecoderName()), std::string("wic"));
  // decodeImage prefers WIC and agrees with the portable decoder bit for bit.
  ImagePtr best = decodeImage(png.data(), png.size(), 1.0f, &err);
  ImagePtr portable = decodeImagePortable(png.data(), png.size(), 1.0f, &err);
  REQUIRE(best);
  REQUIRE(portable);
  CHECK(sameImage(*best, *wic));
  CHECK(sameImage(*portable, *wic));
#endif
}

SQ_TEST("image: WIC encodes and decodes JPEG; probes read the frame size") {
#ifndef _WIN32
  SQ_SKIP("WIC is Windows-only");
#else
  PixelBuffer pb;
  pb.width = 48;
  pb.height = 20;
  pb.rgba.assign(static_cast<size_t>(48) * 20 * 4, 255);
  for (int y = 0; y < 20; ++y)
    for (int x = 0; x < 24; ++x) {  // left half red, right half white
      uint8_t* p = &pb.rgba[(static_cast<size_t>(y) * 48 + x) * 4];
      p[1] = p[2] = 0;
    }
  std::string jpg, err;
  if (!encodeJpegWic(pb, 0.95f, jpg, &err) && err.find("CoCreateInstance") != std::string::npos) SQ_SKIP("WIC unavailable: " + err);
  REQUIRE(!jpg.empty());
  CHECK(sniffImageFormat(jpg.data(), jpg.size()) == ImageFormat::Jpeg);
  uint32_t w = 0, h = 0;
  CHECK(jpegSize(jpg.data(), jpg.size(), &w, &h));
  CHECK_EQ(w, 48u);
  CHECK_EQ(h, 20u);
  CHECK(imageSize(jpg.data(), jpg.size(), &w, &h));
  ImagePtr img = decodeImage(jpg.data(), jpg.size(), 1.0f, &err);
  REQUIRE(img);
  CHECK(near(img->pixelRgba(5, 10), 0xFF0000FFu, 12));
  CHECK(near(img->pixelRgba(40, 10), 0xFFFFFFFFu, 12));
  // The portable decoder is PNG-only and says so.
  CHECK(decodeImagePortable(jpg.data(), jpg.size(), 1.0f, &err) == nullptr);
#endif
}

SQ_TEST("image: portable decoder handles interlaced, palette+tRNS, gray+alpha and 16-bit PNGs") {
  std::string err;
  // Adam7 RGBA
  PixelBuffer pb = testPattern(13, 11);
  std::string ilaced = makePng(13, 11, 8, 6, 1, adam7(pb));
  ImagePtr a = decodeImagePortable(ilaced.data(), ilaced.size(), 1.0f, &err);
  REQUIRE(a);
  for (int y = 0; y < 11; ++y)
    for (int x = 0; x < 13; ++x) CHECK(near(a->pixelRgba(x, y), srcPixel(pb, x, y), 2));

  // 2-bit palette with tRNS: index 1 fully transparent
  std::string pal{"\xFF\x00\x00\x00\xFF\x00\x00\x00\xFF\x10\x20\x30", 12}, trns{"\xFF\x00", 2};
  std::string raw;  // 4x2, indices 0 1 2 3 / 3 2 1 0 -> 1 byte per row
  raw += '\0';
  raw += static_cast<char>(0b00011011);
  raw += '\0';
  raw += static_cast<char>(0b11100100);
  std::string palPng = makePng(4, 2, 2, 3, 0, raw, {{"PLTE", pal}, {"tRNS", trns}});
  ImagePtr p = decodeImagePortable(palPng.data(), palPng.size(), 1.0f, &err);
  REQUIRE(p);
  CHECK_EQ(p->pixelRgba(0, 0), 0xFF0000FFu);
  CHECK_EQ(p->pixelRgba(1, 0) & 0xFF, 0u);
  CHECK_EQ(p->pixelRgba(2, 0), 0x0000FFFFu);
  CHECK_EQ(p->pixelRgba(3, 0), 0x102030FFu);
  CHECK_EQ(p->pixelRgba(0, 1), 0x102030FFu);

  // 16-bit gray + alpha, 2x1
  std::string g16;
  g16 += '\0';
  g16 += std::string("\xFF\xFF\xFF\xFF", 4);  // white, opaque
  g16 += std::string("\x80\x00\x00\x00", 4);  // gray, transparent
  std::string gaPng = makePng(2, 1, 16, 4, 0, g16);
  ImagePtr g = decodeImagePortable(gaPng.data(), gaPng.size(), 1.0f, &err);
  REQUIRE(g);
  CHECK_EQ(g->pixelRgba(0, 0), 0xFFFFFFFFu);
  CHECK_EQ(g->pixelRgba(1, 0) & 0xFF, 0u);

  // 1-bit gray with a color key
  std::string bits;
  bits += '\0';
  bits += static_cast<char>(0b10100000);  // 3 px: 1 0 1
  std::string keyPng = makePng(3, 1, 1, 0, 0, bits, {{"tRNS", std::string("\x00\x00", 2)}});
  ImagePtr k = decodeImagePortable(keyPng.data(), keyPng.size(), 1.0f, &err);
  REQUIRE(k);
  CHECK_EQ(k->pixelRgba(0, 0), 0xFFFFFFFFu);
  CHECK_EQ(k->pixelRgba(1, 0), 0u);

#ifdef _WIN32
  // WIC agrees on the interlaced image.
  if (ImagePtr w = decodeImageWic(ilaced.data(), ilaced.size(), 1.0f, &err)) CHECK(sameImage(*w, *a));
#endif
}

SQ_TEST("image: the stock theme bitmaps decode with both decoders") {
  const std::string root = SQ_UI_THEMES_DIR;
  const char* files[] = {"dark/icons/play.png", "dark/icons/play@2x.png", "dark/knob/cap@3x.png", "light/icons/record@2x.png",
                         "base/meter/segments.png", "base/meter/segments@2x.png", "base/shadow/round@2x.png"};
  for (const char* f : files) {
    auto bytes = sq::readFile(sq::joinPath(root, f));
    REQUIRE(bytes);
    std::string err;
    ImagePtr a = decodeImagePortable(bytes->data(), bytes->size(), 1.0f, &err);
    if (!a) std::printf("  %s: %s\n", f, err.c_str());
    REQUIRE(a);
    uint32_t w = 0, h = 0;
    CHECK(pngSize(bytes->data(), bytes->size(), &w, &h));
    CHECK_EQ(static_cast<uint32_t>(a->width()), w);
    ImagePtr b = decodeImage(bytes->data(), bytes->size(), 1.0f, &err);
    REQUIRE(b);
    CHECK(sameImage(*a, *b));
  }
}

SQ_TEST("image: malformed and hostile PNGs are rejected without crashing") {
  std::string err;
  PixelBuffer pb = testPattern(8, 8);
  std::string png;
  REQUIRE(encodePngCompressed(pb, png));
  CHECK(decodeImage("nope", 4, 1.0f, &err) == nullptr);
  CHECK(decodeImagePortable(png.data(), 20, 1.0f, &err) == nullptr);
  for (size_t cut : {size_t{33}, png.size() / 2, png.size() - 13}) CHECK(decodeImagePortable(png.data(), cut, 1.0f, &err) == nullptr);
  std::string bad = png;
  bad[40] ^= 0x55;  // corrupt IDAT -> CRC mismatch
  CHECK(decodeImagePortable(bad.data(), bad.size(), 1.0f, &err) == nullptr);
  // Huge declared size: refused before any allocation.
  std::string huge = makePng(16384, 16384, 8, 6, 0, std::string(10, '\0'));
  CHECK(decodeImagePortable(huge.data(), huge.size(), 1.0f, &err) == nullptr);
  // Decompression bomb: tiny IDAT inflating far beyond what IHDR allows.
  std::string bomb = makePng(4, 4, 8, 6, 0, std::string(1 << 20, '\0'));
  CHECK(decodeImagePortable(bomb.data(), bomb.size(), 1.0f, &err) == nullptr);
  // The stored-block encoder output still round-trips (snapshot path).
  std::string stored;
  REQUIRE(encodePng(pb, stored));
  ImagePtr s = decodeImagePortable(stored.data(), stored.size(), 1.0f, &err);
  REQUIRE(s);
  CHECK(near(s->pixelRgba(2, 2), pb.pixel(2, 2), 2));
}
