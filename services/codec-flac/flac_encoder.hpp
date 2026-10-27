// codec-flac: original FLAC frame encoder (fixed block size, CONSTANT / VERBATIM / FIXED / LPC
// subframes, partitioned Rice residuals, stereo decorrelation). Bit exact to the FLAC format spec;
// output is decoded by any conforming decoder. Not realtime: runs on the disk/render worker.
#pragma once

#include <cstdint>
#include <vector>

namespace sq::flac {

class BitWriter {
 public:
  void put(uint64_t v, uint32_t bits);  // bits <= 57
  void putSigned(int64_t v, uint32_t bits) { put(static_cast<uint64_t>(v) & ((bits == 64) ? ~0ull : ((1ull << bits) - 1)), bits); }
  void putUnary(uint32_t zeros);        // `zeros` 0 bits then a 1
  void alignZero();
  std::vector<uint8_t>& bytes() { return out_; }
  void clear() { out_.clear(); acc_ = 0; n_ = 0; }
  size_t bitCount() const { return out_.size() * 8 + n_; }
  void flush();

 private:
  std::vector<uint8_t> out_;
  uint64_t acc_ = 0;
  uint32_t n_ = 0;
};

struct EncoderConfig {
  uint32_t sampleRate = 44100;
  uint32_t channels = 2;
  uint32_t bitsPerSample = 16;  // 4..24
  uint32_t blockSize = 4096;
  uint32_t maxLpcOrder = 12;    // 0 disables LPC
  uint32_t maxPartitionOrder = 8;
};

class Encoder {
 public:
  explicit Encoder(const EncoderConfig& cfg);
  // Encodes one frame of `n` samples (n <= blockSize). ch[c][i] are integer samples within bps.
  // Appends the frame to `out`.
  void encodeFrame(const int32_t* const* ch, uint32_t n, uint64_t frameIndex, std::vector<uint8_t>& out);

 private:
  struct Choice {
    uint64_t bits = ~0ull;
    int type = 0;  // 0 const, 1 verbatim, 2 fixed, 3 lpc
    uint32_t order = 0;
    uint32_t precision = 0;
    int shift = 0;
    int32_t coefs[32] = {};
    std::vector<int32_t> residual;
    uint32_t partOrder = 0;
    std::vector<uint32_t> params;
    bool rice2 = false;
  };
  void analyze(const int32_t* x, uint32_t n, uint32_t bps, Choice& best);
  uint64_t riceCost(const int32_t* res, uint32_t n, uint32_t predOrder, uint32_t& partOrder, std::vector<uint32_t>& params, bool& rice2);
  void writeSubframe(BitWriter& bw, const int32_t* x, uint32_t n, uint32_t bps, const Choice& c);
  void writeResidual(BitWriter& bw, const int32_t* res, uint32_t n, uint32_t predOrder, const Choice& c);
  uint64_t fixedEstimate(const int32_t* x, uint32_t n);

  EncoderConfig cfg_;
  std::vector<double> window_;
  std::vector<int32_t> side_, mid_;
  std::vector<int32_t> tmpRes_;
  std::vector<uint64_t> usum_;
  std::vector<double> windowed_;
};

}  // namespace sq::flac
