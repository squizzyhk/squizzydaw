// codec-flac tests: lossless round trips, MD5, compression, metadata/markers, seek, robustness.
#include <cstring>

#include "md5.hpp"
#include "sqcodec/test_util.hpp"

using namespace sqct;

namespace {

CodecEnv& env() {
  static CodecEnv e("codec-flac", sq::codec::kFlacCodecClassId);
  return e;
}

std::vector<uint8_t> encode(const sq_audio_file_format& f, const Channels& c, uint32_t block = 1000,
                            const std::function<void(sq::IAudioFileWriter*)>& before = {},
                            const std::function<void(sq::IAudioFileWriter*)>& after = {}) {
  auto s = sq::makeMemoryStream();
  auto w = env().writer("flac");
  REQUIRE(w);
  REQUIRE_OK(w->openStream(s.get(), &f, nullptr));
  if (before) before(w.get());
  REQUIRE_OK(writeAll(w.get(), c, block));
  if (after) after(w.get());
  REQUIRE_OK(w->finalize());
  return streamBytes(s.get());
}

sq::Ptr<sq::IAudioFileReader> openMem(const std::vector<uint8_t>& bytes) {
  auto r = env().reader();
  auto s = sq::makeMemoryStream(bytes.data(), bytes.size(), false);
  if (!r || SQ_FAILED(r->openStream(s.get()))) return {};
  return r;
}

// What the encoder stores: float -> int at `bits`, back to float (decoder output).
Channels quantize(const Channels& c, uint32_t bits) {
  Channels q = c;
  for (auto& ch : q)
    for (auto& x : ch) x = static_cast<float>(sq::cx::floatToInt(x, bits) * (1.0 / (1u << (bits - 1))));
  return q;
}

sq_audio_file_format fmtOf(sq::IAudioFileReader* r) {
  sq_audio_file_format f{};
  f.struct_size = sizeof f;
  r->getFormat(&f);
  return f;
}

Channels noisy(uint32_t ch, uint32_t frames) {
  Channels c = signal(ch, frames, 44100);
  uint32_t st = 777;
  for (auto& v : c)
    for (auto& x : v) {
      st ^= st << 13; st ^= st >> 17; st ^= st << 5;
      x = x * 0.9f + (static_cast<float>(st & 0xFFFF) / 65536.0f - 0.5f) * 0.05f;
    }
  return c;
}

}  // namespace

SQ_TEST("codec-flac: service loads and answers supports/probe") {
  REQUIRE(env().ok);
  auto* s = env().svc.get();
  CHECK(s->supports("flac", nullptr, SQ_CODEC_READ | SQ_CODEC_WRITE));
  CHECK(s->supports(nullptr, "audio/flac", SQ_CODEC_WRITE));
  CHECK(!s->supports("wav", nullptr, SQ_CODEC_READ));
  CHECK_EQ(s->probe("fLaC\0\0\0\x22", 8), 100);
  CHECK_EQ(s->probe("RIFF", 4), 0);
  sq::Ptr<sq::IAudioFileWriter> w;
  CHECK_EQ(s->createWriter("wav", w.put()), SQ_E_UNSUPPORTED);
}

SQ_TEST("codec-flac: lossless round trips (16/24-bit, mono..6ch, odd lengths)") {
  struct Case { uint32_t ch, frames, bits; sq_sample_format sf; };
  const Case cases[] = {{2, 44100, 16, SQ_SAMPLE_I16}, {1, 10007, 16, SQ_SAMPLE_I16}, {2, 20000, 24, SQ_SAMPLE_I24},
                        {6, 9000, 24, SQ_SAMPLE_I24}, {2, 5000, 24, SQ_SAMPLE_F32}, {2, 17, 16, SQ_SAMPLE_I16},
                        {3, 4096, 16, SQ_SAMPLE_I16}, {2, 4097, 20, SQ_SAMPLE_I24}};
  for (const auto& c : cases) {
    Channels in = noisy(c.ch, c.frames);
    uint32_t bits = c.sf == SQ_SAMPLE_F32 ? 24 : c.bits;
    auto bytes = encode(format(44100, c.ch, c.sf, c.sf == SQ_SAMPLE_F32 ? 0 : c.bits), in, 999);
    dump(("flac_" + std::to_string(c.ch) + "ch_" + std::to_string(c.frames) + "_" + std::to_string(bits) + ".flac").c_str(), bytes);
    auto r = openMem(bytes);
    REQUIRE(r);
    auto f = fmtOf(r.get());
    CHECK_EQ(f.channels, c.ch);
    CHECK_EQ(f.frame_count, sq_sample_time(c.frames));
    CHECK_EQ(f.bits_per_sample, bits);
    Channels out = readAll(r.get(), c.ch);
    REQUIRE_EQ(out[0].size(), size_t(c.frames));
    double d = maxDiff(quantize(in, bits), out);
    if (d != 0.0) std::printf("ch %u frames %u bits %u: diff %g\n", c.ch, c.frames, bits, d);
    CHECK_EQ(d, 0.0);
  }
}

SQ_TEST("codec-flac: STREAMINFO MD5 matches the PCM, and compression actually compresses") {
  Channels in = signal(2, 88200, 44100);
  auto bytes = encode(format(44100, 2, SQ_SAMPLE_I16), in);
  REQUIRE(bytes.size() > 42);
  sq::flac::Md5 md5;
  for (size_t i = 0; i < in[0].size(); ++i)
    for (int c = 0; c < 2; ++c) {
      int32_t v = sq::cx::floatToInt(in[c][i], 16);
      uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
      md5.update(b, 2);
    }
  uint8_t digest[16];
  md5.final(digest);
  CHECK(std::memcmp(bytes.data() + 8 + 18, digest, 16) == 0);
  // tones + harmonics compress to well under half of raw 16-bit PCM
  const size_t raw = in[0].size() * 2 * 2;
  std::printf("flac size %zu / raw %zu = %.1f%%\n", bytes.size(), raw, 100.0 * bytes.size() / raw);
  CHECK(bytes.size() < raw / 2);
  // known MD5 vector (RFC 1321)
  sq::flac::Md5 t;
  t.update("abc", 3);
  t.final(digest);
  const uint8_t abc[16] = {0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0, 0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72};
  CHECK(std::memcmp(digest, abc, 16) == 0);
}

SQ_TEST("codec-flac: silence, full-scale and constant blocks") {
  Channels in(2, std::vector<float>(10000, 0.0f));
  for (size_t i = 5000; i < 10000; ++i) { in[0][i] = (i & 1) ? 1.0f : -1.0f; in[1][i] = 0.25f; }
  auto bytes = encode(format(48000, 2, SQ_SAMPLE_I16), in);
  auto r = openMem(bytes);
  REQUIRE(r);
  CHECK_EQ(maxDiff(quantize(in, 16), readAll(r.get(), 2)), 0.0);
}

SQ_TEST("codec-flac: tags, RIFF chunks and markers round trip; late metadata grows the header") {
  std::vector<uint8_t> big(20000);
  for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 13);
  Channels in = signal(2, 30000);
  auto bytes = encode(
      format(48000, 2, SQ_SAMPLE_I24), in, 1000,
      [](sq::IAudioFileWriter* w) {
        REQUIRE_OK(w->setMetadata("tag:title", "Take Five-ish", 13));
        REQUIRE_OK(w->setMetadata("tag:custom_field", "x", 1));
        REQUIRE_OK(w->setMetadata("chunk:iXML", "<a/>", 4));
        auto m = sq::cx::makeMarker(3, 1000, 0, 0, "Hit");
        REQUIRE_OK(w->addMarker(&m));
        auto l = sq::cx::makeMarker(4, 2000, 500, SQ_MARKER_LOOP, "Loop");
        REQUIRE_OK(w->addMarker(&l));
      },
      [&](sq::IAudioFileWriter* w) {
        // after audio: forces the metadata region to grow beyond the reserved padding
        REQUIRE_OK(w->setMetadata("chunk:blob", big.data(), uint32_t(big.size())));
        REQUIRE_OK(w->setMetadata("tag:artist", "Late Arrival", 12));
      });
  dump("flac_meta.flac", bytes);
  auto r = openMem(bytes);
  REQUIRE(r);
  CHECK_EQ(metaText(r.get(), "tag:title"), std::string("Take Five-ish"));
  CHECK_EQ(metaText(r.get(), "tag:artist"), std::string("Late Arrival"));
  CHECK_EQ(metaText(r.get(), "tag:custom_field"), std::string("x"));
  CHECK_EQ(metaText(r.get(), "chunk:iXML"), std::string("<a/>"));
  CHECK(metaValue(r.get(), "chunk:blob") == big);
  REQUIRE_EQ(r->getMarkerCount(), 2u);
  sq_audio_file_marker m{};
  m.struct_size = sizeof m;
  REQUIRE_OK(r->getMarker(1, &m));
  CHECK_EQ(m.position, 2000);
  CHECK_EQ(m.length, 500);
  CHECK(m.flags & SQ_MARKER_LOOP);
  CHECK_EQ(std::string(m.name), std::string("Loop"));
  CHECK_EQ(maxDiff(quantize(in, 24), readAll(r.get(), 2)), 0.0);
}

SQ_TEST("codec-flac: seeks are sample accurate") {
  Channels in = noisy(2, 100000);
  auto bytes = encode(format(44100, 2, SQ_SAMPLE_I16), in);
  auto r = openMem(bytes);
  REQUIRE(r);
  Channels full = readAll(r.get(), 2);
  REQUIRE_EQ(full[0].size(), size_t(100000));
  CHECK(seekMatches(r.get(), full, 60, 0.0));
  CHECK_OK(r->seek(100000));
  CHECK(readFrames(r.get(), 2, 10)[0].empty());
  CHECK_EQ(r->seek(100001), SQ_E_INVALIDARG);
}

SQ_TEST("codec-flac: release without finalize deletes the file; finalize twice is harmless") {
  std::string dir = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "sqtest-codec-flac");
  sq_fs_create_dirs(dir.c_str());
  std::string p = sq::joinPath(dir, "abort.flac");
  {
    auto w = env().writer("flac");
    auto f = format(48000, 1, SQ_SAMPLE_I16);
    REQUIRE_OK(w->open(p.c_str(), &f, nullptr));
    REQUIRE_OK(writeAll(w.get(), signal(1, 9000)));
  }
  CHECK(!sq_fs_exists(p.c_str()));
  {
    auto w = env().writer("flac");
    auto f = format(48000, 1, SQ_SAMPLE_I16);
    REQUIRE_OK(w->open(p.c_str(), &f, nullptr));
    REQUIRE_OK(writeAll(w.get(), signal(1, 9000)));
    REQUIRE_OK(w->finalize());
    CHECK_EQ(w->finalize(), SQ_S_FALSE);
  }
  auto r = env().reader();
  REQUIRE_OK(r->open(p.c_str()));
  CHECK_EQ(fmtOf(r.get()).frame_count, 9000);
  r = nullptr;
  sq_fs_remove(p.c_str());
  sq_fs_remove(dir.c_str());
}

SQ_TEST("codec-flac: truncated / corrupted files never crash") {
  auto bytes = encode(format(44100, 2, SQ_SAMPLE_I16), noisy(2, 20000), 1000, [](sq::IAudioFileWriter* w) {
    w->setMetadata("tag:title", "fuzz", 4);
    auto m = sq::cx::makeMarker(1, 10, 0, 0, "m");
    w->addMarker(&m);
  });
  fuzz(env(), bytes, 400, 5);
  CHECK(true);
}

// ---- verifier additions ---------------------------------------------------------------------------

SQ_TEST("codec-flac: edge formats (8/12-bit, 8 channels, 192k, 1 frame, full-scale noise, anti-phase square)") {
  struct Case { uint32_t ch, frames, bits; double sr; int kind; };  // kind 0 noisy, 1 white noise, 2 anti-phase square
  const Case cases[] = {{1, 5000, 8, 22050, 0},  {2, 5000, 12, 32000, 0},  {8, 6000, 16, 48000, 0},
                        {2, 9000, 24, 192000, 0}, {2, 1, 16, 44100, 0},     {1, 31, 24, 44100, 0},
                        {2, 8192, 24, 48000, 1},  {2, 8192, 24, 48000, 2},  {2, 8192, 16, 48000, 2}};
  for (const auto& c : cases) {
    Channels in = noisy(c.ch, c.frames);
    uint32_t st = 4242;
    for (uint32_t k = 0; k < c.ch; ++k)
      for (uint32_t i = 0; i < c.frames; ++i) {
        if (c.kind == 1) { st ^= st << 13; st ^= st >> 17; st ^= st << 5; in[k][i] = static_cast<float>(static_cast<int32_t>(st)) / 2147483648.0f; }
        if (c.kind == 2) in[k][i] = ((i / 7) & 1 ? 1.0f : -1.0f) * (k ? -1.0f : 1.0f);
      }
    sq_sample_format sf = c.bits <= 16 ? SQ_SAMPLE_I16 : SQ_SAMPLE_I24;
    auto bytes = encode(format(c.sr, c.ch, sf, c.bits), in, 777);
    auto r = openMem(bytes);
    REQUIRE(r);
    auto f = fmtOf(r.get());
    CHECK_EQ(f.channels, c.ch);
    CHECK_EQ(f.sample_rate, c.sr);
    CHECK_EQ(f.bits_per_sample, c.bits);
    CHECK_EQ(f.frame_count, sq_sample_time(c.frames));
    Channels out = readAll(r.get(), c.ch);
    REQUIRE_EQ(out[0].size(), size_t(c.frames));
    double d = maxDiff(quantize(in, c.bits), out);
    if (d != 0.0) std::printf("edge ch %u frames %u bits %u kind %d: diff %g\n", c.ch, c.frames, c.bits, c.kind, d);
    CHECK_EQ(d, 0.0);
  }
}

SQ_TEST("codec-flac: large early metadata removed after the audio started still finalizes") {
  std::vector<uint8_t> big(10u << 20, 0x5A);
  Channels in = noisy(2, 3000);
  auto bytes = encode(format(44100, 2, SQ_SAMPLE_I16), in, 1000,
                      [&](sq::IAudioFileWriter* w) {
                        REQUIRE_OK(w->setMetadata("chunk:big1", big.data(), uint32_t(big.size())));
                        REQUIRE_OK(w->setMetadata("chunk:big2", big.data(), uint32_t(big.size())));
                      },
                      [&](sq::IAudioFileWriter* w) {
                        REQUIRE_OK(w->setMetadata("chunk:big1", nullptr, 0));
                        REQUIRE_OK(w->setMetadata("chunk:big2", nullptr, 0));
                        REQUIRE_OK(w->setMetadata("tag:title", "small", 5));
                      });
  auto r = openMem(bytes);
  REQUIRE(r);
  CHECK_EQ(metaText(r.get(), "tag:title"), std::string("small"));
  CHECK(metaValue(r.get(), "chunk:big1").empty());
  CHECK_EQ(maxDiff(quantize(in, 16), readAll(r.get(), 2)), 0.0);
}

SQ_TEST("codec-flac: a tag too large for one metadata block is refused, never silently truncated") {
  std::vector<uint8_t> huge(17u << 20, 'a');
  auto s = sq::makeMemoryStream();
  auto w = env().writer("flac");
  auto f = format(44100, 1, SQ_SAMPLE_I16);
  REQUIRE_OK(w->openStream(s.get(), &f, nullptr));
  sq_result rs = w->setMetadata("tag:comment", huge.data(), uint32_t(huge.size()));
  REQUIRE_OK(writeAll(w.get(), noisy(1, 1000)));
  sq_result rf = w->finalize();
  if (SQ_SUCCEEDED(rs) && SQ_SUCCEEDED(rf)) {
    auto r = openMem(streamBytes(s.get()));
    REQUIRE(r);
    CHECK_EQ(metaValue(r.get(), "tag:comment").size(), huge.size());  // stored intact
  } else {
    CHECK(SQ_FAILED(rs));  // refused up front, not at finalize after all the audio
  }
}
