// codec-mp3 tests: decode of ffmpeg/LAME-made fixtures (gapless length, tags, chapters), seek
// accuracy, robustness, and the LAME seam (encode round trip only when a LAME library is found).
#include <algorithm>
#include <cstring>

#include "sqcodec/test_util.hpp"

using namespace sqct;

namespace {

CodecEnv& env() {
  static CodecEnv e("codec-mp3", sq::codec::kMp3CodecClassId);
  return e;
}

std::vector<uint8_t> fixture(const char* name) { return loadFile(sq::joinPath(SQ_CODEC_TEST_DATA, name)); }

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

int zeroCrossings(const std::vector<float>& v, size_t from, size_t to) {
  int n = 0;
  for (size_t i = from + 1; i < to && i < v.size(); ++i) n += (v[i - 1] < 0) != (v[i] < 0);
  return n;
}

}  // namespace

SQ_TEST("codec-mp3: service loads; probe recognises frames and ID3") {
  REQUIRE(env().ok);
  auto* s = env().svc.get();
  CHECK(s->supports("mp3", nullptr, SQ_CODEC_READ));
  CHECK(s->supports(nullptr, "audio/mpeg", SQ_CODEC_READ));
  auto a = fixture("tone_v24_chapters.mp3");
  REQUIRE(a.size() > 1000);
  CHECK(s->probe(a.data(), 4096) >= 80);
  CHECK(s->probe(a.data(), 8) >= 30);
  CHECK_EQ(s->probe("RIFF\0\0\0\0WAVE", 12), 0);
  CHECK_EQ(s->probe("fLaC", 4), 0);
}

SQ_TEST("codec-mp3: gapless decode of a LAME CBR stereo file, ID3v2.4 tags and chapters") {
  auto r = openMem(fixture("tone_v24_chapters.mp3"));
  REQUIRE(r);
  auto f = fmtOf(r.get());
  CHECK_EQ(f.sample_rate, 44100.0);
  CHECK_EQ(f.channels, 2u);
  CHECK_EQ(f.frame_count, 44100);  // encoder delay and padding removed
  CHECK_EQ(f.sample_format, SQ_SAMPLE_F32);
  CHECK(f.bitrate_kbps >= 120 && f.bitrate_kbps <= 145);
  Channels c = readAll(r.get(), 2);
  REQUIRE_EQ(c[0].size(), size_t(44100));
  // 440 Hz left / 660 Hz right over 0.9 s: ~792 / ~1188 zero crossings
  int zl = zeroCrossings(c[0], 2205, 41895), zr = zeroCrossings(c[1], 2205, 41895);
  CHECK(std::abs(zl - 792) <= 6);
  CHECK(std::abs(zr - 1188) <= 6);
  CHECK_EQ(metaText(r.get(), "tag:title"), std::string("Squizzy Test Tone"));
  CHECK_EQ(metaText(r.get(), "tag:artist"), std::string("Squizzy Studio"));
  CHECK_EQ(metaText(r.get(), "tag:album"), std::string("Codec Fixtures"));
  CHECK(!metaValue(r.get(), "chunk:ID3 ").empty());
  REQUIRE_EQ(r->getMarkerCount(), 2u);
  sq_audio_file_marker m{};
  m.struct_size = sizeof m;
  REQUIRE_OK(r->getMarker(1, &m));
  CHECK_EQ(m.position, 17640);  // 400 ms
  CHECK_EQ(m.length, 26460);
  CHECK(m.flags & SQ_MARKER_REGION);
  CHECK_EQ(std::string(m.name), std::string("Main"));
}

SQ_TEST("codec-mp3: mono VBR file with ID3v2.3 + ID3v1") {
  auto r = openMem(fixture("mono_vbr_v23.mp3"));
  REQUIRE(r);
  auto f = fmtOf(r.get());
  CHECK_EQ(f.sample_rate, 48000.0);
  CHECK_EQ(f.channels, 1u);
  CHECK_EQ(f.frame_count, 24000);
  CHECK((f.flags & SQ_AUDIOFILE_VBR) != 0);
  CHECK_EQ(metaText(r.get(), "tag:title"), std::string("Mono VBR"));
  CHECK_EQ(metaText(r.get(), "tag:genre"), std::string("Test"));
  CHECK_EQ(readAll(r.get(), 1)[0].size(), size_t(24000));
}

SQ_TEST("codec-mp3: seeks are sample accurate") {
  auto r = openMem(fixture("tone_v24_chapters.mp3"));
  REQUIRE(r);
  Channels full = readAll(r.get(), 2);
  REQUIRE_EQ(full[0].size(), size_t(44100));
  CHECK(seekMatches(r.get(), full, 40, 1e-6));
  CHECK_OK(r->seek(44100));
  CHECK(readFrames(r.get(), 2, 10)[0].empty());
  CHECK_EQ(r->seek(44101), SQ_E_INVALIDARG);
}

SQ_TEST("codec-mp3: encoder seam follows LAME availability") {
  auto* s = env().svc.get();
  sq::Ptr<sq::IAudioFileWriter> w;
  sq_result r = s->createWriter("mp3", w.put());
  const bool canWrite = s->supports("mp3", nullptr, SQ_CODEC_WRITE) != SQ_FALSE;
  CHECK_EQ(SQ_SUCCEEDED(r), canWrite);
  sq_codec_format_info fi{};
  fi.struct_size = sizeof fi;
  REQUIRE_OK(s->getFormatInfo(0, &fi));
  CHECK_EQ((fi.flags & SQ_CODEC_WRITE) != 0, canWrite);
  if (!canWrite) {
    CHECK_EQ(r, SQ_E_UNSUPPORTED);
    std::printf("LAME not found: MP3 encode round trip skipped (set SQ_LAME_LIBRARY to run it)\n");
    return;
  }
  // round trip through LAME + our ID3v2.4 writer + dr_mp3
  Channels in = signal(2, 44100, 44100);
  auto stream = sq::makeMemoryStream();
  auto fmt = format(44100, 2, SQ_SAMPLE_F32);
  fmt.bitrate_kbps = 192;
  REQUIRE_OK(w->openStream(stream.get(), &fmt, nullptr));
  REQUIRE_OK(w->setMetadata("tag:title", "Encoded Here", 12));
  auto m = sq::cx::makeMarker(1, 22050, 4410, 0, "Half");
  REQUIRE_OK(w->addMarker(&m));
  REQUIRE_OK(writeAll(w.get(), in));
  REQUIRE_OK(w->setMetadata("tag:artist", std::string(6000, 'a').c_str(), 6000));  // forces tag growth
  REQUIRE_OK(w->finalize());
  auto bytes = streamBytes(stream.get());
  dump("lame_roundtrip.mp3", bytes);
  auto rd = openMem(bytes);
  REQUIRE(rd);
  auto f = fmtOf(rd.get());
  CHECK_EQ(f.frame_count, 44100);
  CHECK_EQ(metaText(rd.get(), "tag:title"), std::string("Encoded Here"));
  CHECK_EQ(metaText(rd.get(), "tag:artist").size(), size_t(6000));
  REQUIRE_EQ(rd->getMarkerCount(), 1u);
  sq_audio_file_marker got{};
  got.struct_size = sizeof got;
  REQUIRE_OK(rd->getMarker(0, &got));
  CHECK_EQ(got.position, 22050);
  CHECK_EQ(std::string(got.name), std::string("Half"));
  Channels out = readAll(rd.get(), 2);
  REQUIRE_EQ(out[0].size(), size_t(44100));
  // lossy: compare energy of the difference against the signal (gapless => aligned)
  double e = 0, d = 0;
  for (size_t i = 0; i < 44100; ++i) { e += in[0][i] * in[0][i]; d += (in[0][i] - out[0][i]) * (in[0][i] - out[0][i]); }
  std::printf("LAME round trip SNR %.1f dB\n", 10 * std::log10(e / std::max(d, 1e-12)));
  CHECK(e / std::max(d, 1e-12) > 100.0);  // > 20 dB
}

SQ_TEST("codec-mp3: truncated / corrupted files never crash") {
  fuzz(env(), fixture("tone_v24_chapters.mp3"), 300, 7);
  fuzz(env(), fixture("mono_vbr_v23.mp3"), 200, 8);
  // garbage that only looks like a frame header / ID3 header
  std::vector<uint8_t> g(3000, 0xFF);
  hammer(env(), g);
  std::vector<uint8_t> id3 = {'I', 'D', '3', 4, 0, 0, 0x7F, 0x7F, 0x7F, 0x7F, 'T', 'I', 'T', '2', 0x7F, 0x7F, 0x7F, 0x7F, 0, 0, 3};
  hammer(env(), id3);
  CHECK(true);
}

// ---- verifier additions ---------------------------------------------------------------------------

namespace {
// Minimal ID3v2.<ver> tag: TIT2 "Old" + PRIV frame (no tag mapping) + 16 bytes of padding.
std::vector<uint8_t> rawTag(uint8_t ver) {
  sq::cx::Bytes frames;
  auto frame = [&](const char* id, const std::vector<uint8_t>& d) {
    frames.fourcc(id);
    const uint32_t n = static_cast<uint32_t>(d.size());
    if (ver == 4) { frames.u8((n >> 21) & 0x7F); frames.u8((n >> 14) & 0x7F); frames.u8((n >> 7) & 0x7F); frames.u8(n & 0x7F); }
    else frames.u32be(n);
    frames.u16be(0);
    frames.raw(d.data(), d.size());
  };
  frame("TIT2", {0, 'O', 'l', 'd'});
  std::vector<uint8_t> priv = {'s', 'q', '.', 't', 'e', 's', 't', 0};
  for (int i = 0; i < 300; ++i) priv.push_back(static_cast<uint8_t>(i * 13));
  frame("PRIV", priv);
  const uint32_t size = static_cast<uint32_t>(frames.size() + 16);
  sq::cx::Bytes t;
  t.raw("ID3", 3); t.u8(ver); t.u8(0); t.u8(0);
  t.u8((size >> 21) & 0x7F); t.u8((size >> 14) & 0x7F); t.u8((size >> 7) & 0x7F); t.u8(size & 0x7F);
  t.raw(frames.v.data(), frames.size());
  t.zeros(16);
  return std::move(t.v);
}
bool contains(const std::vector<uint8_t>& hay, const std::vector<uint8_t>& needle) {
  return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}
}  // namespace

SQ_TEST("codec-mp3: ID3 copy-through keeps unmapped frames when tags are also set (LAME only)") {
  auto* s = env().svc.get();
  if (!s->supports("mp3", nullptr, SQ_CODEC_WRITE)) {
    std::printf("LAME not found: ID3 copy-through skipped\n");
    return;
  }
  for (uint8_t ver : {uint8_t(3), uint8_t(4)}) {
    std::vector<uint8_t> raw = rawTag(ver);
    sq::Ptr<sq::IAudioFileWriter> w;
    REQUIRE_OK(s->createWriter("mp3", w.put()));
    auto stream = sq::makeMemoryStream();
    auto fmt = format(44100, 1, SQ_SAMPLE_F32);
    REQUIRE_OK(w->openStream(stream.get(), &fmt, nullptr));
    REQUIRE_OK(w->setMetadata("chunk:ID3 ", raw.data(), uint32_t(raw.size())));
    REQUIRE_OK(w->setMetadata("tag:title", "New", 3));
    REQUIRE_OK(writeAll(w.get(), signal(1, 20000, 44100)));
    REQUIRE_OK(w->finalize());
    auto rd = openMem(streamBytes(stream.get()));
    REQUIRE(rd);
    CHECK_EQ(metaText(rd.get(), "tag:title"), std::string("New"));
    auto id3 = metaValue(rd.get(), "chunk:ID3 ");
    std::vector<uint8_t> privBody(raw.begin() + 10 + 14 + 10, raw.begin() + 10 + 14 + 10 + 308);
    CHECK(contains(id3, privBody));  // the PRIV payload survived
    CHECK(!contains(id3, {'O', 'l', 'd'}));  // replaced title is not duplicated
  }
}

SQ_TEST("codec-mp3: writer refuses nonsense sample rates (LAME only)") {
  auto* s = env().svc.get();
  if (!s->supports("mp3", nullptr, SQ_CODEC_WRITE)) return;
  for (double sr : {0.0, -44100.0, std::nan(""), 1e12}) {
    sq::Ptr<sq::IAudioFileWriter> w;
    REQUIRE_OK(s->createWriter("mp3", w.put()));
    auto stream = sq::makeMemoryStream();
    auto fmt = format(sr, 2, SQ_SAMPLE_F32);
    CHECK_EQ(w->openStream(stream.get(), &fmt, nullptr), SQ_E_UNSUPPORTED);
  }
}

SQ_TEST("codec-mp3: crafted ID3 (huge extended header, odd UTF-16, nested CHAP) parses without harm") {
  auto base = fixture("mono_vbr_v23.mp3");
  REQUIRE(base.size() > 20);
  size_t skip = 10 + ((size_t(base[6] & 0x7F) << 21) | (size_t(base[7] & 0x7F) << 14) | (size_t(base[8] & 0x7F) << 7) | size_t(base[9] & 0x7F));
  REQUIRE(skip < base.size());
  std::vector<uint8_t> audio(base.begin() + static_cast<ptrdiff_t>(skip), base.end());
  sq::cx::Bytes f;
  // TIT2 in UTF-16 with BOM and an odd byte count
  f.fourcc("TIT2"); f.u32be(8); f.u16be(0); f.u8(1); f.u8(0xFF); f.u8(0xFE); f.u8('H'); f.u8(0); f.u8('i'); f.u8(0); f.u8(0x41);
  // CHAP with a frame size larger than the remaining tag
  f.fourcc("CHAP"); f.u32be(0x7FFFFFF0); f.u16be(0);
  const uint32_t size = static_cast<uint32_t>(f.size());
  sq::cx::Bytes t;
  t.raw("ID3", 3); t.u8(3); t.u8(0x40); t.u8(0);  // v2.3 with the extended-header flag
  t.u8((size >> 21) & 0x7F); t.u8((size >> 14) & 0x7F); t.u8((size >> 7) & 0x7F); t.u8(size & 0x7F);
  t.raw(f.v.data(), f.size());
  t.raw(audio.data(), audio.size());
  hammer(env(), t.v);
  // the same without the extended-header flag: the UTF-16 title decodes
  t.v[5] = 0;
  auto r = openMem(t.v);
  REQUIRE(r);
  CHECK_EQ(metaText(r.get(), "tag:title"), std::string("Hi"));
  CHECK_EQ(readAll(r.get(), 1)[0].size(), size_t(24000));
}
