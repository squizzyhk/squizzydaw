// engine/simd: kernel bodies shared by the x86 SIMD sets. Included INSIDE an anonymous namespace
// of ops_<isa>.cpp after that file defined `struct V` (vector traits), so every instantiation has
// internal linkage and is compiled with that file's ISA flags only (no ODR mixing between sets).
//
// V must provide: W (lanes), F/I vector types, load/store/set1/zero/add/sub/mul/madd/min/max/abs,
// hsum/hmax, cvtRound (float->int32, nearest-even), cvtI (int32->float), loadI32/storeI32,
// loadI16 (widening), storeI16 (saturating narrow), loadI24 (reads 3*W+4 bytes), loadF64/storeF64,
// interleave2/deinterleave2, iota, zeroNan (NaN lanes -> +0).
// NaN policy (identical in every set, incl. generic): peak ignores NaN samples, clamp passes NaN
// through, integer conversion maps NaN to 0. min/max return their SECOND operand when either is
// NaN (x86 semantics), so operand order below is deliberate.
// Deliberately no standard-library includes here: scalar tails use SSE scalar intrinsics.

inline int32_t scalarRound(float v) { return _mm_cvtss_si32(_mm_set_ss(v)); }
inline float scalarAbs(float v) { return v < 0.0f ? -v : v; }
inline float clampf(float v, float lo, float hi) {
  v = v < lo ? lo : v;
  return v > hi ? hi : v;
}

void kClear(float* d, uint32_t n) {
  uint32_t i = 0;
  const V::F z = V::zero();
  for (; i + V::W <= n; i += V::W) V::store(d + i, z);
  for (; i < n; ++i) d[i] = 0.0f;
}
void kCopy(float* d, const float* s, uint32_t n) {
  if (d == s) return;
  uint32_t i = 0;
  for (; i + V::W <= n; i += V::W) V::store(d + i, V::load(s + i));
  for (; i < n; ++i) d[i] = s[i];
}
void kAdd(float* d, const float* s, uint32_t n) {
  uint32_t i = 0;
  for (; i + V::W <= n; i += V::W) V::store(d + i, V::add(V::load(d + i), V::load(s + i)));
  for (; i < n; ++i) d[i] += s[i];
}
void kMul(float* d, const float* s, uint32_t n) {
  uint32_t i = 0;
  for (; i + V::W <= n; i += V::W) V::store(d + i, V::mul(V::load(d + i), V::load(s + i)));
  for (; i < n; ++i) d[i] *= s[i];
}
void kScale(float* d, float g, uint32_t n) {
  uint32_t i = 0;
  const V::F gv = V::set1(g);
  for (; i + V::W <= n; i += V::W) V::store(d + i, V::mul(V::load(d + i), gv));
  for (; i < n; ++i) d[i] *= g;
}
void kMix(float* d, const float* s, float g, uint32_t n) {
  uint32_t i = 0;
  const V::F gv = V::set1(g);
  for (; i + V::W <= n; i += V::W) V::store(d + i, V::madd(V::load(s + i), gv, V::load(d + i)));
  for (; i < n; ++i) d[i] += s[i] * g;
}
void kMixRamp(float* d, const float* s, float g0, float g1, uint32_t n) {
  if (!n) return;
  const float step = (g1 - g0) / static_cast<float>(n);
  const V::F stepv = V::set1(step), g0v = V::set1(g0), io = V::iota();
  uint32_t i = 0;
  for (; i + V::W <= n; i += V::W) {
    V::F idx = V::add(V::set1(static_cast<float>(i)), io);
    V::F g = V::madd(stepv, idx, g0v);
    V::store(d + i, V::madd(V::load(s + i), g, V::load(d + i)));
  }
  for (; i < n; ++i) d[i] += s[i] * (g0 + step * static_cast<float>(i));
}
float kPeak(const float* s, uint32_t n) {
  uint32_t i = 0;
  V::F acc = V::zero();
  for (; i + V::W <= n; i += V::W) acc = V::max(V::abs(V::load(s + i)), acc);  // NaN lane keeps acc
  float m = V::hmax(acc);
  for (; i < n; ++i) {
    float a = scalarAbs(s[i]);
    if (a > m) m = a;
  }
  return m;
}
float kSumSquares(const float* s, uint32_t n) {
  uint32_t i = 0;
  V::F a0 = V::zero(), a1 = V::zero();
  for (; i + 2 * V::W <= n; i += 2 * V::W) {
    V::F x = V::load(s + i), y = V::load(s + i + V::W);
    a0 = V::madd(x, x, a0);
    a1 = V::madd(y, y, a1);
  }
  for (; i + V::W <= n; i += V::W) {
    V::F x = V::load(s + i);
    a0 = V::madd(x, x, a0);
  }
  float r = V::hsum(V::add(a0, a1));
  for (; i < n; ++i) r += s[i] * s[i];
  return r;
}
float kDot(const float* a, const float* b, uint32_t n) {
  uint32_t i = 0;
  V::F a0 = V::zero(), a1 = V::zero();
  for (; i + 2 * V::W <= n; i += 2 * V::W) {
    a0 = V::madd(V::load(a + i), V::load(b + i), a0);
    a1 = V::madd(V::load(a + i + V::W), V::load(b + i + V::W), a1);
  }
  for (; i + V::W <= n; i += V::W) a0 = V::madd(V::load(a + i), V::load(b + i), a0);
  float r = V::hsum(V::add(a0, a1));
  for (; i < n; ++i) r += a[i] * b[i];
  return r;
}
void kClamp(float* d, float lo, float hi, uint32_t n) {
  uint32_t i = 0;
  const V::F lv = V::set1(lo), hv = V::set1(hi);
  for (; i + V::W <= n; i += V::W) V::store(d + i, V::min(hv, V::max(lv, V::load(d + i))));  // NaN passes
  for (; i < n; ++i) d[i] = clampf(d[i], lo, hi);
}
void kInterleave(float* d, const float* const* s, uint32_t ch, uint32_t frames) {
  uint32_t f = 0;
  if (ch == 2) {
    const float* l = s[0];
    const float* r = s[1];
    for (; f + V::W <= frames; f += V::W) V::interleave2(d + 2 * f, V::load(l + f), V::load(r + f));
  } else if (ch == 1) {
    kCopy(d, s[0], frames);
    return;
  }
  for (; f < frames; ++f)
    for (uint32_t c = 0; c < ch; ++c) d[f * ch + c] = s[c][f];
}
void kDeinterleave(float* const* d, const float* s, uint32_t ch, uint32_t frames) {
  uint32_t f = 0;
  if (ch == 2) {
    float* l = d[0];
    float* r = d[1];
    for (; f + V::W <= frames; f += V::W) {
      V::F a, b;
      V::deinterleave2(s + 2 * f, a, b);
      V::store(l + f, a);
      V::store(r + f, b);
    }
  } else if (ch == 1) {
    kCopy(d[0], s, frames);
    return;
  }
  for (; f < frames; ++f)
    for (uint32_t c = 0; c < ch; ++c) d[c][f] = s[f * ch + c];
}
void kFromI16(float* d, const int16_t* s, uint32_t n) {
  uint32_t i = 0;
  const V::F k = V::set1(1.0f / kI16Scale);
  for (; i + V::W <= n; i += V::W) V::store(d + i, V::mul(V::cvtI(V::loadI16(s + i)), k));
  for (; i < n; ++i) d[i] = static_cast<float>(s[i]) * (1.0f / kI16Scale);
}
void kFromI24(float* d, const uint8_t* s, uint32_t n) {
  uint32_t i = 0;
  const V::F k = V::set1(1.0f / kI24Scale);
  for (; i + V::W + 2 <= n; i += V::W) V::store(d + i, V::mul(V::cvtI(V::loadI24(s + 3 * i)), k));
  for (; i < n; ++i) {
    const uint8_t* p = s + 3 * i;
    int32_t v = static_cast<int32_t>(static_cast<uint32_t>(p[0]) << 8 | static_cast<uint32_t>(p[1]) << 16 |
                                     static_cast<uint32_t>(p[2]) << 24) >> 8;
    d[i] = static_cast<float>(v) * (1.0f / kI24Scale);
  }
}
void kFromI32(float* d, const int32_t* s, uint32_t n) {
  uint32_t i = 0;
  const V::F k = V::set1(1.0f / kI32Scale);
  for (; i + V::W <= n; i += V::W) V::store(d + i, V::mul(V::cvtI(V::loadI32(s + i)), k));
  for (; i < n; ++i) d[i] = static_cast<float>(s[i]) * (1.0f / kI32Scale);
}
inline V::I vecRoundClamp(V::F x, V::F scale, V::F lo, V::F hi) {
  return V::cvtRound(V::min(V::max(V::zeroNan(V::mul(x, scale)), lo), hi));
}
inline int32_t roundClampS(float x, float scale, float maxv) {
  if (x != x) return 0;  // NaN -> silence, never full scale
  return scalarRound(clampf(x * scale, -scale, maxv));
}
void kToI16(int16_t* d, const float* s, uint32_t n) {
  uint32_t i = 0;
  const V::F sc = V::set1(kI16Scale), lo = V::set1(-kI16Scale), hi = V::set1(kI16Max);
  for (; i + V::W <= n; i += V::W) V::storeI16(d + i, vecRoundClamp(V::load(s + i), sc, lo, hi));
  for (; i < n; ++i) d[i] = static_cast<int16_t>(roundClampS(s[i], kI16Scale, kI16Max));
}
void kToI24(uint8_t* d, const float* s, uint32_t n) {
  uint32_t i = 0;
  const V::F sc = V::set1(kI24Scale), lo = V::set1(-kI24Scale), hi = V::set1(kI24Max);
  alignas(64) int32_t tmp[V::W];
  for (; i + V::W <= n; i += V::W) {
    V::storeI32(tmp, vecRoundClamp(V::load(s + i), sc, lo, hi));
    uint8_t* p = d + 3 * i;
    for (uint32_t j = 0; j < V::W; ++j) {
      uint32_t v = static_cast<uint32_t>(tmp[j]);
      p[3 * j] = static_cast<uint8_t>(v);
      p[3 * j + 1] = static_cast<uint8_t>(v >> 8);
      p[3 * j + 2] = static_cast<uint8_t>(v >> 16);
    }
  }
  for (; i < n; ++i) {
    uint32_t v = static_cast<uint32_t>(roundClampS(s[i], kI24Scale, kI24Max));
    d[3 * i] = static_cast<uint8_t>(v);
    d[3 * i + 1] = static_cast<uint8_t>(v >> 8);
    d[3 * i + 2] = static_cast<uint8_t>(v >> 16);
  }
}
void kToI32(int32_t* d, const float* s, uint32_t n) {
  uint32_t i = 0;
  const V::F sc = V::set1(kI32Scale), lo = V::set1(-kI32Scale), hi = V::set1(kI32Max);
  for (; i + V::W <= n; i += V::W) V::storeI32(d + i, vecRoundClamp(V::load(s + i), sc, lo, hi));
  for (; i < n; ++i) d[i] = roundClampS(s[i], kI32Scale, kI32Max);
}
void kFromF64(float* d, const double* s, uint32_t n) {
  uint32_t i = 0;
  for (; i + V::W <= n; i += V::W) V::store(d + i, V::loadF64(s + i));
  for (; i < n; ++i) d[i] = static_cast<float>(s[i]);
}
void kToF64(double* d, const float* s, uint32_t n) {
  uint32_t i = 0;
  for (; i + V::W <= n; i += V::W) V::storeF64(d + i, V::load(s + i));
  for (; i < n; ++i) d[i] = s[i];
}

#define SQ_SIMD_KERNEL_TABLE(isaId, isaName)                                                          \
  {sizeof(sq_vector_ops), isaId, isaName, &kClear, &kCopy, &kAdd, &kMul, &kScale, &kMix, &kMixRamp, \
   &kPeak, &kSumSquares, &kDot, &kClamp, &kInterleave, &kDeinterleave, &kFromI16, &kFromI24,         \
   &kFromI32, &kToI16, &kToI24, &kToI32, &kFromF64, &kToF64}
