// codec-flac: FLAC frame encoder implementation (see flac_encoder.hpp). Written from the public
// format description: frame header + CRC-8, subframes, Rice partitions, CRC-16 footer.
#include "flac_encoder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace sq::flac {

// ---- bits --------------------------------------------------------------------------------------
void BitWriter::put(uint64_t v, uint32_t bits) {
  if (!bits) return;
  // keep the accumulator below 64 bits: drain whole bytes first
  while (n_ >= 8) {
    out_.push_back(static_cast<uint8_t>(acc_ >> (n_ - 8)));
    n_ -= 8;
  }
  if (bits > 56) {
    put(v >> 32, bits - 32);
    put(v & 0xFFFFFFFFull, 32);
    return;
  }
  acc_ = (acc_ << bits) | (v & ((1ull << bits) - 1));
  n_ += bits;
  while (n_ >= 8) {
    out_.push_back(static_cast<uint8_t>(acc_ >> (n_ - 8)));
    n_ -= 8;
  }
  acc_ &= (n_ ? ((1ull << n_) - 1) : 0);
}
void BitWriter::putUnary(uint32_t zeros) {
  while (zeros >= 32) {
    put(0, 32);
    zeros -= 32;
  }
  put(1, zeros + 1);
}
void BitWriter::alignZero() {
  if (n_ & 7) put(0, 8 - (n_ & 7));
}
void BitWriter::flush() { alignZero(); }

namespace {

uint8_t crc8(const uint8_t* p, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int b = 0; b < 8; ++b) c = static_cast<uint8_t>((c & 0x80) ? (c << 1) ^ 0x07 : (c << 1));
  }
  return c;
}
uint16_t crc16(const uint8_t* p, size_t n) {
  uint16_t c = 0;
  for (size_t i = 0; i < n; ++i) {
    c = static_cast<uint16_t>(c ^ (p[i] << 8));
    for (int b = 0; b < 8; ++b) c = static_cast<uint16_t>((c & 0x8000) ? (c << 1) ^ 0x8005 : (c << 1));
  }
  return c;
}
inline uint32_t zigzag(int32_t r) { return (static_cast<uint32_t>(r) << 1) ^ static_cast<uint32_t>(r >> 31); }

// FIXED predictor residual of order 0..4 into res[order..n)
void fixedResidual(const int32_t* x, uint32_t n, uint32_t order, int32_t* res) {
  for (uint32_t i = order; i < n; ++i) {
    int64_t r;
    switch (order) {
      case 0: r = x[i]; break;
      case 1: r = int64_t(x[i]) - x[i - 1]; break;
      case 2: r = int64_t(x[i]) - 2 * int64_t(x[i - 1]) + x[i - 2]; break;
      case 3: r = int64_t(x[i]) - 3 * int64_t(x[i - 1]) + 3 * int64_t(x[i - 2]) - x[i - 3]; break;
      default: r = int64_t(x[i]) - 4 * int64_t(x[i - 1]) + 6 * int64_t(x[i - 2]) - 4 * int64_t(x[i - 3]) + x[i - 4]; break;
    }
    res[i] = static_cast<int32_t>(r);
  }
}

// LPC residual; false when a residual does not fit the Rice coder comfortably.
bool lpcResidual(const int32_t* x, uint32_t n, const int32_t* q, uint32_t order, int shift, int32_t* res) {
  for (uint32_t i = order; i < n; ++i) {
    int64_t sum = 0;
    for (uint32_t j = 0; j < order; ++j) sum += int64_t(q[j]) * x[i - 1 - j];
    int64_t r = int64_t(x[i]) - (sum >> shift);
    if (r > (1ll << 30) || r < -(1ll << 30)) return false;
    res[i] = static_cast<int32_t>(r);
  }
  return true;
}

}  // namespace

Encoder::Encoder(const EncoderConfig& cfg) : cfg_(cfg) {
  // Tukey(0.5) analysis window
  const uint32_t N = cfg_.blockSize;
  window_.resize(N);
  const double p = 0.5;
  const uint32_t taper = static_cast<uint32_t>(p / 2 * N);
  for (uint32_t i = 0; i < N; ++i) {
    double w = 1.0;
    if (taper > 0 && i < taper) w = 0.5 * (1 - std::cos(3.14159265358979323846 * i / taper));
    else if (taper > 0 && i >= N - taper) w = 0.5 * (1 - std::cos(3.14159265358979323846 * (N - 1 - i) / taper));
    window_[i] = w;
  }
  side_.resize(N);
  mid_.resize(N);
  tmpRes_.resize(N);
  windowed_.resize(N);
}

uint64_t Encoder::riceCost(const int32_t* res, uint32_t n, uint32_t predOrder, uint32_t& partOrderOut,
                           std::vector<uint32_t>& paramsOut, bool& rice2Out) {
  // largest usable partition order
  uint32_t maxOrder = 0;
  while (maxOrder < cfg_.maxPartitionOrder && (n % (2u << maxOrder)) == 0 && (n >> (maxOrder + 1)) > predOrder) ++maxOrder;
  // per-partition sums at max order, merged upward
  const uint32_t parts = 1u << maxOrder;
  usum_.assign(parts, 0);
  const uint32_t psize = n >> maxOrder;
  for (uint32_t p = 0; p < parts; ++p) {
    uint32_t s = p == 0 ? predOrder : p * psize, e = (p + 1) * psize;
    uint64_t sum = 0;
    for (uint32_t i = s; i < e; ++i) sum += zigzag(res[i]);
    usum_[p] = sum;
  }
  uint64_t bestBits = ~0ull;
  std::vector<uint32_t> params;
  for (int po = static_cast<int>(maxOrder); po >= 0; --po) {
    const uint32_t np = 1u << po;
    const uint32_t merge = 1u << (maxOrder - po);
    uint64_t bits = 0;
    params.assign(np, 0);
    bool r2 = false;
    for (uint32_t p = 0; p < np; ++p) {
      uint64_t sum = 0;
      for (uint32_t m = 0; m < merge; ++m) sum += usum_[p * merge + m];
      uint32_t cnt = (n >> po) - (p == 0 ? predOrder : 0);
      uint32_t k = 0;
      if (cnt) {
        uint64_t mean = sum / cnt;
        while (k < 30 && (1ull << (k + 1)) <= mean + 1) ++k;
      }
      // evaluate k-1..k+1 with the bound sum >> k (approximation of the exact quotient total)
      uint64_t kb = ~0ull;
      uint32_t kbest = k;
      for (int kk = std::max(0, static_cast<int>(k) - 1); kk <= static_cast<int>(std::min<uint32_t>(k + 1, 30)); ++kk) {
        uint64_t b = static_cast<uint64_t>(cnt) * (kk + 1) + (sum >> kk);
        if (b < kb) { kb = b; kbest = static_cast<uint32_t>(kk); }
      }
      params[p] = kbest;
      if (kbest > 14) r2 = true;
      bits += kb;
    }
    bits += static_cast<uint64_t>(np) * (r2 ? 5 : 4) + 6;
    if (bits < bestBits) {
      bestBits = bits;
      partOrderOut = static_cast<uint32_t>(po);
      paramsOut = params;
      rice2Out = r2;
    }
  }
  return bestBits;
}

uint64_t Encoder::fixedEstimate(const int32_t* x, uint32_t n) {
  uint64_t best = ~0ull;
  for (uint32_t o = 0; o <= 4 && o < n; ++o) {
    fixedResidual(x, n, o, tmpRes_.data());
    uint64_t s = 0;
    for (uint32_t i = o; i < n; ++i) s += zigzag(tmpRes_[i]);
    best = std::min(best, s);
  }
  return best;
}

void Encoder::analyze(const int32_t* x, uint32_t n, uint32_t bps, Choice& best) {
  best = Choice{};
  // CONSTANT
  bool constant = true;
  for (uint32_t i = 1; i < n && constant; ++i) constant = x[i] == x[0];
  if (constant) {
    best.type = 0;
    best.bits = bps;
    return;
  }
  // VERBATIM baseline
  best.type = 1;
  best.bits = static_cast<uint64_t>(n) * bps;
  std::vector<int32_t> res(n);
  // FIXED orders
  for (uint32_t o = 0; o <= 4 && o < n; ++o) {
    fixedResidual(x, n, o, res.data());
    uint32_t po = 0;
    std::vector<uint32_t> params;
    bool r2 = false;
    uint64_t bits = static_cast<uint64_t>(o) * bps + riceCost(res.data(), n, o, po, params, r2);
    if (bits < best.bits) {
      best.bits = bits;
      best.type = 2;
      best.order = o;
      best.residual = res;
      best.partOrder = po;
      best.params = params;
      best.rice2 = r2;
    }
  }
  // LPC
  const uint32_t maxOrder = std::min<uint32_t>(cfg_.maxLpcOrder, n > 1 ? n - 1 : 0);
  if (maxOrder == 0 || n < 32) return;
  for (uint32_t i = 0; i < n; ++i) windowed_[i] = x[i] * window_[n == cfg_.blockSize ? i : i * (cfg_.blockSize - 1) / (n - 1)];
  double autoc[33] = {};
  for (uint32_t lag = 0; lag <= maxOrder; ++lag) {
    double s = 0;
    for (uint32_t i = lag; i < n; ++i) s += windowed_[i] * windowed_[i - lag];
    autoc[lag] = s;
  }
  if (autoc[0] <= 0) return;
  autoc[0] *= 1.0 + 1e-9;  // tiny lag-0 lift for numerical stability
  // Levinson-Durbin: lp[o][j] predicts x[i] from x[i-1-j]
  double lpc[32] = {}, lp[33][32] = {};
  double err = autoc[0];
  uint32_t maxOk = 0;
  for (uint32_t i = 0; i < maxOrder; ++i) {
    double r = -autoc[i + 1];
    for (uint32_t j = 0; j < i; ++j) r -= lpc[j] * autoc[i - j];
    if (err <= 0) break;
    r /= err;
    lpc[i] = r;
    for (uint32_t j = 0; j < (i >> 1); ++j) {
      double t = lpc[j];
      lpc[j] += r * lpc[i - 1 - j];
      lpc[i - 1 - j] += r * t;
    }
    if (i & 1) lpc[i >> 1] += lpc[i >> 1] * r;
    err *= (1.0 - r * r);
    for (uint32_t j = 0; j <= i; ++j) lp[i + 1][j] = -lpc[j];
    maxOk = i + 1;
  }
  const uint32_t precision = bps <= 16 ? 13 : 15;
  for (uint32_t order = 1; order <= maxOk; ++order) {
    double cmax = 0;
    for (uint32_t j = 0; j < order; ++j) cmax = std::max(cmax, std::fabs(lp[order][j]));
    if (cmax <= 0 || !std::isfinite(cmax)) continue;
    int log2c = 0;
    std::frexp(cmax, &log2c);
    --log2c;
    int shift = static_cast<int>(precision) - log2c - 2;
    if (shift > 15) shift = 15;
    if (shift < 0) continue;
    const int32_t qmax = (1 << (precision - 1)) - 1, qmin = -(1 << (precision - 1));
    int32_t q[32];
    double e = 0;
    for (uint32_t j = 0; j < order; ++j) {
      e += lp[order][j] * static_cast<double>(1 << shift);
      long v = std::lround(e);
      if (v > qmax) v = qmax;
      if (v < qmin) v = qmin;
      e -= static_cast<double>(v);
      q[j] = static_cast<int32_t>(v);
    }
    if (!lpcResidual(x, n, q, order, shift, res.data())) continue;
    uint32_t po = 0;
    std::vector<uint32_t> params;
    bool r2 = false;
    uint64_t bits = static_cast<uint64_t>(order) * bps + 4 + 5 + static_cast<uint64_t>(order) * precision +
                    riceCost(res.data(), n, order, po, params, r2);
    if (bits < best.bits) {
      best.bits = bits;
      best.type = 3;
      best.order = order;
      best.precision = precision;
      best.shift = shift;
      std::copy(q, q + order, best.coefs);
      best.residual = res;
      best.partOrder = po;
      best.params = params;
      best.rice2 = r2;
    }
  }
}

void Encoder::writeResidual(BitWriter& bw, const int32_t* res, uint32_t n, uint32_t predOrder, const Choice& c) {
  bw.put(c.rice2 ? 1 : 0, 2);
  bw.put(c.partOrder, 4);
  const uint32_t np = 1u << c.partOrder;
  const uint32_t psize = n >> c.partOrder;
  for (uint32_t p = 0; p < np; ++p) {
    const uint32_t k = c.params[p];
    bw.put(k, c.rice2 ? 5 : 4);
    uint32_t s = p == 0 ? predOrder : p * psize, e = (p + 1) * psize;
    for (uint32_t i = s; i < e; ++i) {
      uint32_t u = zigzag(res[i]);
      bw.putUnary(u >> k);
      if (k) bw.put(u & ((1u << k) - 1), k);
    }
  }
}

void Encoder::writeSubframe(BitWriter& bw, const int32_t* x, uint32_t n, uint32_t bps, const Choice& c) {
  bw.put(0, 1);
  switch (c.type) {
    case 0:
      bw.put(0, 6);
      bw.put(0, 1);
      bw.putSigned(x[0], bps);
      break;
    case 1:
      bw.put(1, 6);
      bw.put(0, 1);
      for (uint32_t i = 0; i < n; ++i) bw.putSigned(x[i], bps);
      break;
    case 2:
      bw.put(8 | c.order, 6);
      bw.put(0, 1);
      for (uint32_t i = 0; i < c.order; ++i) bw.putSigned(x[i], bps);
      writeResidual(bw, c.residual.data(), n, c.order, c);
      break;
    default:
      bw.put(32 | (c.order - 1), 6);
      bw.put(0, 1);
      for (uint32_t i = 0; i < c.order; ++i) bw.putSigned(x[i], bps);
      bw.put(c.precision - 1, 4);
      bw.putSigned(c.shift, 5);
      for (uint32_t i = 0; i < c.order; ++i) bw.putSigned(c.coefs[i], c.precision);
      writeResidual(bw, c.residual.data(), n, c.order, c);
      break;
  }
}

void Encoder::encodeFrame(const int32_t* const* ch, uint32_t n, uint64_t frameIndex, std::vector<uint8_t>& out) {
  const uint32_t bps = cfg_.bitsPerSample;
  // channel assignment
  uint32_t assign = cfg_.channels - 1;  // independent
  const int32_t* src[8] = {};
  uint32_t sbps[8] = {};
  for (uint32_t c = 0; c < cfg_.channels; ++c) { src[c] = ch[c]; sbps[c] = bps; }
  if (cfg_.channels == 2 && n >= 32) {
    for (uint32_t i = 0; i < n; ++i) {
      side_[i] = ch[0][i] - ch[1][i];
      mid_[i] = static_cast<int32_t>((int64_t(ch[0][i]) + ch[1][i]) >> 1);
    }
    uint64_t eL = fixedEstimate(ch[0], n), eR = fixedEstimate(ch[1], n), eS = fixedEstimate(side_.data(), n),
             eM = fixedEstimate(mid_.data(), n);
    uint64_t cand[4] = {eL + eR, eL + eS, eS + eR, eM + eS};
    int best = static_cast<int>(std::min_element(cand, cand + 4) - cand);
    if (best == 1) { assign = 8; src[1] = side_.data(); sbps[1] = bps + 1; }
    else if (best == 2) { assign = 9; src[0] = side_.data(); sbps[0] = bps + 1; }
    else if (best == 3) { assign = 10; src[0] = mid_.data(); src[1] = side_.data(); sbps[1] = bps + 1; }
  }

  BitWriter bw;
  // header
  bw.put(0xFFF8, 16);  // sync + reserved 0 + fixed blocking
  uint32_t bsCode;
  if (n == 4096) bsCode = 12;
  else if (n == 4608) bsCode = 5;
  else if (n <= 256) bsCode = 6;
  else bsCode = 7;
  bw.put(bsCode, 4);
  bw.put(0, 4);  // sample rate: from STREAMINFO
  bw.put(assign, 4);
  bw.put(0, 3);  // sample size: from STREAMINFO
  bw.put(0, 1);
  // frame number, UTF-8 style
  uint64_t v = frameIndex;
  if (v < 0x80) {
    bw.put(v, 8);
  } else {
    int bytes = 2;
    while (bytes < 7 && v >= (1ull << (5 * bytes + 1))) ++bytes;
    uint32_t lead = (0xFF00u >> bytes) & 0xFF;
    bw.put(lead | (v >> (6 * (bytes - 1))), 8);
    for (int i = bytes - 2; i >= 0; --i) bw.put(0x80 | ((v >> (6 * i)) & 0x3F), 8);
  }
  if (bsCode == 6) bw.put(n - 1, 8);
  else if (bsCode == 7) bw.put(n - 1, 16);
  bw.flush();
  bw.bytes().push_back(crc8(bw.bytes().data(), bw.bytes().size()));

  Choice c;
  for (uint32_t k = 0; k < cfg_.channels; ++k) {
    analyze(src[k], n, sbps[k], c);
    writeSubframe(bw, src[k], n, sbps[k], c);
  }
  bw.alignZero();
  uint16_t crc = crc16(bw.bytes().data(), bw.bytes().size());
  bw.put(crc, 16);
  out.insert(out.end(), bw.bytes().begin(), bw.bytes().end());
}

}  // namespace sq::flac
