// Image container, format probes, the portable PNG decoder (miniz inflate) and the portable
// compressed PNG encoder. WIC lives in image_wic.cpp.
#include "sq/ui/image.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <new>

#include "miniz.h"
#include "sq/platform.h"

namespace sq::ui {

namespace {
std::atomic<uint64_t> g_nextImageId{1};

uint32_t be32(const uint8_t* q) { return (uint32_t{q[0]} << 24) | (uint32_t{q[1]} << 16) | (uint32_t{q[2]} << 8) | uint32_t{q[3]}; }
uint16_t be16(const uint8_t* q) { return static_cast<uint16_t>((q[0] << 8) | q[1]); }
uint16_t le16(const uint8_t* q) { return static_cast<uint16_t>(q[0] | (q[1] << 8)); }
uint32_t le32(const uint8_t* q) { return uint32_t{q[0]} | (uint32_t{q[1]} << 8) | (uint32_t{q[2]} << 16) | (uint32_t{q[3]} << 24); }

constexpr uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

bool setErr(std::string* e, const char* msg) {
  if (e) *e = msg;
  return false;
}
}  // namespace

Image::Image(int w, int h, std::vector<uint8_t> bgraPremul, float density)
    : width_(w > 0 ? w : 0), height_(h > 0 ? h : 0), density_(density > 0 ? density : 1.0f), pixels_(std::move(bgraPremul)),
      id_(g_nextImageId.fetch_add(1, std::memory_order_relaxed)) {
  pixels_.resize(static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4, 0);
}

uint32_t Image::pixelRgba(int x, int y) const {
  if (x < 0 || y < 0 || x >= width_ || y >= height_) return 0;
  const uint8_t* p = pixels_.data() + static_cast<size_t>(y) * stride() + static_cast<size_t>(x) * 4;
  uint32_t a = p[3];
  auto un = [a](uint8_t c) -> uint32_t { return a == 0 ? 0u : std::min<uint32_t>(255u, (uint32_t{c} * 255u + a / 2) / a); };
  return (un(p[2]) << 24) | (un(p[1]) << 16) | (un(p[0]) << 8) | a;
}

// ---- probes --------------------------------------------------------------------------------------

ImageFormat sniffImageFormat(const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  if (!p) return ImageFormat::Unknown;
  if (size >= 8 && std::memcmp(p, kPngSig, 8) == 0) return ImageFormat::Png;
  if (size >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return ImageFormat::Jpeg;
  if (size >= 2 && p[0] == 'B' && p[1] == 'M') return ImageFormat::Bmp;
  if (size >= 6 && (std::memcmp(p, "GIF87a", 6) == 0 || std::memcmp(p, "GIF89a", 6) == 0)) return ImageFormat::Gif;
  return ImageFormat::Unknown;
}

bool pngSize(const void* data, size_t size, uint32_t* width, uint32_t* height) {
  if (!data || size < 24) return false;
  const auto* p = static_cast<const uint8_t*>(data);
  if (std::memcmp(p, kPngSig, 8) != 0 || std::memcmp(p + 12, "IHDR", 4) != 0) return false;
  if (width) *width = be32(p + 16);
  if (height) *height = be32(p + 20);
  return true;
}

bool jpegSize(const void* data, size_t size, uint32_t* width, uint32_t* height) {
  const auto* p = static_cast<const uint8_t*>(data);
  if (!p || size < 4 || p[0] != 0xFF || p[1] != 0xD8) return false;
  size_t i = 2;
  while (i + 4 <= size) {
    if (p[i] != 0xFF) return false;
    uint8_t m = p[i + 1];
    if (m == 0xFF) {  // fill byte
      ++i;
      continue;
    }
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {  // standalone markers
      i += 2;
      continue;
    }
    if (m == 0xD9 || m == 0xDA) return false;  // EOI / SOS before any frame header
    size_t len = be16(p + i + 2);
    if (len < 2 || i + 2 + len > size) return false;
    bool sof = m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
    if (sof) {
      if (len < 7) return false;
      if (height) *height = be16(p + i + 5);
      if (width) *width = be16(p + i + 7);
      return true;
    }
    i += 2 + len;
  }
  return false;
}

bool imageSize(const void* data, size_t size, uint32_t* width, uint32_t* height) {
  const auto* p = static_cast<const uint8_t*>(data);
  switch (sniffImageFormat(data, size)) {
    case ImageFormat::Png: return pngSize(data, size, width, height);
    case ImageFormat::Jpeg: return jpegSize(data, size, width, height);
    case ImageFormat::Bmp: {
      if (size < 26) return false;
      // |int32| computed in unsigned arithmetic (INT32_MIN has no positive int32).
      auto mag = [](uint32_t v) { return (v & 0x80000000u) ? 0u - v : v; };
      if (width) *width = mag(le32(p + 18));
      if (height) *height = mag(le32(p + 22));
      return true;
    }
    case ImageFormat::Gif:
      if (size < 10) return false;
      if (width) *width = le16(p + 6);
      if (height) *height = le16(p + 8);
      return true;
    default: return false;
  }
}

// ---- portable PNG decoder ------------------------------------------------------------------------

namespace {

struct PngHeader {
  uint32_t w = 0, h = 0;
  int depth = 0, ctype = 0, interlace = 0;
  int channels() const { return ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : 4; }
  size_t bitsPerPixel() const { return static_cast<size_t>(channels()) * static_cast<size_t>(depth); }
  size_t rowBytes(uint32_t width) const { return (static_cast<size_t>(width) * bitsPerPixel() + 7) / 8; }
  size_t filterBpp() const { return std::max<size_t>(1, bitsPerPixel() / 8); }
};

bool validDepth(int ctype, int depth) {
  switch (ctype) {
    case 0: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case 2:
    case 4:
    case 6: return depth == 8 || depth == 16;
    default: return false;
  }
}

uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
  int p = int{a} + int{b} - int{c};
  int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

// In-place unfilter of one pass (rows of 1 filter byte + rowBytes). false on a bad filter type.
bool unfilter(uint8_t* data, uint32_t rows, size_t rowBytes, size_t bpp) {
  uint8_t* prev = nullptr;
  for (uint32_t y = 0; y < rows; ++y) {
    uint8_t* line = data + static_cast<size_t>(y) * (rowBytes + 1);
    uint8_t ft = line[0];
    uint8_t* cur = line + 1;
    for (size_t i = 0; i < rowBytes; ++i) {
      uint8_t a = i >= bpp ? cur[i - bpp] : 0;
      uint8_t b = prev ? prev[i] : 0;
      uint8_t c = prev && i >= bpp ? prev[i - bpp] : 0;
      switch (ft) {
        case 0: break;
        case 1: cur[i] = static_cast<uint8_t>(cur[i] + a); break;
        case 2: cur[i] = static_cast<uint8_t>(cur[i] + b); break;
        case 3: cur[i] = static_cast<uint8_t>(cur[i] + ((int{a} + int{b}) >> 1)); break;
        case 4: cur[i] = static_cast<uint8_t>(cur[i] + paeth(a, b, c)); break;
        default: return false;
      }
    }
    prev = cur;
  }
  return true;
}

// Everything a decode needs from the chunk stream, validated: header, palette/tRNS, the
// concatenated IDAT bytes and the exact inflated size the header implies.
struct PngParsed {
  PngHeader hdr;
  uint8_t palette[256][4] = {};
  uint32_t paletteSize = 0;
  bool hasKey = false;
  uint16_t key[3] = {0, 0, 0};
  std::vector<uint8_t> idat;
  size_t expected = 0;
};

struct PngDecodeState {
  const PngParsed& png;
  const PngHeader& hdr;
  std::vector<uint8_t>& rgba;  // straight RGBA8 output, w*h*4

  // Raw sample k (0-based, channel-interleaved) of a row at the header bit depth.
  uint32_t sample(const uint8_t* row, size_t k) const {
    int d = hdr.depth;
    if (d == 8) return row[k];
    if (d == 16) return be16(row + k * 2);
    size_t bit = k * static_cast<size_t>(d);
    uint32_t byte = row[bit / 8];
    uint32_t shift = static_cast<uint32_t>(8 - d - static_cast<int>(bit % 8));
    return (byte >> shift) & ((1u << d) - 1u);
  }
  uint8_t to8(uint32_t v) const {
    int d = hdr.depth;
    if (d == 8) return static_cast<uint8_t>(v);
    if (d == 16) return static_cast<uint8_t>(v >> 8);
    return static_cast<uint8_t>(v * 255u / ((1u << d) - 1u));
  }

  void emitRow(const uint8_t* row, uint32_t width, uint32_t y, uint32_t x0, uint32_t dx) {
    const int ch = hdr.channels();
    for (uint32_t i = 0; i < width; ++i) {
      uint8_t* o = rgba.data() + (static_cast<size_t>(y) * hdr.w + x0 + static_cast<size_t>(i) * dx) * 4;
      size_t k = static_cast<size_t>(i) * static_cast<size_t>(ch);
      switch (hdr.ctype) {
        case 0: {
          uint32_t g = sample(row, k);
          o[0] = o[1] = o[2] = to8(g);
          o[3] = png.hasKey && g == png.key[0] ? 0 : 255;
          break;
        }
        case 2: {
          uint32_t r = sample(row, k), g = sample(row, k + 1), b = sample(row, k + 2);
          o[0] = to8(r);
          o[1] = to8(g);
          o[2] = to8(b);
          o[3] = png.hasKey && r == png.key[0] && g == png.key[1] && b == png.key[2] ? 0 : 255;
          break;
        }
        case 3: {
          uint32_t idx = sample(row, k);
          if (idx < png.paletteSize) std::memcpy(o, png.palette[idx], 4);
          else o[0] = o[1] = o[2] = o[3] = 0;  // out-of-range index: transparent
          break;
        }
        case 4: {
          o[0] = o[1] = o[2] = to8(sample(row, k));
          o[3] = to8(sample(row, k + 1));
          break;
        }
        default: {
          o[0] = to8(sample(row, k));
          o[1] = to8(sample(row, k + 1));
          o[2] = to8(sample(row, k + 2));
          o[3] = to8(sample(row, k + 3));
          break;
        }
      }
    }
  }
};

struct Pass {
  uint32_t x0, y0, dx, dy;
};
constexpr Pass kAdam7[7] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};

// Size of the zlib stream `z` once inflated, counted through a 32 KB window (nothing proportional
// to the output is allocated). Fails on corrupt/truncated data, an Adler-32 mismatch, or as soon
// as the output exceeds `want`.
bool inflatedSizeIs(const std::vector<uint8_t>& z, size_t want, std::string* error) {
  auto dec = std::make_unique<tinfl_decompressor>();
  tinfl_init(dec.get());
  std::vector<uint8_t> dict(TINFL_LZ_DICT_SIZE);
  size_t inPos = 0, outPos = 0, total = 0;
  for (;;) {
    size_t inBytes = z.size() - inPos, outBytes = dict.size() - outPos;
    tinfl_status s = tinfl_decompress(dec.get(), z.data() + inPos, &inBytes, dict.data(), dict.data() + outPos, &outBytes,
                                      TINFL_FLAG_PARSE_ZLIB_HEADER);
    inPos += inBytes;
    total += outBytes;
    outPos = (outPos + outBytes) & (TINFL_LZ_DICT_SIZE - 1);
    if (total > want) return setErr(error, "PNG image data inflates beyond the size IHDR implies");
    if (s == TINFL_STATUS_DONE) break;
    if (s != TINFL_STATUS_HAS_MORE_OUTPUT) return setErr(error, "corrupt or truncated PNG image data");
  }
  return total == want || setErr(error, "corrupt or truncated PNG image data");
}

// Walks and validates the chunk stream (CRCs, IHDR limits, PLTE/tRNS, IDAT, IEND) and checks that
// the image data really inflates to exactly what IHDR implies - before anything sized by the
// header is allocated, so a tiny file cannot claim a huge image.
bool parsePng(const uint8_t* p, size_t size, PngParsed& out, std::string* error) {
  if (!p || size < 33 || std::memcmp(p, kPngSig, 8) != 0) return setErr(error, "not a PNG");
  PngHeader& hdr = out.hdr;
  bool sawIhdr = false, sawEnd = false;
  size_t off = 8;
  while (off + 12 <= size) {
    uint32_t len = be32(p + off);
    if (len > size - off - 12) return setErr(error, "truncated PNG chunk");
    const uint8_t* type = p + off + 4;
    const uint8_t* body = p + off + 8;
    if (sq_crc32(0, type, 4 + static_cast<size_t>(len)) != be32(body + len)) return setErr(error, "PNG chunk CRC mismatch");
    if (!sawIhdr && std::memcmp(type, "IHDR", 4) != 0) return setErr(error, "PNG does not start with IHDR");
    if (std::memcmp(type, "IHDR", 4) == 0) {
      if (sawIhdr || len != 13) return setErr(error, "bad PNG IHDR");
      sawIhdr = true;
      hdr.w = be32(body);
      hdr.h = be32(body + 4);
      hdr.depth = body[8];
      hdr.ctype = body[9];
      hdr.interlace = body[12];
      if (hdr.w == 0 || hdr.h == 0 || hdr.w > 16384 || hdr.h > 16384 || uint64_t{hdr.w} * hdr.h > kMaxImagePixels)
        return setErr(error, "PNG dimensions out of range");
      if (!validDepth(hdr.ctype, hdr.depth) || body[10] != 0 || body[11] != 0 || hdr.interlace > 1)
        return setErr(error, "unsupported PNG layout");
    } else if (std::memcmp(type, "PLTE", 4) == 0) {
      if (len % 3 != 0 || len / 3 > 256 || len == 0) return setErr(error, "bad PNG palette");
      out.paletteSize = len / 3;
      for (uint32_t i = 0; i < out.paletteSize; ++i) {
        out.palette[i][0] = body[i * 3];
        out.palette[i][1] = body[i * 3 + 1];
        out.palette[i][2] = body[i * 3 + 2];
        out.palette[i][3] = 255;
      }
    } else if (std::memcmp(type, "tRNS", 4) == 0) {
      if (hdr.ctype == 3) {
        for (uint32_t i = 0; i < len && i < 256; ++i) out.palette[i][3] = body[i];
      } else if (hdr.ctype == 0 && len >= 2) {
        out.hasKey = true;
        out.key[0] = be16(body);
      } else if (hdr.ctype == 2 && len >= 6) {
        out.hasKey = true;
        out.key[0] = be16(body);
        out.key[1] = be16(body + 2);
        out.key[2] = be16(body + 4);
      }
    } else if (std::memcmp(type, "IDAT", 4) == 0) {
      if (out.idat.size() + len > (size_t{1} << 30)) return setErr(error, "PNG data too large");
      out.idat.insert(out.idat.end(), body, body + len);
    } else if (std::memcmp(type, "IEND", 4) == 0) {
      sawEnd = true;
      break;
    } else if ((type[0] & 0x20) == 0) {
      return setErr(error, "unknown critical PNG chunk");
    }
    off += 12 + static_cast<size_t>(len);
  }
  if (!sawIhdr || !sawEnd || out.idat.empty()) return setErr(error, "incomplete PNG");
  if (hdr.ctype == 3 && out.paletteSize == 0) return setErr(error, "palette PNG without PLTE");

  // Exact inflated size from the header: inflate can never write more than this.
  out.expected = 0;
  if (hdr.interlace == 0) {
    out.expected = (hdr.rowBytes(hdr.w) + 1) * hdr.h;
  } else {
    for (const Pass& ps : kAdam7) {
      uint32_t pw = hdr.w > ps.x0 ? (hdr.w - ps.x0 + ps.dx - 1) / ps.dx : 0;
      uint32_t ph = hdr.h > ps.y0 ? (hdr.h - ps.y0 + ps.dy - 1) / ps.dy : 0;
      if (pw && ph) out.expected += (hdr.rowBytes(pw) + 1) * ph;
    }
  }
  return inflatedSizeIs(out.idat, out.expected, error);
}

}  // namespace

ImagePtr decodeImagePortable(const void* data, size_t size, float density, std::string* error) {
  try {
    PngParsed png;
    if (!parsePng(static_cast<const uint8_t*>(data), size, png, error)) return nullptr;
    const PngHeader& hdr = png.hdr;
    std::vector<uint8_t> raw(png.expected);
    size_t got = tinfl_decompress_mem_to_mem(raw.data(), raw.size(), png.idat.data(), png.idat.size(), TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || got != png.expected) {
      setErr(error, "corrupt or truncated PNG image data");
      return nullptr;
    }
    std::vector<uint8_t> pixels;
    PngDecodeState st{png, hdr, pixels};
    st.rgba.assign(static_cast<size_t>(hdr.w) * hdr.h * 4, 0);
    const size_t bpp = hdr.filterBpp();
    if (hdr.interlace == 0) {
      size_t rb = hdr.rowBytes(hdr.w);
      if (!unfilter(raw.data(), hdr.h, rb, bpp)) {
        setErr(error, "bad PNG filter type");
        return nullptr;
      }
      for (uint32_t y = 0; y < hdr.h; ++y) st.emitRow(raw.data() + static_cast<size_t>(y) * (rb + 1) + 1, hdr.w, y, 0, 1);
    } else {
      size_t at = 0;
      for (const Pass& ps : kAdam7) {
        uint32_t pw = hdr.w > ps.x0 ? (hdr.w - ps.x0 + ps.dx - 1) / ps.dx : 0;
        uint32_t ph = hdr.h > ps.y0 ? (hdr.h - ps.y0 + ps.dy - 1) / ps.dy : 0;
        if (!pw || !ph) continue;
        size_t rb = hdr.rowBytes(pw);
        if (!unfilter(raw.data() + at, ph, rb, bpp)) {
          setErr(error, "bad PNG filter type");
          return nullptr;
        }
        for (uint32_t j = 0; j < ph; ++j) st.emitRow(raw.data() + at + static_cast<size_t>(j) * (rb + 1) + 1, pw, ps.y0 + j * ps.dy, ps.x0, ps.dx);
        at += (rb + 1) * ph;
      }
    }
    // straight RGBA -> premultiplied BGRA
    std::vector<uint8_t>& px = st.rgba;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
      uint32_t a = px[i + 3];
      uint8_t r = px[i], g = px[i + 1], b = px[i + 2];
      px[i] = static_cast<uint8_t>((b * a + 127) / 255);
      px[i + 1] = static_cast<uint8_t>((g * a + 127) / 255);
      px[i + 2] = static_cast<uint8_t>((r * a + 127) / 255);
    }
    return std::make_shared<Image>(static_cast<int>(hdr.w), static_cast<int>(hdr.h), std::move(px), density);
  } catch (const std::bad_alloc&) {
    setErr(error, "out of memory decoding PNG");
    return nullptr;
  }
}

bool pngPreflight(const void* data, size_t size, std::string* error) {
  try {
    PngParsed png;
    return parsePng(static_cast<const uint8_t*>(data), size, png, error);
  } catch (const std::bad_alloc&) {
    return setErr(error, "out of memory checking PNG");
  }
}

ImagePtr decodeImage(const void* data, size_t size, float density, std::string* error) {
  if (!data || size == 0) {
    setErr(error, "empty image data");
    return nullptr;
  }
  // PNGs are checked first (bounded, strict): WIC would allocate the frame IHDR claims before it
  // reads any data, and it accepts truncated image data (filling the rest with zeros).
  const bool png = sniffImageFormat(data, size) == ImageFormat::Png;
  if (png && !pngPreflight(data, size, error)) return nullptr;
  std::string wicErr;
  if (ImagePtr img = decodeImageWic(data, size, density, &wicErr)) return img;
  if (png) return decodeImagePortable(data, size, density, error);
  if (error) *error = wicErr.empty() ? "unsupported image format" : wicErr;
  return nullptr;
}

// ---- portable compressed PNG encoder -------------------------------------------------------------

namespace {
void put32(std::string& s, uint32_t v) {
  s.push_back(static_cast<char>(v >> 24));
  s.push_back(static_cast<char>(v >> 16));
  s.push_back(static_cast<char>(v >> 8));
  s.push_back(static_cast<char>(v));
}
void pngChunk(std::string& out, const char* type, const std::string& data) {
  put32(out, static_cast<uint32_t>(data.size()));
  std::string td(type, 4);
  td += data;
  out += td;
  put32(out, sq_crc32(0, td.data(), td.size()));
}
}  // namespace

bool encodePngCompressed(const PixelBuffer& px, std::string& out, std::string* error) {
  if (px.width <= 0 || px.height <= 0 || px.rgba.size() != static_cast<size_t>(px.width) * static_cast<size_t>(px.height) * 4)
    return setErr(error, "empty or inconsistent pixel buffer");
  try {
    const size_t row = static_cast<size_t>(px.width) * 4;
    std::vector<uint8_t> raw((row + 1) * static_cast<size_t>(px.height));
    std::vector<uint8_t> cand(row);
    const uint8_t* prev = nullptr;
    for (int y = 0; y < px.height; ++y) {
      const uint8_t* cur = px.rgba.data() + row * static_cast<size_t>(y);
      uint8_t* dst = raw.data() + (row + 1) * static_cast<size_t>(y);
      // Minimum sum of absolute differences heuristic over the five filters.
      uint64_t best = UINT64_MAX;
      for (uint8_t ft = 0; ft < 5; ++ft) {
        uint64_t sum = 0;
        for (size_t i = 0; i < row; ++i) {
          uint8_t a = i >= 4 ? cur[i - 4] : 0, b = prev ? prev[i] : 0, c = prev && i >= 4 ? prev[i - 4] : 0;
          uint8_t pred = ft == 0 ? 0 : ft == 1 ? a : ft == 2 ? b : ft == 3 ? static_cast<uint8_t>((int{a} + int{b}) >> 1) : paeth(a, b, c);
          cand[i] = static_cast<uint8_t>(cur[i] - pred);
          sum += static_cast<uint64_t>(std::min<int>(cand[i], 256 - cand[i]));
        }
        if (sum < best) {
          best = sum;
          dst[0] = ft;
          std::memcpy(dst + 1, cand.data(), row);
        }
      }
      prev = cur;
    }
    if (raw.size() > 0x7FFFFFFFu) return setErr(error, "image too large");
    mz_ulong zlen = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::string z(static_cast<size_t>(zlen), '\0');
    if (mz_compress2(reinterpret_cast<unsigned char*>(z.data()), &zlen, raw.data(), static_cast<mz_ulong>(raw.size()), 9) != MZ_OK)
      return setErr(error, "deflate failed");
    z.resize(static_cast<size_t>(zlen));
    out.assign(reinterpret_cast<const char*>(kPngSig), 8);
    std::string ihdr;
    put32(ihdr, static_cast<uint32_t>(px.width));
    put32(ihdr, static_cast<uint32_t>(px.height));
    ihdr += std::string("\x08\x06\x00\x00\x00", 5);
    pngChunk(out, "IHDR", ihdr);
    pngChunk(out, "IDAT", z);
    pngChunk(out, "IEND", std::string());
    return true;
  } catch (const std::bad_alloc&) {
    return setErr(error, "out of memory encoding PNG");
  }
}

}  // namespace sq::ui
