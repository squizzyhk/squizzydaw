// codec-aac tests (Media Foundation): M4A and ADTS encode/decode round trips, alignment and
// quality, sample-accurate seek, abort semantics, unsupported metadata contract, robustness.
#include <cstring>

#include "sqcodec/test_util.hpp"

using namespace sqct;

namespace {

CodecEnv& env() {
  static CodecEnv e("codec-aac", sq::codec::kAacCodecClassId);
  return e;
}

bool mfAvailable() { return env().ok && env().svc->getFormatCount() > 0; }

std::vector<uint8_t> encode(const char* ext, uint32_t ch, double sr, const Channels& c, uint32_t kbps = 192) {
  auto s = sq::makeMemoryStream();
  auto w = env().writer(ext);
  REQUIRE(w);
  auto f = format(sr, ch, SQ_SAMPLE_F32);
  f.bitrate_kbps = kbps;
  REQUIRE_OK(w->openStream(s.get(), &f, nullptr));
  REQUIRE_OK(writeAll(w.get(), c, 1500));
  REQUIRE_OK(w->finalize());
  return streamBytes(s.get());
}

sq::Ptr<sq::IAudioFileReader> openMem(const std::vector<uint8_t>& bytes) {
  auto r = env().reader();
  auto s = sq::makeMemoryStream(bytes.data(), bytes.size(), false);
  if (!r || SQ_FAILED(r->openStream(s.get()))) return {};
  return r;
}

sq_audio_file_format fmtOf(sq::IAudioFileReader* r) {
  sq_audio_file_format f{};
  f.struct_size = sizeof f;
  r->getFormat(&f);
  return f;
}

// best lag (decoded = input delayed by lag) and SNR at that lag over the middle of the signal
void align(const std::vector<float>& in, const std::vector<float>& out, int& lag, double& snr) {
  lag = 0;
  double best = -1;
  const size_t from = 8000, len = 16000;
  for (int l = -3000; l <= 3000; ++l) {
    double s = 0;
    for (size_t i = from; i < from + len; ++i) {
      long j = static_cast<long>(i) + l;
      if (j < 0 || static_cast<size_t>(j) >= out.size()) continue;
      s += in[i] * out[static_cast<size_t>(j)];
    }
    if (s > best) { best = s; lag = l; }
  }
  double e = 0, d = 0;
  for (size_t i = from; i < from + len; ++i) {
    long j = static_cast<long>(i) + lag;
    if (j < 0 || static_cast<size_t>(j) >= out.size()) continue;
    e += in[i] * in[i];
    d += (in[i] - out[static_cast<size_t>(j)]) * (in[i] - out[static_cast<size_t>(j)]);
  }
  snr = 10 * std::log10(e / std::max(d, 1e-12));
}

}  // namespace

SQ_TEST("codec-aac: service loads; formats follow Media Foundation availability") {
  REQUIRE(env().ok);
  auto* s = env().svc.get();
  if (!mfAvailable()) {
    std::printf("Media Foundation not available: AAC tests skipped\n");
    CHECK(!s->supports("m4a", nullptr, SQ_CODEC_READ));
    return;
  }
  CHECK(s->supports("m4a", nullptr, SQ_CODEC_READ | SQ_CODEC_WRITE));
  CHECK(s->supports("aac", "audio/aac", SQ_CODEC_WRITE));
  CHECK(!s->supports("mp4", nullptr, SQ_CODEC_WRITE));
  CHECK(!s->supports("wav", nullptr, SQ_CODEC_READ));
  CHECK_EQ(s->probe("\0\0\0\x20" "ftypM4A ", 12), 95);
  CHECK_EQ(s->probe("RIFF\0\0\0\0WAVE", 12), 0);
  sq::Ptr<sq::IAudioFileWriter> w;
  CHECK_EQ(s->createWriter("ogg", w.put()), SQ_E_UNSUPPORTED);
}

SQ_TEST("codec-aac: M4A round trip (stereo 48k): length, alignment, quality") {
  if (!mfAvailable()) return;
  Channels in = signal(2, 48000, 48000);
  auto bytes = encode("m4a", 2, 48000, in);
  dump("aac_stereo.m4a", bytes);
  CHECK(env().svc->probe(bytes.data(), static_cast<uint32_t>(std::min<size_t>(bytes.size(), 64))) >= 60);
  auto r = openMem(bytes);
  REQUIRE(r);
  auto f = fmtOf(r.get());
  CHECK_EQ(f.sample_rate, 48000.0);
  CHECK_EQ(f.channels, 2u);
  std::printf("m4a: frame_count %lld bitrate %u\n", static_cast<long long>(f.frame_count), f.bitrate_kbps);
  CHECK(f.frame_count >= 48000 && f.frame_count <= 48000 + 4096);
  Channels out = readAll(r.get(), 2);
  CHECK_EQ(static_cast<sq_sample_time>(out[0].size()), f.frame_count);
  int lag = 0;
  double snr = 0;
  align(in[0], out[0], lag, snr);
  std::printf("m4a: lag %d, SNR %.1f dB\n", lag, snr);
  CHECK(snr > 20.0);
  CHECK(lag >= 0 && lag <= 2112);
}

SQ_TEST("codec-aac: ADTS round trip (mono 44.1k)") {
  if (!mfAvailable()) return;
  Channels in = signal(1, 44100, 44100);
  auto bytes = encode("aac", 1, 44100, in, 128);
  dump("aac_mono.aac", bytes);
  REQUIRE(bytes.size() > 100);
  CHECK(bytes[0] == 0xFF && (bytes[1] & 0xF6) == 0xF0);
  auto r = openMem(bytes);
  REQUIRE(r);
  auto f = fmtOf(r.get());
  CHECK_EQ(f.channels, 1u);
  Channels out = readAll(r.get(), 1);
  CHECK(out[0].size() >= 40000);
  int lag = 0;
  double snr = 0;
  align(in[0], out[0], lag, snr);
  std::printf("adts: frames %zu lag %d SNR %.1f dB\n", out[0].size(), lag, snr);
  CHECK(snr > 15.0);
}

SQ_TEST("codec-aac: seeks are sample accurate (decode-and-discard)") {
  if (!mfAvailable()) return;
  auto bytes = encode("m4a", 2, 44100, signal(2, 100000, 44100));
  auto r = openMem(bytes);
  REQUIRE(r);
  Channels full = readAll(r.get(), 2);
  REQUIRE(full[0].size() >= 100000);
  CHECK(seekMatches(r.get(), full, 25, 1e-4));
  CHECK_OK(r->seek(0));
  Channels again = readFrames(r.get(), 2, 3000);
  CHECK(maxDiff(again, full, 0, 3000) <= 1e-4);
}

SQ_TEST("codec-aac: path writer, abort deletes, unsupported formats and metadata") {
  if (!mfAvailable()) return;
  std::string dir = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "sqtest-codec-aac");
  sq_fs_create_dirs(dir.c_str());
  std::string p = sq::joinPath(dir, "take.m4a");
  {
    auto w = env().writer("m4a");
    auto f = format(48000, 2, SQ_SAMPLE_F32);
    REQUIRE_OK(w->open(p.c_str(), &f, nullptr));
    CHECK_EQ(w->setMetadata("tag:title", "x", 1), SQ_E_UNSUPPORTED);
    auto m = sq::cx::makeMarker(1, 0, 0, 0, "m");
    CHECK_EQ(w->addMarker(&m), SQ_E_UNSUPPORTED);
    REQUIRE_OK(writeAll(w.get(), signal(2, 10000)));
  }
  CHECK(!sq_fs_exists(p.c_str()));
  {
    auto w = env().writer("m4a");
    auto f = format(48000, 2, SQ_SAMPLE_F32);
    REQUIRE_OK(w->open(p.c_str(), &f, nullptr));
    REQUIRE_OK(writeAll(w.get(), signal(2, 10000)));
    REQUIRE_OK(w->finalize());
    CHECK_EQ(w->finalize(), SQ_S_FALSE);
  }
  auto r = env().reader();
  REQUIRE_OK(r->open(p.c_str()));
  CHECK(fmtOf(r.get()).frame_count >= 10000);
  r = nullptr;
  sq_fs_remove(p.c_str());
  sq_fs_remove(dir.c_str());
  auto w = env().writer("m4a");
  auto bad = format(96000, 2, SQ_SAMPLE_F32);
  auto s = sq::makeMemoryStream();
  CHECK_EQ(w->openStream(s.get(), &bad, nullptr), SQ_E_UNSUPPORTED);
  auto bad2 = format(48000, 3, SQ_SAMPLE_F32);
  CHECK_EQ(w->openStream(s.get(), &bad2, nullptr), SQ_E_UNSUPPORTED);
  auto rr = env().reader();
  CHECK_EQ(rr->open("Z:/definitely/not/here.m4a"), SQ_E_NOTFOUND);
}

SQ_TEST("codec-aac: truncated / corrupted files never crash") {
  if (!mfAvailable()) return;
  auto bytes = encode("m4a", 2, 48000, signal(2, 20000));
  fuzz(env(), bytes, 60, 21);
  auto adts = encode("aac", 1, 48000, signal(1, 20000), 96);
  fuzz(env(), adts, 40, 22);
  CHECK(true);
}

// ---- verifier additions ---------------------------------------------------------------------------

SQ_TEST("codec-aac: container reported matches the file (ADTS 'aac', MPEG-4 'm4a'), from stream and path") {
  if (!mfAvailable()) return;
  auto adts = encode("aac", 1, 48000, signal(1, 9000), 96);
  auto m4a = encode("m4a", 1, 48000, signal(1, 9000), 96);
  auto r = openMem(adts);
  REQUIRE(r);
  CHECK_EQ(std::string(fmtOf(r.get()).container), std::string("aac"));
  r = openMem(m4a);
  REQUIRE(r);
  CHECK_EQ(std::string(fmtOf(r.get()).container), std::string("m4a"));
  std::string dir = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "sqtest-codec-aac2");
  sq_fs_create_dirs(dir.c_str());
  std::string p = sq::joinPath(dir, "adts.aac");
  REQUIRE_OK(sq_fs_write_file(p.c_str(), adts.data(), adts.size()));
  auto rp = env().reader();
  REQUIRE_OK(rp->open(p.c_str()));
  CHECK_EQ(std::string(fmtOf(rp.get()).container), std::string("aac"));
  rp = nullptr;
  sq_fs_remove(p.c_str());
  sq_fs_remove(dir.c_str());
}

SQ_TEST("codec-aac: writer misuse is refused cleanly") {
  if (!mfAvailable()) return;
  auto w = env().writer("m4a");
  REQUIRE(w);
  CHECK_EQ(w->write(nullptr), SQ_E_CLOSED);
  CHECK_EQ(w->finalize(), SQ_E_CLOSED);
  auto s = sq::makeMemoryStream();
  auto f = format(48000, 2, SQ_SAMPLE_F32);
  REQUIRE_OK(w->openStream(s.get(), &f, nullptr));
  CHECK_EQ(w->openStream(s.get(), &f, nullptr), SQ_E_EXISTS);
  Channels one = signal(1, 100);
  CHECK_EQ(writeAll(w.get(), one), SQ_E_INVALIDARG);  // channel count mismatch
  REQUIRE_OK(writeAll(w.get(), signal(2, 4000)));
  REQUIRE_OK(w->finalize());
  CHECK_EQ(w->write(nullptr), SQ_E_CLOSED);
}
