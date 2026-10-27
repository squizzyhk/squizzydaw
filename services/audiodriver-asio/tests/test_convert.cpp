// audiodriver-asio: native sample format <-> float conversion, bit-exact.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "asio_convert.hpp"
#include "sqtest.hpp"

using sq::asio::SampleType;
using sq::asio::fromFloat;
using sq::asio::toFloat;

namespace {

uint32_t bitsOf(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
float fl(double d) { return static_cast<float>(d); }

// Converts one float to the native format and returns the bytes.
std::vector<uint8_t> enc(SampleType t, float x) {
  std::vector<uint8_t> b(sq::asio::bytesPerSample(t), 0xEE);
  fromFloat(t, &x, b.data(), 1);
  return b;
}
float dec(SampleType t, const std::vector<uint8_t>& b) {
  float x = -123.0f;
  toFloat(t, b.data(), &x, 1);
  return x;
}
std::vector<uint8_t> le32(uint32_t v) {
  return {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
}
std::vector<uint8_t> be32(uint32_t v) {
  return {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
}

struct Lcg {
  uint64_t s = 0x2545F4914F6CDD1Dull;
  uint32_t next() {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>(s >> 32);
  }
};

// Every value of an N-bit integer format (or `limit` pseudo-random ones) decodes to v / 2^(N-1)
// and encodes back to the same container bytes.
void roundTripInt(SampleType t, uint32_t bits, uint32_t limit = 0) {
  const uint32_t bytes = sq::asio::bytesPerSample(t);
  const bool big = sq::asio::isBigEndian(t);
  const uint64_t count = limit ? limit : (1ull << bits);
  const uint32_t chunk = 4096;
  std::vector<uint8_t> src(static_cast<size_t>(chunk) * bytes), back(src.size());
  std::vector<float> f(chunk);
  std::vector<int32_t> vals(chunk);
  Lcg rng;
  uint64_t done = 0, bad = 0;
  while (done < count) {
    const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(chunk, count - done));
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t raw = limit ? rng.next() : static_cast<uint32_t>(done + i);
      const uint32_t shift = 32 - bits;
      const int32_t v = static_cast<int32_t>(raw << shift) >> shift;  // sign-extended N-bit value
      vals[i] = v;
      const uint32_t u = static_cast<uint32_t>(v);
      uint8_t le[4] = {static_cast<uint8_t>(u), static_cast<uint8_t>(u >> 8), static_cast<uint8_t>(u >> 16),
                       static_cast<uint8_t>(u >> 24)};
      uint8_t* p = &src[static_cast<size_t>(i) * bytes];
      for (uint32_t k = 0; k < bytes; ++k) p[k] = big ? le[bytes - 1 - k] : le[k];
    }
    toFloat(t, src.data(), f.data(), n);
    fromFloat(t, f.data(), back.data(), n);
    const double k = 1.0 / static_cast<double>(1ull << (bits - 1));
    for (uint32_t i = 0; i < n; ++i)
      if (bitsOf(f[i]) != bitsOf(fl(vals[i] * k))) ++bad;
    if (std::memcmp(src.data(), back.data(), static_cast<size_t>(n) * bytes) != 0) ++bad;
    done += n;
  }
  CHECK_EQ(bad, 0ull);
}

}  // namespace

SQ_TEST("asio convert: Int16 LSB/MSB every value round-trips bit-exact") {
  roundTripInt(SampleType::Int16LSB, 16);
  roundTripInt(SampleType::Int16MSB, 16);
  CHECK_EQ(bitsOf(dec(SampleType::Int16LSB, {0xFF, 0x7F})), bitsOf(fl(32767.0 / 32768.0)));
  CHECK_EQ(bitsOf(dec(SampleType::Int16LSB, {0x00, 0x80})), bitsOf(-1.0f));
  CHECK_EQ(bitsOf(dec(SampleType::Int16MSB, {0x12, 0x34})), bitsOf(fl(0x1234 / 32768.0)));
}

SQ_TEST("asio convert: Int16 quantisation (clamp, ties to even, NaN)") {
  using B = std::vector<uint8_t>;
  CHECK(enc(SampleType::Int16LSB, 1.0f) == (B{0xFF, 0x7F}));
  CHECK(enc(SampleType::Int16LSB, 2.0f) == (B{0xFF, 0x7F}));
  CHECK(enc(SampleType::Int16LSB, -1.0f) == (B{0x00, 0x80}));
  CHECK(enc(SampleType::Int16LSB, -7.0f) == (B{0x00, 0x80}));
  CHECK(enc(SampleType::Int16LSB, 0.5f) == (B{0x00, 0x40}));
  CHECK(enc(SampleType::Int16MSB, 0.5f) == (B{0x40, 0x00}));
  CHECK(enc(SampleType::Int16LSB, fl(0.5 / 32768)) == (B{0x00, 0x00}));   // 0.5 LSB -> 0 (even)
  CHECK(enc(SampleType::Int16LSB, fl(1.5 / 32768)) == (B{0x02, 0x00}));   // 1.5 LSB -> 2 (even)
  CHECK(enc(SampleType::Int16LSB, fl(2.5 / 32768)) == (B{0x02, 0x00}));   // 2.5 LSB -> 2 (even)
  CHECK(enc(SampleType::Int16LSB, fl(-1.5 / 32768)) == (B{0xFE, 0xFF}));  // -1.5 LSB -> -2
  CHECK(enc(SampleType::Int16LSB, std::numeric_limits<float>::quiet_NaN()) == (B{0x00, 0x00}));
  CHECK(enc(SampleType::Int16LSB, std::numeric_limits<float>::infinity()) == (B{0xFF, 0x7F}));
  CHECK(enc(SampleType::Int16LSB, -std::numeric_limits<float>::infinity()) == (B{0x00, 0x80}));
}

SQ_TEST("asio convert: Int24 packed LSB/MSB every value round-trips bit-exact") {
  roundTripInt(SampleType::Int24LSB, 24);
  roundTripInt(SampleType::Int24MSB, 24, 1u << 20);
  using B = std::vector<uint8_t>;
  CHECK_EQ(bitsOf(dec(SampleType::Int24LSB, {0x56, 0x34, 0x12})), bitsOf(fl(0x123456 / 8388608.0)));
  CHECK_EQ(bitsOf(dec(SampleType::Int24MSB, {0x12, 0x34, 0x56})), bitsOf(fl(0x123456 / 8388608.0)));
  CHECK_EQ(bitsOf(dec(SampleType::Int24LSB, {0xFF, 0xFF, 0xFF})), bitsOf(fl(-1.0 / 8388608.0)));
  CHECK(enc(SampleType::Int24LSB, 1.0f) == (B{0xFF, 0xFF, 0x7F}));
  CHECK(enc(SampleType::Int24MSB, 1.0f) == (B{0x7F, 0xFF, 0xFF}));
  CHECK(enc(SampleType::Int24LSB, -1.0f) == (B{0x00, 0x00, 0x80}));
  CHECK(enc(SampleType::Int24MSB, -0.5f) == (B{0xC0, 0x00, 0x00}));
}

SQ_TEST("asio convert: Int32 LSB/MSB values, scale ends, rounding") {
  CHECK(enc(SampleType::Int32LSB, 1.0f) == le32(0x7FFFFFFFu));
  CHECK(enc(SampleType::Int32LSB, -1.0f) == le32(0x80000000u));
  CHECK(enc(SampleType::Int32LSB, 0.5f) == le32(0x40000000u));
  CHECK(enc(SampleType::Int32MSB, 0.5f) == be32(0x40000000u));
  CHECK(enc(SampleType::Int32LSB, std::ldexp(1.0f, -31)) == le32(1u));
  CHECK(enc(SampleType::Int32LSB, std::ldexp(1.0f, -32)) == le32(0u));        // 0.5 LSB -> 0
  CHECK(enc(SampleType::Int32LSB, std::ldexp(3.0f, -32)) == le32(2u));        // 1.5 LSB -> 2
  CHECK(enc(SampleType::Int32LSB, std::numeric_limits<float>::quiet_NaN()) == le32(0u));
  CHECK_EQ(bitsOf(dec(SampleType::Int32LSB, le32(0x7FFFFFFFu))), bitsOf(1.0f));  // rounds once to 1.0f
  CHECK_EQ(bitsOf(dec(SampleType::Int32LSB, le32(0x80000000u))), bitsOf(-1.0f));
  CHECK_EQ(bitsOf(dec(SampleType::Int32LSB, le32(0x00000100u))), bitsOf(std::ldexp(1.0f, -23)));
  CHECK_EQ(bitsOf(dec(SampleType::Int32MSB, be32(0x00000001u))), bitsOf(std::ldexp(1.0f, -31)));
  // Any int32 with at most 24 significant bits survives int -> float -> int exactly.
  Lcg rng;
  uint32_t bad = 0;
  for (int i = 0; i < 200000; ++i) {
    const int32_t v = static_cast<int32_t>(rng.next() & 0xFFFFFF00u);
    for (SampleType t : {SampleType::Int32LSB, SampleType::Int32MSB}) {
      const std::vector<uint8_t> raw = t == SampleType::Int32LSB ? le32(static_cast<uint32_t>(v)) : be32(static_cast<uint32_t>(v));
      if (enc(t, dec(t, raw)) != raw) ++bad;
    }
  }
  CHECK_EQ(bad, 0u);
}

SQ_TEST("asio convert: Int32 containers with 16/18/20/24-bit alignment") {
  roundTripInt(SampleType::Int32LSB16, 16);
  roundTripInt(SampleType::Int32MSB16, 16);
  roundTripInt(SampleType::Int32LSB18, 18);
  roundTripInt(SampleType::Int32MSB18, 18);
  roundTripInt(SampleType::Int32LSB20, 20);
  roundTripInt(SampleType::Int32MSB20, 20, 1u << 18);
  roundTripInt(SampleType::Int32LSB24, 24);
  roundTripInt(SampleType::Int32MSB24, 24, 1u << 20);
  // reading sign-extends the low N bits and ignores the container's upper bits
  CHECK_EQ(bitsOf(dec(SampleType::Int32LSB16, le32(0x00007FFFu))), bitsOf(fl(32767.0 / 32768.0)));
  CHECK_EQ(bitsOf(dec(SampleType::Int32LSB16, le32(0x0000FFFFu))), bitsOf(fl(-1.0 / 32768.0)));
  CHECK_EQ(bitsOf(dec(SampleType::Int32LSB16, le32(0xABCD8000u))), bitsOf(-1.0f));
  CHECK_EQ(bitsOf(dec(SampleType::Int32LSB24, le32(0x00FFFFFFu))), bitsOf(fl(-1.0 / 8388608.0)));
  CHECK_EQ(bitsOf(dec(SampleType::Int32MSB24, be32(0x00400000u))), bitsOf(0.5f));
  CHECK_EQ(bitsOf(dec(SampleType::Int32LSB18, le32(0x0001FFFFu))), bitsOf(fl(131071.0 / 131072.0)));
  CHECK_EQ(bitsOf(dec(SampleType::Int32LSB20, le32(0x00080000u))), bitsOf(-1.0f));
  // writing is sign-extended to 32 bits and clamped to the N-bit range
  CHECK(enc(SampleType::Int32LSB16, fl(-1.0 / 32768.0)) == le32(0xFFFFFFFFu));
  CHECK(enc(SampleType::Int32LSB16, 0.5f) == le32(0x00004000u));
  CHECK(enc(SampleType::Int32LSB24, 1.0f) == le32(0x007FFFFFu));
  CHECK(enc(SampleType::Int32LSB24, -1.0f) == le32(0xFF800000u));
  CHECK(enc(SampleType::Int32MSB24, -1.0f) == be32(0xFF800000u));
  CHECK(enc(SampleType::Int32LSB20, 3.0f) == le32(0x0007FFFFu));
  CHECK(enc(SampleType::Int32MSB18, -3.0f) == be32(0xFFFE0000u));
}

SQ_TEST("asio convert: MSB output is the byte swap of LSB output for every integer width") {
  const std::pair<SampleType, SampleType> pairs[] = {
      {SampleType::Int16LSB, SampleType::Int16MSB},     {SampleType::Int24LSB, SampleType::Int24MSB},
      {SampleType::Int32LSB, SampleType::Int32MSB},     {SampleType::Int32LSB16, SampleType::Int32MSB16},
      {SampleType::Int32LSB18, SampleType::Int32MSB18}, {SampleType::Int32LSB20, SampleType::Int32MSB20},
      {SampleType::Int32LSB24, SampleType::Int32MSB24}, {SampleType::Float32LSB, SampleType::Float32MSB},
      {SampleType::Float64LSB, SampleType::Float64MSB},
  };
  Lcg rng;
  std::vector<float> in(1024);
  for (float& x : in) x = static_cast<float>(static_cast<int32_t>(rng.next())) / 1.5e9f;  // some beyond +-1
  for (auto [lsb, msb] : pairs) {
    const uint32_t b = sq::asio::bytesPerSample(lsb);
    REQUIRE_EQ(b, sq::asio::bytesPerSample(msb));
    std::vector<uint8_t> l(in.size() * b), m(in.size() * b);
    fromFloat(lsb, in.data(), l.data(), static_cast<uint32_t>(in.size()));
    fromFloat(msb, in.data(), m.data(), static_cast<uint32_t>(in.size()));
    uint32_t bad = 0;
    for (size_t i = 0; i < in.size(); ++i)
      for (uint32_t k = 0; k < b; ++k)
        if (l[i * b + k] != m[i * b + (b - 1 - k)]) ++bad;
    CHECK_EQ(bad, 0u);
    std::vector<float> fl1(in.size()), fl2(in.size());
    toFloat(lsb, l.data(), fl1.data(), static_cast<uint32_t>(in.size()));
    toFloat(msb, m.data(), fl2.data(), static_cast<uint32_t>(in.size()));
    CHECK(std::memcmp(fl1.data(), fl2.data(), fl1.size() * sizeof(float)) == 0);
  }
}

SQ_TEST("asio convert: Float32 / Float64 pass through bit-exact, unclamped") {
  const float vals[] = {0.0f, -0.0f, 1.0f, -1.0f, 2.5f, -1e-40f /* denormal */, 0.1f, 123456.75f};
  for (float v : vals) {
    const std::vector<uint8_t> b = enc(SampleType::Float32LSB, v);
    CHECK_EQ(bitsOf(dec(SampleType::Float32LSB, b)), bitsOf(v));
    CHECK_EQ(bitsOf(dec(SampleType::Float32MSB, enc(SampleType::Float32MSB, v))), bitsOf(v));
    CHECK_EQ(bitsOf(dec(SampleType::Float64LSB, enc(SampleType::Float64LSB, v))), bitsOf(v));
    CHECK_EQ(bitsOf(dec(SampleType::Float64MSB, enc(SampleType::Float64MSB, v))), bitsOf(v));
  }
  using B = std::vector<uint8_t>;
  CHECK(enc(SampleType::Float32MSB, 1.0f) == (B{0x3F, 0x80, 0x00, 0x00}));
  CHECK(enc(SampleType::Float32LSB, 1.0f) == (B{0x00, 0x00, 0x80, 0x3F}));
  CHECK(enc(SampleType::Float64MSB, 1.0f) == (B{0x3F, 0xF0, 0, 0, 0, 0, 0, 0}));
  CHECK(enc(SampleType::Float64LSB, -2.0f) == (B{0, 0, 0, 0, 0, 0, 0x00, 0xC0}));
  // double -> float rounds once (nearest)
  const double d = 0.1;
  B raw(8);
  std::memcpy(raw.data(), &d, 8);
  CHECK_EQ(bitsOf(dec(SampleType::Float64LSB, raw)), bitsOf(static_cast<float>(0.1)));
  // NaN passes through float formats
  CHECK(std::isnan(dec(SampleType::Float32LSB, enc(SampleType::Float32LSB, std::numeric_limits<float>::quiet_NaN()))));
}

SQ_TEST("asio convert: format table, silence, unsupported types") {
  CHECK_EQ(sq::asio::bytesPerSample(SampleType::Int16MSB), 2u);
  CHECK_EQ(sq::asio::bytesPerSample(SampleType::Int24LSB), 3u);
  CHECK_EQ(sq::asio::bytesPerSample(SampleType::Int32LSB20), 4u);
  CHECK_EQ(sq::asio::bytesPerSample(SampleType::Float64MSB), 8u);
  CHECK_EQ(sq::asio::significantBits(SampleType::Int32MSB18), 18u);
  CHECK_EQ(sq::asio::toSampleFormat(SampleType::Int16LSB), SQ_SAMPLE_I16);
  CHECK_EQ(sq::asio::toSampleFormat(SampleType::Int24MSB), SQ_SAMPLE_I24);
  CHECK_EQ(sq::asio::toSampleFormat(SampleType::Int32LSB), SQ_SAMPLE_I32);
  CHECK_EQ(sq::asio::toSampleFormat(SampleType::Int32LSB24), SQ_SAMPLE_I24_IN32);
  CHECK_EQ(sq::asio::toSampleFormat(SampleType::Float32LSB), SQ_SAMPLE_F32);
  CHECK_EQ(sq::asio::toSampleFormat(SampleType::Float64MSB), SQ_SAMPLE_F64);

  const auto bogus = static_cast<SampleType>(32);  // not a PCM type
  CHECK(!sq::asio::isSupported(bogus));
  CHECK_EQ(sq::asio::bytesPerSample(bogus), 0u);
  CHECK_EQ(sq::asio::toSampleFormat(bogus), SQ_SAMPLE_UNKNOWN);
  uint8_t raw[16];
  std::memset(raw, 0x5A, sizeof raw);
  float f[4] = {1, 2, 3, 4};
  toFloat(bogus, raw, f, 4);
  CHECK(f[0] == 0.0f && f[3] == 0.0f);  // unsupported input reads as silence
  const float one[4] = {1, 1, 1, 1};
  fromFloat(bogus, one, raw, 4);
  CHECK_EQ(raw[0], 0x5A);  // unsupported output untouched

  sq::asio::writeSilence(SampleType::Int24MSB, raw, 4);
  for (int i = 0; i < 12; ++i) CHECK_EQ(raw[i], 0);
  CHECK_EQ(raw[12], 0x5A);
}

// ---- independent known-answer vectors (verifier) ----------------------------------------------
// Hand-written byte patterns with their mathematically expected value: catches a bug shared by
// both directions (which a pure round-trip test cannot see) and byte-order mix-ups.
SQ_TEST("asio convert: known-answer byte vectors for every integer type, both directions") {
  struct Vec {
    SampleType t;
    std::vector<uint8_t> bytes;
    double value;  // exact
  };
  const double p15 = 32768.0, p23 = 8388608.0, p31 = 2147483648.0;
  const std::vector<Vec> vecs = {
      {SampleType::Int16LSB, {0x00, 0x80}, -1.0},
      {SampleType::Int16LSB, {0xFF, 0x7F}, 32767.0 / p15},
      {SampleType::Int16LSB, {0x01, 0x00}, 1.0 / p15},
      {SampleType::Int16MSB, {0x80, 0x00}, -1.0},
      {SampleType::Int16MSB, {0x12, 0x34}, 0x1234 / p15},
      {SampleType::Int24LSB, {0x00, 0x00, 0x80}, -1.0},
      {SampleType::Int24LSB, {0x56, 0x34, 0x12}, 0x123456 / p23},
      {SampleType::Int24LSB, {0xFF, 0xFF, 0xFF}, -1.0 / p23},
      {SampleType::Int24MSB, {0x12, 0x34, 0x56}, 0x123456 / p23},
      {SampleType::Int24MSB, {0x80, 0x00, 0x00}, -1.0},
      {SampleType::Int32LSB, {0x00, 0x00, 0x00, 0x40}, 0.5},
      {SampleType::Int32LSB, {0x00, 0x00, 0x00, 0x80}, -1.0},
      {SampleType::Int32MSB, {0xC0, 0x00, 0x00, 0x00}, -0.5},
      {SampleType::Int32MSB, {0x00, 0x00, 0x01, 0x00}, 256.0 / p31},
      {SampleType::Int32LSB16, {0x00, 0x40, 0x00, 0x00}, 0.5},
      {SampleType::Int32LSB16, {0x00, 0x80, 0xFF, 0xFF}, -1.0},
      {SampleType::Int32MSB16, {0xFF, 0xFF, 0xC0, 0x00}, -0.5},
      {SampleType::Int32LSB18, {0x00, 0x00, 0x01, 0x00}, 0.5},       // 2^16 / 2^17
      {SampleType::Int32MSB18, {0xFF, 0xFE, 0x00, 0x00}, -1.0},      // -2^17
      {SampleType::Int32LSB20, {0x00, 0x00, 0x04, 0x00}, 0.5},       // 2^18 / 2^19
      {SampleType::Int32MSB20, {0x00, 0x07, 0xFF, 0xFF}, 524287.0 / 524288.0},
      {SampleType::Int32LSB24, {0x00, 0x00, 0x40, 0x00}, 0.5},
      {SampleType::Int32MSB24, {0xFF, 0x80, 0x00, 0x00}, -1.0},
      {SampleType::Int32MSB24, {0x00, 0x12, 0x34, 0x56}, 0x123456 / p23},
  };
  for (const Vec& v : vecs) {
    const float want = static_cast<float>(v.value);
    const float got = dec(v.t, v.bytes);
    if (bitsOf(got) != bitsOf(want))
      std::printf("decode %s: got %.9g want %.9g\n", sq::asio::sampleTypeName(v.t), got, want);
    CHECK_EQ(bitsOf(got), bitsOf(want));
    // values that are exact in float encode back to exactly these bytes (sign-extended containers)
    const std::vector<uint8_t> e = enc(v.t, want);
    if (e != v.bytes) std::printf("encode %s %.9g mismatch\n", sq::asio::sampleTypeName(v.t), want);
    CHECK(e == v.bytes);
  }
  // container garbage above the significant bits is ignored when reading
  CHECK_EQ(bitsOf(dec(SampleType::Int32LSB16, {0x00, 0x40, 0x5A, 0xA5})), bitsOf(0.5f));
  CHECK_EQ(bitsOf(dec(SampleType::Int32MSB20, {0xAB, 0xAC, 0x00, 0x00})), bitsOf(-0.5f));
  // Float MSB known patterns
  CHECK_EQ(bitsOf(dec(SampleType::Float32MSB, {0x3F, 0x80, 0x00, 0x00})), bitsOf(1.0f));
  CHECK_EQ(bitsOf(dec(SampleType::Float64MSB, {0xBF, 0xE0, 0, 0, 0, 0, 0, 0})), bitsOf(-0.5f));
  CHECK(enc(SampleType::Float64LSB, 0.25f) == (std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0xD0, 0x3F}));
  // full scale clips to the positive maximum in every width
  CHECK(enc(SampleType::Int24MSB, 1.0f) == (std::vector<uint8_t>{0x7F, 0xFF, 0xFF}));
  CHECK(enc(SampleType::Int32LSB18, 1.0f) == le32(0x0001FFFFu));
  CHECK(enc(SampleType::Int32MSB, 2.0f) == be32(0x7FFFFFFFu));
  CHECK(enc(SampleType::Int32LSB20, -7.0f) == le32(0xFFF80000u));
}
