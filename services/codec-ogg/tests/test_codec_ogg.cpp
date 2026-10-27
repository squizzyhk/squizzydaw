// codec-ogg tests: Vorbis decode of an ffmpeg/libvorbis fixture (length, comments, seek accuracy),
// Vorbis encoder (write -> read round trip within codec tolerance, tags, determinism, spill path,
// contract), robustness. The Opus decode seam has its own CTest (codec_ogg_opus) so that it can
// SKIP visibly when libopusfile is not installed.
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "sqcodec/test_util.hpp"

using namespace sqct;

namespace {

CodecEnv& env() {
  static CodecEnv e("codec-ogg", sq::codec::kOggCodecClassId);
  return e;
}
std::vector<uint8_t> fixture(const char* name) { return loadFile(sq::joinPath(SQ_CODEC_TEST_DATA, name)); }
sq::Ptr<sq::IAudioFileReader> openMem(const std::vector<uint8_t>& bytes, sq_result* res = nullptr) {
  auto r = env().reader();
  auto s = sq::makeMemoryStream(bytes.data(), bytes.size(), false);
  sq_result x = r ? r->openStream(s.get()) : SQ_E_FAIL;
  if (res) *res = x;
  if (SQ_FAILED(x)) return {};
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

// Ogg page CRC (independent implementation of the reference polynomial, bitwise).
uint32_t crcOgg(const uint8_t* p, size_t n) {
  uint32_t crc = 0;
  for (size_t i = 0; i < n; ++i) {
    crc ^= static_cast<uint32_t>(p[i]) << 24;
    for (int b = 0; b < 8; ++b) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : (crc << 1);
  }
  return crc;
}
// Every page complete, CRC-correct, one serial, sequence numbers 0,1,2,..., BOS first, EOS last.
bool pagesValid(const std::vector<uint8_t>& d, size_t* pageCount = nullptr) {
  size_t off = 0, n = 0;
  uint32_t serial = 0;
  bool eosSeen = false;
  while (off < d.size()) {
    if (off + 27 > d.size() || std::memcmp(d.data() + off, "OggS", 4) != 0 || eosSeen) return false;
    const uint8_t* h = d.data() + off;
    const size_t segs = h[26];
    if (off + 27 + segs > d.size()) return false;
    size_t body = 0;
    for (size_t i = 0; i < segs; ++i) body += h[27 + i];
    const size_t len = 27 + segs + body;
    if (off + len > d.size()) return false;
    std::vector<uint8_t> page(h, h + len);
    const uint32_t stored = sq::cx::rd32le(page.data() + 22);
    std::memset(page.data() + 22, 0, 4);
    if (crcOgg(page.data(), len) != stored) return false;
    if (n == 0) {
      serial = sq::cx::rd32le(h + 14);
      if (!(h[5] & 0x02)) return false;
    } else if (sq::cx::rd32le(h + 14) != serial || (h[5] & 0x02)) {
      return false;
    }
    if (sq::cx::rd32le(h + 18) != n) return false;
    eosSeen = (h[5] & 0x04) != 0;
    off += len;
    ++n;
  }
  if (pageCount) *pageCount = n;
  return n >= 3 && eosSeen;
}

using Tags = std::vector<std::pair<std::string, std::string>>;

// Encodes `c` to memory. Tags are set before the audio, or after it with late = true.
std::vector<uint8_t> encode(const Channels& c, double sr, sq::IAttributes* opts, const Tags& tags = {}, bool late = false,
                            sq_result* res = nullptr, uint32_t block = 1000) {
  auto w = env().writer("ogg");
  auto out = sq::makeMemoryStream();
  auto f = format(sr, static_cast<uint32_t>(c.size()), SQ_SAMPLE_F32);
  sq_result r = w ? w->openStream(out.get(), &f, opts) : SQ_E_FAIL;
  auto setTags = [&] {
    for (const auto& [k, v] : tags)
      if (SQ_SUCCEEDED(r)) r = w->setMetadata(k.c_str(), v.data(), static_cast<uint32_t>(v.size()));
  };
  if (!late) setTags();
  if (SQ_SUCCEEDED(r)) r = writeAll(w.get(), c, block);
  if (late) setTags();
  if (SQ_SUCCEEDED(r)) r = w->finalize();
  if (res) *res = r;
  if (SQ_FAILED(r)) return {};
  return streamBytes(out.get());
}

// Signal-to-noise ratio of b against the reference a (dB) over [from, to).
double snrDb(const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to) {
  double sig = 0, err = 0;
  for (size_t i = from; i < to && i < a.size() && i < b.size(); ++i) {
    sig += double(a[i]) * a[i];
    err += (double(a[i]) - b[i]) * (double(a[i]) - b[i]);
  }
  return err > 0 ? 10.0 * std::log10(sig / err) : 200.0;
}

// Lag (in [-maxLag, maxLag]) that best aligns b with a: 0 means no time shift.
int bestLag(const std::vector<float>& a, const std::vector<float>& b, size_t from, size_t to, int maxLag) {
  int best = 0;
  double bestC = -1e300;
  for (int lag = -maxLag; lag <= maxLag; ++lag) {
    double c = 0;
    for (size_t i = from; i < to; ++i) c += double(a[i]) * b[static_cast<size_t>(static_cast<int64_t>(i) + lag)];
    if (c > bestC) { bestC = c; best = lag; }
  }
  return best;
}

}  // namespace

SQ_TEST("codec-ogg: service loads, capability contract, probe") {
  REQUIRE(env().ok);
  auto* s = env().svc.get();
  CHECK(s->supports("ogg", nullptr, SQ_CODEC_READ));
  CHECK(s->supports("ogg", nullptr, SQ_CODEC_WRITE));
  CHECK(s->supports("oga", nullptr, SQ_CODEC_WRITE));
  CHECK(s->supports(nullptr, "audio/ogg", SQ_CODEC_WRITE));
  CHECK(!s->supports("opus", nullptr, SQ_CODEC_WRITE));  // no Opus encoder
  sq::Ptr<sq::IAudioFileWriter> w;
  CHECK_EQ(s->createWriter("opus", w.put()), SQ_E_UNSUPPORTED);
  CHECK(!w);
  CHECK_OK(s->createWriter("ogg", w.put()));
  CHECK(w);
  sq_codec_info ci{};
  ci.struct_size = sizeof ci;
  CHECK_OK(s->getCodecInfo(&ci));
  CHECK((ci.flags & SQ_CODEC_WRITE) != 0);
  auto v = fixture("tone.ogg");
  REQUIRE(v.size() > 100);
  CHECK_EQ(s->probe(v.data(), 64), 100);
  auto o = fixture("tone.opus");
  int32_t po = s->probe(o.data(), 64);
  CHECK(po > 0);
  CHECK_EQ(s->probe("RIFF", 4), 0);
}

SQ_TEST("codec-ogg: Vorbis decode, length, comments") {
  auto r = openMem(fixture("tone.ogg"));
  REQUIRE(r);
  auto f = fmtOf(r.get());
  CHECK_EQ(f.sample_rate, 44100.0);
  CHECK_EQ(f.channels, 2u);
  CHECK_EQ(f.frame_count, 44100);
  CHECK_EQ(std::string(f.container), std::string("ogg"));
  Channels c = readAll(r.get(), 2);
  REQUIRE_EQ(c[0].size(), size_t(44100));
  CHECK(std::abs(zeroCrossings(c[0], 2205, 41895) - 792) <= 6);
  CHECK(std::abs(zeroCrossings(c[1], 2205, 41895) - 1188) <= 6);
  CHECK_EQ(metaText(r.get(), "tag:title"), std::string("Squizzy Vorbis"));
  CHECK_EQ(metaText(r.get(), "tag:artist"), std::string("Squizzy Studio"));
  CHECK_EQ(metaText(r.get(), "tag:track"), std::string("3"));
  CHECK_EQ(metaText(r.get(), "tag:customthing"), std::string("yes"));
  CHECK(!metaText(r.get(), "tag:encoder").empty());
}

SQ_TEST("codec-ogg: Vorbis seeks are sample accurate") {
  auto r = openMem(fixture("tone.ogg"));
  REQUIRE(r);
  Channels full = readAll(r.get(), 2);
  REQUIRE_EQ(full[0].size(), size_t(44100));
  CHECK(seekMatches(r.get(), full, 40, 1e-5));
  CHECK_OK(r->seek(44100));
  CHECK(readFrames(r.get(), 2, 10)[0].empty());
  CHECK_OK(r->seek(0));
  CHECK_EQ(readFrames(r.get(), 2, 100)[0].size(), size_t(100));
  CHECK_EQ(r->seek(44101), SQ_E_INVALIDARG);
}

SQ_TEST("codec-ogg: Vorbis write -> read round trip (length exact, SNR, tags, seek)") {
  REQUIRE(env().ok);
  const uint32_t frames = 44100;
  Channels in = signal(2, frames, 44100.0);
  const std::string utf8 = "calm \xC3\xA9t\xC3\xA9";
  auto bytes = encode(in, 44100.0, nullptr,
                      {{"tag:title", "Round Trip"}, {"tag:artist", "Squizzy Studio"}, {"tag:track", "7"}, {"tag:mood", utf8}});
  REQUIRE(bytes.size() > 1000);
  dump("roundtrip.ogg", bytes);
  size_t pages = 0;
  CHECK(pagesValid(bytes, &pages));
  CHECK_EQ(env().svc->probe(bytes.data(), 64), 100);
  auto r = openMem(bytes);
  REQUIRE(r);
  auto f = fmtOf(r.get());
  CHECK_EQ(f.sample_rate, 44100.0);
  CHECK_EQ(f.channels, 2u);
  CHECK_EQ(f.frame_count, int64_t(frames));
  Channels out = readAll(r.get(), 2);
  REQUIRE_EQ(out[0].size(), size_t(frames));
  const double s0 = snrDb(in[0], out[0], 2048, frames - 2048), s1 = snrDb(in[1], out[1], 2048, frames - 2048);
  std::printf("  vorbis q0.5: %zu bytes, %zu pages, SNR L %.1f dB, R %.1f dB\n", bytes.size(), pages, s0, s1);
  CHECK(s0 > 20.0);
  CHECK(s1 > 20.0);
  CHECK_EQ(bestLag(in[0], out[0], 4096, frames - 4096, 64), 0);  // sample aligned, no codec delay
  CHECK_EQ(bestLag(in[1], out[1], 4096, frames - 4096, 64), 0);
  CHECK_EQ(metaText(r.get(), "tag:title"), std::string("Round Trip"));
  CHECK_EQ(metaText(r.get(), "tag:artist"), std::string("Squizzy Studio"));
  CHECK_EQ(metaText(r.get(), "tag:track"), std::string("7"));
  CHECK_EQ(metaText(r.get(), "tag:mood"), utf8);
  CHECK(seekMatches(r.get(), out, 20, 1e-5));
}

SQ_TEST("codec-ogg: Vorbis writer quality / bitrate / rates / channel counts") {
  Channels in = signal(1, 48000, 48000.0);
  auto lo = sq::makeAttributes();
  lo->setDouble("quality", 0.0);
  auto hi = sq::makeAttributes();
  hi->setDouble("quality", 1.0);
  auto bl = encode(in, 48000.0, lo.get()), bh = encode(in, 48000.0, hi.get());
  REQUIRE(!bl.empty());
  REQUIRE(!bh.empty());
  CHECK(bh.size() > bl.size());
  {
    auto rl = openMem(bl), rh = openMem(bh);
    REQUIRE(rl);
    REQUIRE(rh);
    Channels ol = readAll(rl.get(), 1), oh = readAll(rh.get(), 1);
    REQUIRE_EQ(ol[0].size(), size_t(48000));
    REQUIRE_EQ(oh[0].size(), size_t(48000));
    const double sl = snrDb(in[0], ol[0], 2048, 46000), sh = snrDb(in[0], oh[0], 2048, 46000);
    std::printf("  q0: %zu bytes SNR %.1f dB, q1: %zu bytes SNR %.1f dB\n", bl.size(), sl, bh.size(), sh);
    CHECK(sh > sl);
    CHECK(sl > 10.0);
  }
  auto br = sq::makeAttributes();
  br->setInt("bitrate_kbps", 96);
  Channels st = signal(2, 96000, 48000.0);
  auto bb = encode(st, 48000.0, br.get());
  REQUIRE(!bb.empty());
  const double kbps = bb.size() * 8.0 / 2.0 / 1000.0;
  std::printf("  managed 96 kbps -> %.1f kbps actual\n", kbps);
  CHECK(kbps > 48.0 && kbps < 160.0);
  for (double sr : {22050.0, 32000.0, 96000.0}) {
    Channels x = signal(2, static_cast<uint32_t>(sr / 2), sr);
    auto b = encode(x, sr, nullptr);
    REQUIRE(!b.empty());
    CHECK(pagesValid(b));
    auto r = openMem(b);
    REQUIRE(r);
    auto f = fmtOf(r.get());
    CHECK_EQ(f.sample_rate, sr);
    CHECK_EQ(f.frame_count, int64_t(sr / 2));
  }
  Channels six = signal(6, 24000, 48000.0);
  auto b6 = encode(six, 48000.0, nullptr);
  dump("six.ogg", b6);
  REQUIRE(!b6.empty());
  auto r6 = openMem(b6);
  REQUIRE(r6);
  CHECK_EQ(fmtOf(r6.get()).channels, 6u);
  Channels o6 = readAll(r6.get(), 6);
  REQUIRE_EQ(o6[5].size(), size_t(24000));
  // channels 0-4 are full band and come back in place (no reordering); the 6th is LFE in the Vorbis
  // 5.1 order and is low-passed by the encoder, so it is not compared.
  for (int c = 0; c < 5; ++c) {
    const double s6 = snrDb(six[c], o6[c], 2048, 22000);
    std::printf("  6 ch: channel %d SNR %.1f dB\n", c, s6);
    CHECK(s6 > 25.0);
  }
  // empty stream (finalize without audio) is still a valid file
  sq_result res = SQ_OK;
  auto empty = encode(Channels(2), 44100.0, nullptr, {}, false, &res);
  CHECK_OK(res);
  CHECK(pagesValid(empty));
}

SQ_TEST("codec-ogg: Vorbis output is deterministic; late tags land in the header") {
  Channels in = signal(2, 30000, 44100.0);
  auto a = encode(in, 44100.0, nullptr, {{"tag:title", "Same"}});
  auto b = encode(in, 44100.0, nullptr, {{"tag:title", "Same"}});
  REQUIRE(!a.empty());
  CHECK(a == b);
  // A long tag set after the audio needs more header pages: the audio pages get renumbered.
  std::string big(100000, 'x');  // > 255 lacing values: needs extra header pages
  auto late = encode(in, 44100.0, nullptr, {{"tag:title", "Late"}, {"tag:comment", big}}, true);
  REQUIRE(!late.empty());
  size_t pa = 0, pl = 0;
  CHECK(pagesValid(a, &pa));
  CHECK(pagesValid(late, &pl));
  CHECK(pl > pa);
  auto r = openMem(late);
  REQUIRE(r);
  CHECK_EQ(metaText(r.get(), "tag:title"), std::string("Late"));
  CHECK_EQ(metaText(r.get(), "tag:comment"), big);
  CHECK_EQ(fmtOf(r.get()).frame_count, int64_t(30000));
  CHECK_EQ(readAll(r.get(), 2)[0].size(), size_t(30000));
  // removing a tag (size 0) before finalize drops it
  auto removed = encode(in, 44100.0, nullptr, {{"tag:title", "Gone"}, {"tag:title", ""}});
  auto r2 = openMem(removed);
  REQUIRE(r2);
  CHECK(metaText(r2.get(), "tag:title").empty());
}

SQ_TEST("codec-ogg: Vorbis spill path (headers committed early) is identical; later tags refused") {
  Channels in = signal(2, 88200, 44100.0);
  auto opts = sq::makeAttributes();
  opts->setInt("ogg.spill_bytes", 2000);
  auto w = env().writer("ogg");
  auto out = sq::makeMemoryStream();
  auto f = format(44100.0, 2, SQ_SAMPLE_F32);
  REQUIRE(w);
  CHECK_OK(w->openStream(out.get(), &f, opts.get()));
  const std::string t = "Early";
  CHECK_OK(w->setMetadata("tag:title", t.data(), static_cast<uint32_t>(t.size())));
  CHECK_OK(writeAll(w.get(), in, 1000));
  CHECK_EQ(w->setMetadata("tag:artist", "x", 1), SQ_E_UNSUPPORTED);
  CHECK_OK(w->finalize());
  auto bytes = streamBytes(out.get());
  CHECK(pagesValid(bytes));
  auto r = openMem(bytes);
  REQUIRE(r);
  CHECK_EQ(metaText(r.get(), "tag:title"), t);
  CHECK_EQ(fmtOf(r.get()).frame_count, int64_t(88200));
  auto buffered = encode(in, 44100.0, nullptr, {{"tag:title", t}});
  CHECK(bytes == buffered);
}

SQ_TEST("codec-ogg: Vorbis writer argument and lifecycle contract") {
  auto w = env().writer("ogg");
  REQUIRE(w);
  float* none[2] = {nullptr, nullptr};
  sq_audio_block blk{none, 2, 0, 0};
  CHECK_EQ(w->write(&blk), SQ_E_CLOSED);
  CHECK_EQ(w->finalize(), SQ_E_CLOSED);
  auto out = sq::makeMemoryStream();
  auto bad = format(44100.0, 0, SQ_SAMPLE_F32);
  CHECK(SQ_FAILED(w->openStream(out.get(), &bad, nullptr)));
  bad = format(0.0, 2, SQ_SAMPLE_F32);
  CHECK(SQ_FAILED(w->openStream(out.get(), &bad, nullptr)));
  bad = format(44100.0, 9, SQ_SAMPLE_F32);
  CHECK_EQ(w->openStream(out.get(), &bad, nullptr), SQ_E_UNSUPPORTED);
  auto f = format(44100.0, 2, SQ_SAMPLE_F32);
  CHECK_OK(w->openStream(out.get(), &f, nullptr));
  CHECK_EQ(w->openStream(out.get(), &f, nullptr), SQ_E_EXISTS);
  CHECK_EQ(w->setMetadata("chunk:bext", "abcd", 4), SQ_E_UNSUPPORTED);
  CHECK_EQ(w->setMetadata("tag:a=b", "x", 1), SQ_E_INVALIDARG);
  CHECK_EQ(w->setMetadata("tag:x", "a\0b", 3), SQ_E_INVALIDARG);
  CHECK_EQ(w->setMetadata("tag:x", "\xFF\xFE", 2), SQ_E_INVALIDARG);  // not UTF-8
  sq_audio_file_marker m = sq::cx::makeMarker(1, 10, 0, 0, "m");
  CHECK_EQ(w->addMarker(&m), SQ_E_UNSUPPORTED);
  Channels mono = signal(1, 100);
  CHECK_EQ(writeAll(w.get(), mono), SQ_E_INVALIDARG);  // wrong channel count
  // NaN / huge samples do not break the encoder
  Channels odd = signal(2, 4000, 44100.0);
  odd[0][100] = std::nanf("");
  odd[1][200] = 1e30f;
  CHECK_OK(writeAll(w.get(), odd));
  CHECK_OK(w->finalize());
  CHECK_EQ(w->finalize(), SQ_S_FALSE);
  CHECK_EQ(w->write(&blk), SQ_E_CLOSED);
  CHECK_EQ(w->setMetadata("tag:late", "x", 1), SQ_E_CLOSED);
  auto r = openMem(streamBytes(out.get()));
  REQUIRE(r);
  CHECK_EQ(fmtOf(r.get()).frame_count, int64_t(4000));
}

SQ_TEST("codec-ogg: path writer keeps the file on finalize, deletes it on abort") {
  std::string dir = sq::joinPath(sq::specialDir(SQ_DIR_EXE), "codec_ogg_tmp");
  sq_fs_create_dirs(dir.c_str());
  std::string keep = sq::joinPath(dir, "keep.ogg"), drop = sq::joinPath(dir, "drop.ogg");
  auto f = format(44100.0, 2, SQ_SAMPLE_F32);
  Channels in = signal(2, 20000, 44100.0);
  {
    auto w = env().writer("ogg");
    REQUIRE(w);
    CHECK_OK(w->open(keep.c_str(), &f, nullptr));
    CHECK_OK(writeAll(w.get(), in));
    CHECK_OK(w->finalize());
  }
  {
    auto w = env().writer("ogg");
    REQUIRE(w);
    CHECK_OK(w->open(drop.c_str(), &f, nullptr));
    CHECK_OK(writeAll(w.get(), in));
  }  // released without finalize: aborted
  CHECK(sq_fs_is_file(keep.c_str()));
  CHECK(!sq_fs_is_file(drop.c_str()));
  {
    auto r = env().reader();
    REQUIRE(r);
    CHECK_OK(r->open(keep.c_str()));
    CHECK_EQ(fmtOf(r.get()).frame_count, int64_t(20000));
  }
  sq_fs_remove(keep.c_str());
  sq_fs_remove(dir.c_str());
}

SQ_TEST("codec-ogg: Opus without libopusfile is a clean SQ_E_UNSUPPORTED") {
  if (env().svc->supports("opus", nullptr, SQ_CODEC_READ)) return;  // decode path: CTest codec_ogg_opus
  sq_result res = SQ_OK;
  auto r = openMem(fixture("tone.opus"), &res);
  CHECK(!r);
  CHECK_EQ(res, SQ_E_UNSUPPORTED);
}

SQ_TEST("codec-ogg: truncated / corrupted files never crash") {
  fuzz(env(), fixture("tone.ogg"), 400, 11);
  fuzz(env(), fixture("tone.opus"), 100, 12);
  CHECK(true);
}

SQ_TEST("codec-ogg: setup header with a codebook that fails init is an error, not a crash on seek") {
  // Regression (third_party/libvorbis/PATCHES.md): tone.ogg with corrupted setup bytes and fixed
  // page CRCs. Before the patch, the seek retried decoder init with zeroed codebooks (div by 0).
  auto bad = fixture("bad_books.ogg");
  REQUIRE(bad.size() > 100);
  sq_result res = SQ_OK;
  auto r = openMem(bad, &res);
  if (r) {
    auto f = fmtOf(r.get());
    (void)readAll(r.get(), f.channels);
    (void)r->seek(f.frame_count / 2);  // must return (either way), not crash
    (void)r->seek(0);
    (void)readAll(r.get(), f.channels);
  }
  hammer(env(), bad);
  CHECK(true);  // reaching here is the assertion
}

SQ_TEST("codec-ogg: corrupted payloads with valid page CRCs (decoder-level) never crash") {
  auto fixCrcs = [](std::vector<uint8_t>& d) {
    size_t off = 0;
    while (off + 27 <= d.size() && !std::memcmp(d.data() + off, "OggS", 4)) {
      size_t segs = d[off + 26], body = 0;
      if (off + 27 + segs > d.size()) break;
      for (size_t i = 0; i < segs; ++i) body += d[off + 27 + i];
      size_t len = 27 + segs + body;
      if (off + len > d.size()) break;
      std::memset(d.data() + off + 22, 0, 4);
      sq::cx::wr32le(d.data() + off + 22, crcOgg(d.data() + off, len));
      off += len;
    }
  };
  auto valid = fixture("tone.ogg");
  uint32_t st = 4242;
  auto rnd = [&]() { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; };
  const bool verbose = !sq::cx::envVar("SQ_CODEC_FUZZ_VERBOSE").empty();
  int scale = std::atoi(sq::cx::envVar("SQ_CODEC_FUZZ_SCALE").c_str());
  if (scale <= 0) scale = 1;
  for (int it = 0; it < 400 * scale; ++it) {
    std::vector<uint8_t> b = valid;
    int flips = 1 + static_cast<int>(rnd() % 6);
    for (int f = 0; f < flips; ++f) {
      size_t at = rnd() % b.size();
      if (at >= 22 && at < 27) continue;  // keep page framing parseable
      b[at] = static_cast<uint8_t>(rnd());
    }
    fixCrcs(b);
    if (verbose) { std::printf("crc-fuzz %d\n", it); std::fflush(stdout); dump("crcfuzz_last.bin", b); }
    hammer(env(), b);
  }
  CHECK(true);
}

// ---- verifier hardening (adversarial review) ------------------------------------------------------

SQ_TEST("codec-ogg: empty and one-frame encodes read back with their exact length") {
  for (uint32_t frames : {0u, 1u, 7u}) {
    Channels in(2, std::vector<float>(frames, 0.25f));
    sq_result res = SQ_OK;
    auto bytes = frames ? encode(in, 44100.0, nullptr, {{"tag:title", "Short"}}, false, &res)
                        : [&] {
                            auto w = env().writer("ogg");
                            auto out = sq::makeMemoryStream();
                            auto f = format(44100.0, 2, SQ_SAMPLE_F32);
                            res = w ? w->openStream(out.get(), &f, nullptr) : SQ_E_FAIL;
                            if (SQ_SUCCEEDED(res)) res = w->finalize();
                            return SQ_SUCCEEDED(res) ? streamBytes(out.get()) : std::vector<uint8_t>{};
                          }();
    CHECK_OK(res);
    REQUIRE(!bytes.empty());
    CHECK(pagesValid(bytes));
    sq_result open = SQ_OK;
    auto r = openMem(bytes, &open);
    if (!r) std::printf("frames %u: open failed %d\n", frames, static_cast<int>(open));
    REQUIRE(r);
    CHECK_EQ(fmtOf(r.get()).frame_count, static_cast<int64_t>(frames));
    auto back = readAll(r.get(), 2);
    CHECK_EQ(back[0].size(), static_cast<size_t>(frames));
    CHECK_OK(r->seek(0));
  }
}

SQ_TEST("codec-ogg: low rates and mono encode; a writer whose open failed can be opened again") {
  for (double sr : {8000.0, 11025.0, 16000.0}) {
    Channels in = signal(1, static_cast<uint32_t>(sr), sr);
    sq_result res = SQ_OK;
    auto bytes = encode(in, sr, nullptr, {}, false, &res);
    CHECK_OK(res);
    auto r = openMem(bytes);
    REQUIRE(r);
    auto f = fmtOf(r.get());
    CHECK_EQ(f.sample_rate, sr);
    CHECK_EQ(f.frame_count, static_cast<int64_t>(sr));
  }
  auto w = env().writer("ogg");
  REQUIRE(w);
  auto f = format(44100.0, 2, SQ_SAMPLE_F32);
  std::string bad = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "codec_ogg_no_such_dir_\x01"), "x.ogg");
  CHECK(SQ_FAILED(w->open(bad.c_str(), &f, nullptr)));
  auto out = sq::makeMemoryStream();
  CHECK_OK(w->openStream(out.get(), &f, nullptr));
  CHECK_OK(writeAll(w.get(), signal(2, 5000, 44100.0)));
  CHECK_OK(w->finalize());
  auto r = openMem(streamBytes(out.get()));
  REQUIRE(r);
  CHECK_EQ(fmtOf(r.get()).frame_count, int64_t(5000));
}
