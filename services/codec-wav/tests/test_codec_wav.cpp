// codec-wav tests: round trips, RF64, chunk preservation, markers, tags, seek, robustness.
#include <cstring>

#include "sqcodec/test_util.hpp"

using namespace sqct;

namespace {

CodecEnv& env() {
  static CodecEnv e("codec-wav", sq::codec::kWavCodecClassId);
  return e;
}

std::string tempPath(const char* name) {
  std::string d = sq::joinPath(sq::specialDir(SQ_DIR_TEMP), "sqtest-codec-wav");
  sq_fs_create_dirs(d.c_str());
  return sq::joinPath(d, name);
}

struct Written {
  sq::Ptr<sq::IByteStream> stream;
  std::vector<uint8_t> bytes;
};

Written writeMem(const char* ext, const sq_audio_file_format& f, const Channels& c, sq::IAttributes* opts = nullptr) {
  Written w;
  w.stream = sq::makeMemoryStream();
  auto wr = env().writer(ext);
  REQUIRE(wr);
  REQUIRE_OK(wr->openStream(w.stream.get(), &f, opts));
  REQUIRE_OK(writeAll(wr.get(), c));
  REQUIRE_OK(wr->finalize());
  w.bytes = streamBytes(w.stream.get());
  static int n = 0;
  dump((std::string("wav_") + std::to_string(n++) + "." + ext).c_str(), w.bytes);
  return w;
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

void roundTrip(const char* ext, sq_sample_format sf, uint32_t bits, uint32_t ch, double tol, const char* container) {
  Channels in = signal(ch, 4801, 44100);
  auto f = format(44100, ch, sf, bits);
  Written w = writeMem(ext, f, in);
  auto r = openMem(w.bytes);
  REQUIRE(r);
  auto rf = fmtOf(r.get());
  CHECK_EQ(rf.channels, ch);
  CHECK_EQ(rf.sample_rate, 44100.0);
  CHECK_EQ(rf.frame_count, 4801);
  CHECK_EQ(std::string(rf.container), std::string(container));
  if (bits) CHECK_EQ(rf.bits_per_sample, bits);
  CHECK((rf.flags & SQ_AUDIOFILE_SAMPLE_EXACT) != 0);
  Channels out = readAll(r.get(), ch);
  REQUIRE_EQ(out[0].size(), size_t(4801));
  double d = maxDiff(in, out);
  if (!(d <= tol)) std::printf("%s fmt %d bits %u: diff %g > %g\n", ext, int(sf), bits, d, tol);
  CHECK(d <= tol);
}

}  // namespace

SQ_TEST("codec-wav: service loads, describes itself, answers supports/probe") {
  REQUIRE(env().ok);
  auto* s = env().svc.get();
  sq_codec_info ci{};
  ci.struct_size = sizeof ci;
  REQUIRE_OK(s->getCodecInfo(&ci));
  CHECK_EQ(std::string(ci.name), std::string("WAV/AIFF"));
  CHECK(s->getFormatCount() >= 5);
  CHECK(s->supports("wav", nullptr, SQ_CODEC_READ | SQ_CODEC_WRITE));
  CHECK(s->supports(".AIFF", nullptr, SQ_CODEC_WRITE));
  CHECK(s->supports(nullptr, "audio/wav", SQ_CODEC_READ));
  CHECK(!s->supports("flac", nullptr, SQ_CODEC_READ));
  CHECK(!s->supports(nullptr, nullptr, SQ_CODEC_READ));
  CHECK_EQ(s->probe("RIFF\0\0\0\0WAVEfmt ", 16), 100);
  CHECK_EQ(s->probe("FORM\0\0\0\0AIFC", 12), 100);
  CHECK_EQ(s->probe("fLaC\0\0\0\0\0\0\0\0", 12), 0);
  CHECK_EQ(s->probe("RIFF", 4), 0);
  sq::Ptr<sq::IAudioFileWriter> w;
  CHECK_EQ(s->createWriter("mp3", w.put()), SQ_E_UNSUPPORTED);
  // findCodec through the registry
  sq::ICodecService* found = sq::codec::findCodec(&env().reg, "wav", nullptr, SQ_CODEC_WRITE);
  REQUIRE(found != nullptr);
  found->release();
}

SQ_TEST("codec-wav: WAV round trips 8/16/20/24/32-int and 32/64-float") {
  roundTrip("wav", SQ_SAMPLE_I16, 8, 2, 2.0 / 128, "wav");
  roundTrip("wav", SQ_SAMPLE_I16, 0, 2, 1.0 / 32768 + 1e-7, "wav");
  roundTrip("wav", SQ_SAMPLE_I24, 20, 2, 1.0 / 524288 + 1e-7, "wav");
  roundTrip("wav", SQ_SAMPLE_I24, 0, 1, 1.0 / 8388608 + 1e-7, "wav");
  roundTrip("wav", SQ_SAMPLE_I32, 0, 2, 1e-7, "wav");
  roundTrip("wav", SQ_SAMPLE_F32, 0, 2, 0.0, "wav");
  roundTrip("wav", SQ_SAMPLE_F64, 0, 3, 0.0, "wav");
  roundTrip("wav", SQ_SAMPLE_I24, 0, 6, 1.0 / 8388608 + 1e-7, "wav");  // WAVE_FORMAT_EXTENSIBLE
}

SQ_TEST("codec-wav: AIFF / AIFF-C round trips") {
  roundTrip("aif", SQ_SAMPLE_I16, 0, 2, 1.0 / 32768 + 1e-7, "aif");
  roundTrip("aiff", SQ_SAMPLE_I24, 0, 2, 1.0 / 8388608 + 1e-7, "aif");
  roundTrip("aif", SQ_SAMPLE_I32, 0, 1, 1e-7, "aif");
  roundTrip("aif", SQ_SAMPLE_I16, 8, 1, 2.0 / 128, "aif");
  roundTrip("aif", SQ_SAMPLE_F32, 0, 2, 0.0, "aifc");  // float upgrades to AIFF-C
  roundTrip("aifc", SQ_SAMPLE_F64, 0, 2, 0.0, "aifc");
  roundTrip("aifc", SQ_SAMPLE_I16, 0, 2, 1.0 / 32768 + 1e-7, "aifc");
}

SQ_TEST("codec-wav: extensible header carries channel mask and valid bits") {
  Channels in = signal(6, 100);
  Written w = writeMem("wav", format(48000, 6, SQ_SAMPLE_I24), in);
  const auto& b = w.bytes;
  // RIFF(12) JUNK(8+28) fmt header at 48
  REQUIRE(b.size() > 100);
  CHECK(!std::memcmp(b.data() + 48, "fmt ", 4));
  CHECK_EQ(sq::cx::rd16le(b.data() + 56), 0xFFFEu);
  CHECK_EQ(sq::cx::rd16le(b.data() + 56 + 18), 24u);
  CHECK_EQ(sq::cx::rd32le(b.data() + 56 + 20), 0x3Fu);
}

SQ_TEST("codec-wav: RF64 forced by option, ds64 sizes read back") {
  Channels in = signal(2, 3000);
  auto opts = sq::makeAttributes();
  opts->setBool("rf64", SQ_TRUE);
  Written w = writeMem("wav", format(48000, 2, SQ_SAMPLE_I16), in, opts.get());
  REQUIRE(w.bytes.size() > 48);
  CHECK(!std::memcmp(w.bytes.data(), "RF64", 4));
  CHECK_EQ(sq::cx::rd32le(w.bytes.data() + 4), 0xFFFFFFFFu);
  CHECK(!std::memcmp(w.bytes.data() + 12, "ds64", 4));
  CHECK_EQ(sq::cx::rd64le(w.bytes.data() + 28), uint64_t(3000 * 4));
  auto r = openMem(w.bytes);
  REQUIRE(r);
  auto f = fmtOf(r.get());
  CHECK_EQ(std::string(f.container), std::string("rf64"));
  CHECK_EQ(f.frame_count, 3000);
  CHECK(maxDiff(in, readAll(r.get(), 2)) <= 1.0 / 32768 + 1e-7);
  // rf64=false: no JUNK reservation, plain RIFF
  opts->setBool("rf64", SQ_FALSE);
  Written w2 = writeMem("wav", format(48000, 2, SQ_SAMPLE_I16), in, opts.get());
  CHECK(!std::memcmp(w2.bytes.data(), "RIFF", 4));
  CHECK(!std::memcmp(w2.bytes.data() + 12, "fmt ", 4));
  CHECK_EQ(sq::cx::rd32le(w2.bytes.data() + 4), uint32_t(w2.bytes.size() - 8));
}

SQ_TEST("codec-wav: unknown, bext, iXML and CHAN chunks preserved byte-exact (pre and post data)") {
  std::vector<uint8_t> ixml = {'<', 'B', 'W', 'F', 'X', 'M', 'L', '/', '>'};  // odd size: needs a pad byte
  std::vector<uint8_t> custom(1001);
  for (size_t i = 0; i < custom.size(); ++i) custom[i] = static_cast<uint8_t>(i * 7);
  std::vector<uint8_t> bext(602 + 12, 0);
  std::memcpy(bext.data(), "a description", 13);
  std::memcpy(bext.data() + 256, "SquizzyRig", 10);
  std::memcpy(bext.data() + 320, "2026-10-27", 10);
  std::memcpy(bext.data() + 330, "12:34:56", 8);
  std::memcpy(bext.data() + 602, "A=PCM,F=48k", 11);
  uint8_t chan[12] = {0, 1, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0};  // use-bitmap, FL|FR

  Channels in = signal(2, 2000);
  auto stream = sq::makeMemoryStream();
  auto wr = env().writer("wav");
  REQUIRE_OK(wr->openStream(stream.get(), &(const sq_audio_file_format&)format(48000, 2, SQ_SAMPLE_I24), nullptr));
  REQUIRE_OK(wr->setMetadata("chunk:iXML", ixml.data(), uint32_t(ixml.size())));
  REQUIRE_OK(wr->setMetadata("chunk:bext", bext.data(), uint32_t(bext.size())));
  REQUIRE_OK(wr->setMetadata("chunk:CHAN", chan, sizeof chan));
  CHECK_EQ(wr->setMetadata("chunk:fmt ", chan, 4), SQ_E_INVALIDARG);  // structural ids are refused
  CHECK_EQ(wr->setMetadata("nonsense", chan, 4), SQ_E_UNSUPPORTED);
  REQUIRE_OK(writeAll(wr.get(), in));
  REQUIRE_OK(wr->setMetadata("chunk:zz9 ", custom.data(), uint32_t(custom.size())));  // after audio started
  REQUIRE_OK(wr->finalize());
  auto bytes = streamBytes(stream.get());

  auto r = openMem(bytes);
  REQUIRE(r);
  CHECK(metaValue(r.get(), "chunk:iXML") == ixml);
  CHECK(metaValue(r.get(), "chunk:zz9 ") == custom);
  CHECK(metaValue(r.get(), "chunk:bext") == bext);
  CHECK(metaValue(r.get(), "chunk:CHAN") == std::vector<uint8_t>(chan, chan + 12));
  CHECK_EQ(metaText(r.get(), "tag:originator"), std::string("SquizzyRig"));
  CHECK_EQ(metaText(r.get(), "tag:origination_date"), std::string("2026-10-27 12:34:56"));
  CHECK(maxDiff(in, readAll(r.get(), 2)) <= 1.0 / 8388608 + 1e-7);

  // copy-through: every key the reader exposes goes into a new writer unchanged
  auto stream2 = sq::makeMemoryStream();
  auto wr2 = env().writer("wav");
  REQUIRE_OK(wr2->openStream(stream2.get(), &(const sq_audio_file_format&)format(48000, 2, SQ_SAMPLE_I24), nullptr));
  for (uint32_t i = 0; i < r->getMetadataCount(); ++i) {
    std::string k = metaKey(r.get(), i);
    auto v = metaValue(r.get(), k.c_str());
    REQUIRE_OK(wr2->setMetadata(k.c_str(), v.data(), uint32_t(v.size())));
  }
  REQUIRE_OK(writeAll(wr2.get(), in));
  REQUIRE_OK(wr2->finalize());
  auto r2 = openMem(streamBytes(stream2.get()));
  REQUIRE(r2);
  REQUIRE_EQ(r2->getMetadataCount(), r->getMetadataCount());
  for (uint32_t i = 0; i < r->getMetadataCount(); ++i) {
    std::string k = metaKey(r.get(), i);
    CHECK(metaValue(r2.get(), k.c_str()) == metaValue(r.get(), k.c_str()));
  }
}

SQ_TEST("codec-wav: pre-data chunk changed later is moved, JUNKed original is hidden") {
  Channels in = signal(1, 500);
  auto stream = sq::makeMemoryStream();
  auto wr = env().writer("wav");
  REQUIRE_OK(wr->openStream(stream.get(), &(const sq_audio_file_format&)format(48000, 1, SQ_SAMPLE_I16), nullptr));
  REQUIRE_OK(wr->setMetadata("chunk:abcd", "1234", 4));
  REQUIRE_OK(wr->setMetadata("chunk:efgh", "xy", 2));
  REQUIRE_OK(wr->setMetadata("chunk:gone", "zz", 2));
  REQUIRE_OK(writeAll(wr.get(), in));
  REQUIRE_OK(wr->setMetadata("chunk:abcd", "123456789", 9));  // grows: re-emitted after data
  REQUIRE_OK(wr->setMetadata("chunk:efgh", "XY", 2));         // same size: patched in place
  REQUIRE_OK(wr->setMetadata("chunk:gone", nullptr, 0));      // removed
  REQUIRE_OK(wr->finalize());
  auto r = openMem(streamBytes(stream.get()));
  REQUIRE(r);
  CHECK_EQ(metaText(r.get(), "chunk:abcd"), std::string("123456789"));
  CHECK_EQ(metaText(r.get(), "chunk:efgh"), std::string("XY"));
  CHECK(metaValue(r.get(), "chunk:gone").empty());
  CHECK_EQ(r->getMetadataCount(), 2u);
  CHECK(maxDiff(in, readAll(r.get(), 1)) <= 1.0 / 32768 + 1e-7);
}

SQ_TEST("codec-wav: markers, regions and loops round trip (WAV cue/adtl/smpl and AIFF MARK/INST)") {
  for (const char* ext : {"wav", "aif"}) {
    Channels in = signal(2, 48000);
    auto stream = sq::makeMemoryStream();
    auto wr = env().writer(ext);
    REQUIRE_OK(wr->openStream(stream.get(), &(const sq_audio_file_format&)format(48000, 2, SQ_SAMPLE_I16), nullptr));
    auto mk = [](uint32_t id, sq_sample_time pos, sq_sample_time len, uint32_t flags, const char* name) {
      return sq::cx::makeMarker(id, pos, len, flags, name);
    };
    auto m1 = mk(1, 100, 0, 0, "Verse");
    auto m2 = mk(2, 12000, 6000, 0, "Chorus region");
    auto m3 = mk(7, 30000, 4410, SQ_MARKER_LOOP, "Loop A");
    const bool wav = !std::strcmp(ext, "wav");
    REQUIRE_OK(wr->addMarker(&m1));
    if (wav) REQUIRE_OK(wr->addMarker(&m2));
    REQUIRE_OK(wr->addMarker(&m3));
    REQUIRE_OK(writeAll(wr.get(), in));
    REQUIRE_OK(wr->finalize());
    dump((std::string("markers.") + ext).c_str(), streamBytes(stream.get()));
    auto r = openMem(streamBytes(stream.get()));
    REQUIRE(r);
    REQUIRE_EQ(r->getMarkerCount(), wav ? 3u : 2u);
    std::vector<sq_audio_file_marker> got(r->getMarkerCount());
    for (uint32_t i = 0; i < got.size(); ++i) {
      got[i].struct_size = sizeof got[i];
      REQUIRE_OK(r->getMarker(i, &got[i]));
    }
    CHECK_EQ(got[0].position, 100);
    CHECK_EQ(got[0].length, 0);
    CHECK_EQ(std::string(got[0].name), std::string("Verse"));
    if (wav) {
      CHECK_EQ(got[1].position, 12000);
      CHECK_EQ(got[1].length, 6000);
      CHECK(got[1].flags & SQ_MARKER_REGION);
      CHECK(!(got[1].flags & SQ_MARKER_LOOP));
      CHECK_EQ(std::string(got[1].name), std::string("Chorus region"));
    }
    const auto& lp = got.back();
    CHECK_EQ(lp.position, 30000);
    CHECK_EQ(lp.length, 4410);
    CHECK((lp.flags & (SQ_MARKER_LOOP | SQ_MARKER_REGION)) == (SQ_MARKER_LOOP | SQ_MARKER_REGION));
    CHECK_EQ(std::string(lp.name), std::string("Loop A"));
    CHECK_EQ(lp.id, 7u);
  }
}

SQ_TEST("codec-wav: tags map to LIST/INFO (WAV) and NAME/AUTH (AIFF) and back") {
  for (const char* ext : {"wav", "aif"}) {
    auto stream = sq::makeMemoryStream();
    auto wr = env().writer(ext);
    REQUIRE_OK(wr->openStream(stream.get(), &(const sq_audio_file_format&)format(44100, 1, SQ_SAMPLE_I16), nullptr));
    REQUIRE_OK(wr->setMetadata("tag:title", "Night Take 3", 12));
    REQUIRE_OK(wr->setMetadata("tag:artist", "The Squizzies", 13));
    REQUIRE_OK(wr->setMetadata("tag:copyright", "(c) 2026", 8));
    if (!std::strcmp(ext, "wav")) {
      REQUIRE_OK(wr->setMetadata("tag:genre", "Ambient", 7));
      REQUIRE_OK(wr->setMetadata("tag:originator", "Desk A", 6));
    } else {
      CHECK_EQ(wr->setMetadata("tag:genre", "Ambient", 7), SQ_E_UNSUPPORTED);
    }
    REQUIRE_OK(writeAll(wr.get(), signal(1, 100)));
    REQUIRE_OK(wr->finalize());
    auto r = openMem(streamBytes(stream.get()));
    REQUIRE(r);
    CHECK_EQ(metaText(r.get(), "tag:title"), std::string("Night Take 3"));
    CHECK_EQ(metaText(r.get(), "tag:artist"), std::string("The Squizzies"));
    CHECK_EQ(metaText(r.get(), "tag:copyright"), std::string("(c) 2026"));
    if (!std::strcmp(ext, "wav")) {
      CHECK_EQ(metaText(r.get(), "tag:genre"), std::string("Ambient"));
      CHECK_EQ(metaText(r.get(), "tag:originator"), std::string("Desk A"));
      CHECK_EQ(metaValue(r.get(), "chunk:bext").size(), size_t(602));
    }
  }
}

SQ_TEST("codec-wav: seeks are sample accurate") {
  for (const char* ext : {"wav", "aif"}) {
    Channels in = signal(2, 20000);
    Written w = writeMem(ext, format(48000, 2, SQ_SAMPLE_F32), in);
    auto r = openMem(w.bytes);
    REQUIRE(r);
    Channels full = readAll(r.get(), 2);
    CHECK(seekMatches(r.get(), full, 50, 0.0));
    CHECK_EQ(r->seek(20000), SQ_OK);  // end is a valid position
    CHECK_EQ(r->seek(20001), SQ_E_INVALIDARG);
    CHECK_EQ(r->seek(-1), SQ_E_INVALIDARG);
  }
}

SQ_TEST("codec-wav: file writer by path; release without finalize deletes the file") {
  std::string p = tempPath("abort.wav");
  sq_fs_remove(p.c_str());
  {
    auto wr = env().writer("wav");
    REQUIRE_OK(wr->open(p.c_str(), &(const sq_audio_file_format&)format(48000, 1, SQ_SAMPLE_I16), nullptr));
    REQUIRE_OK(writeAll(wr.get(), signal(1, 1000)));
    CHECK(sq_fs_exists(p.c_str()));
  }
  CHECK(!sq_fs_exists(p.c_str()));
  std::string q = tempPath("keep.aif");
  {
    auto wr = env().writer("aif");
    REQUIRE_OK(wr->open(q.c_str(), &(const sq_audio_file_format&)format(48000, 1, SQ_SAMPLE_I16), nullptr));
    REQUIRE_OK(writeAll(wr.get(), signal(1, 1000)));
    REQUIRE_OK(wr->finalize());
    CHECK_EQ(wr->write(nullptr), SQ_E_CLOSED);
  }
  auto r = env().reader();
  REQUIRE_OK(r->open(q.c_str()));
  CHECK_EQ(fmtOf(r.get()).frame_count, 1000);
  r = nullptr;
  sq_fs_remove(q.c_str());
}

SQ_TEST("codec-wav: hand-made files (8-bit, AIFC sowt, data before fmt, streaming size 0)") {
  // 8-bit unsigned mono WAV, 4 frames
  sq::cx::Bytes b;
  b.fourcc("RIFF"); b.u32le(36 + 4); b.fourcc("WAVE");
  b.fourcc("fmt "); b.u32le(16); b.u16le(1); b.u16le(1); b.u32le(8000); b.u32le(8000); b.u16le(1); b.u16le(8);
  b.fourcc("data"); b.u32le(4); b.u8(128); b.u8(255); b.u8(0); b.u8(64);
  auto r = openMem(b.v);
  REQUIRE(r);
  Channels c = readAll(r.get(), 1);
  REQUIRE_EQ(c[0].size(), size_t(4));
  CHECK_EQ(c[0][0], 0.0f);
  CHECK_EQ(c[0][2], -1.0f);
  CHECK_EQ(c[0][3], -0.5f);

  // data chunk before fmt, streaming writer left size 0
  sq::cx::Bytes d;
  d.fourcc("RIFF"); d.u32le(0); d.fourcc("WAVE");
  d.fourcc("odd "); d.u32le(3); d.u8(1); d.u8(2); d.u8(3); d.u8(0);  // padded
  d.fourcc("fmt "); d.u32le(16); d.u16le(1); d.u16le(1); d.u32le(8000); d.u32le(16000); d.u16le(2); d.u16le(16);
  d.fourcc("data"); d.u32le(0); d.u16le(0x4000); d.u16le(0xC000);
  r = openMem(d.v);
  REQUIRE(r);
  CHECK_EQ(fmtOf(r.get()).frame_count, 2);
  CHECK(metaValue(r.get(), "chunk:odd ") == std::vector<uint8_t>({1, 2, 3}));
  c = readAll(r.get(), 1);
  REQUIRE_EQ(c[0].size(), size_t(2));
  CHECK_EQ(c[0][0], 0.5f);
  CHECK_EQ(c[0][1], -0.5f);

  // AIFF-C little-endian 'sowt' 16-bit stereo, 2 frames, 44.1k (ext80 = 400E AC44 0000 0000 0000)
  sq::cx::Bytes a;
  a.fourcc("FORM"); a.u32be(0); a.fourcc("AIFC");
  a.fourcc("COMM"); a.u32be(24); a.u16be(2); a.u32be(2); a.u16be(16);
  const uint8_t r441[10] = {0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0};
  a.raw(r441, 10); a.fourcc("sowt"); a.u16be(0);
  a.fourcc("SSND"); a.u32be(8 + 8); a.u32be(0); a.u32be(0);
  a.u16le(0x4000); a.u16le(0xC000); a.u16le(0x2000); a.u16le(0);
  r = openMem(a.v);
  REQUIRE(r);
  auto f = fmtOf(r.get());
  CHECK_EQ(f.sample_rate, 44100.0);
  CHECK_EQ(std::string(f.container), std::string("aifc"));
  c = readAll(r.get(), 2);
  REQUIRE_EQ(c[0].size(), size_t(2));
  CHECK_EQ(c[0][0], 0.5f);
  CHECK_EQ(c[1][0], -0.5f);
  CHECK_EQ(c[0][1], 0.25f);

  // unsupported compressed WAV (ADPCM) fails cleanly
  sq::cx::Bytes u;
  u.fourcc("RIFF"); u.u32le(0); u.fourcc("WAVE");
  u.fourcc("fmt "); u.u32le(16); u.u16le(2); u.u16le(1); u.u32le(8000); u.u32le(4000); u.u16le(256); u.u16le(4);
  u.fourcc("data"); u.u32le(0);
  auto rr = env().reader();
  auto s = sq::makeMemoryStream(u.v.data(), u.v.size(), false);
  CHECK_EQ(rr->openStream(s.get()), SQ_E_UNSUPPORTED);
}

SQ_TEST("codec-wav: truncated / corrupted files never crash") {
  // rich source file: markers, tags, chunks
  auto build = [](const char* ext) {
    auto stream = sq::makeMemoryStream();
    auto wr = env().writer(ext);
    REQUIRE_OK(wr->openStream(stream.get(), &(const sq_audio_file_format&)format(48000, 2, SQ_SAMPLE_I24), nullptr));
    wr->setMetadata("chunk:iXML", "<x/>", 4);
    wr->setMetadata("tag:title", "fuzz", 4);
    auto m = sq::cx::makeMarker(1, 10, 100, SQ_MARKER_LOOP, "L");
    wr->addMarker(&m);
    auto m2 = sq::cx::makeMarker(2, 50, 0, 0, "P");
    wr->addMarker(&m2);
    REQUIRE_OK(writeAll(wr.get(), signal(2, 3000)));
    REQUIRE_OK(wr->finalize());
    return streamBytes(stream.get());
  };
  fuzz(env(), build("wav"), 400, 1);
  fuzz(env(), build("aif"), 400, 2);
  auto opts = sq::makeAttributes();
  opts->setBool("rf64", SQ_TRUE);
  fuzz(env(), writeMem("wav", format(48000, 2, SQ_SAMPLE_I16), signal(2, 500), opts.get()).bytes, 200, 3);
  CHECK(true);
}

// ---- verifier additions ---------------------------------------------------------------------------

SQ_TEST("codec-wav: CHAN bitmap that does not match the channel count is not used as the WAV mask") {
  // CoreAudio use-bitmap layout with only FL|FR set, but 6 channels are written.
  uint8_t chan[12] = {0, 1, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0};
  auto stream = sq::makeMemoryStream();
  auto wr = env().writer("wav");
  REQUIRE_OK(wr->openStream(stream.get(), &(const sq_audio_file_format&)format(48000, 6, SQ_SAMPLE_I24), nullptr));
  REQUIRE_OK(wr->setMetadata("chunk:CHAN", chan, sizeof chan));
  REQUIRE_OK(writeAll(wr.get(), signal(6, 100)));
  REQUIRE_OK(wr->finalize());
  auto b = streamBytes(stream.get());
  REQUIRE(b.size() > 100);
  REQUIRE(!std::memcmp(b.data() + 48, "fmt ", 4));
  const uint32_t mask = sq::cx::rd32le(b.data() + 56 + 20);
  uint32_t bits = 0;
  for (uint32_t m = mask; m; m &= m - 1) ++bits;
  CHECK(mask == 0 || bits == 6);
  CHECK_EQ(mask, 0x3Fu);
  // a matching bitmap is still honoured (L R C LFE Ls Rs as bitmap 0x3F -> 0x3F; use 0x60F: L R C LFE + side pair)
  uint8_t chan2[12] = {0, 1, 0, 0, 0, 0, 0x06, 0x0F, 0, 0, 0, 0};
  auto stream2 = sq::makeMemoryStream();
  auto wr2 = env().writer("wav");
  REQUIRE_OK(wr2->openStream(stream2.get(), &(const sq_audio_file_format&)format(48000, 6, SQ_SAMPLE_I24), nullptr));
  REQUIRE_OK(wr2->setMetadata("chunk:CHAN", chan2, sizeof chan2));
  REQUIRE_OK(writeAll(wr2.get(), signal(6, 100)));
  REQUIRE_OK(wr2->finalize());
  auto b2 = streamBytes(stream2.get());
  CHECK_EQ(sq::cx::rd32le(b2.data() + 56 + 20), 0x60Fu);
}

SQ_TEST("codec-wav: AIFF loop whose start marker uses the highest MARK id keeps its loop") {
  auto stream = sq::makeMemoryStream();
  auto wr = env().writer("aif");
  REQUIRE_OK(wr->openStream(stream.get(), &(const sq_audio_file_format&)format(48000, 1, SQ_SAMPLE_I16), nullptr));
  auto m = sq::cx::makeMarker(0x7FFF, 1000, 500, SQ_MARKER_LOOP, "Top");
  REQUIRE_OK(wr->addMarker(&m));
  auto m2 = sq::cx::makeMarker(3, 4000, 300, SQ_MARKER_LOOP, "Second");
  REQUIRE_OK(wr->addMarker(&m2));
  REQUIRE_OK(writeAll(wr.get(), signal(1, 8000)));
  REQUIRE_OK(wr->finalize());
  auto r = openMem(streamBytes(stream.get()));
  REQUIRE(r);
  REQUIRE_EQ(r->getMarkerCount(), 2u);
  for (uint32_t i = 0; i < 2; ++i) {
    sq_audio_file_marker g{};
    g.struct_size = sizeof g;
    REQUIRE_OK(r->getMarker(i, &g));
    CHECK(g.id >= 1 && g.id <= 0x7FFF);
    CHECK(g.flags & SQ_MARKER_LOOP);
    CHECK_EQ(g.length, i == 0 ? 500 : 300);
  }
}

SQ_TEST("codec-wav: empty file, reopen guards, getMetadataKey truncation, bad args") {
  // writer finalized without audio: valid, zero frames
  auto stream = sq::makeMemoryStream();
  auto wr = env().writer("wav");
  REQUIRE_OK(wr->openStream(stream.get(), &(const sq_audio_file_format&)format(44100, 2, SQ_SAMPLE_I16), nullptr));
  REQUIRE_OK(wr->setMetadata("chunk:abcd", "x", 1));
  REQUIRE_OK(wr->finalize());
  auto bytes = streamBytes(stream.get());
  auto r = openMem(bytes);
  REQUIRE(r);
  CHECK_EQ(fmtOf(r.get()).frame_count, 0);
  Channels c = readAll(r.get(), 2);
  CHECK(c[0].empty());
  // second open on the same reader is refused
  auto s2 = sq::makeMemoryStream(bytes.data(), bytes.size(), false);
  CHECK_EQ(r->openStream(s2.get()), SQ_E_EXISTS);
  // key copy-out: needed includes the NUL, short buffers are NUL terminated
  char small[4];
  uint32_t need = 0;
  CHECK_EQ(r->getMetadataKey(0, small, sizeof small, &need), SQ_E_BUFFER_TOO_SMALL);
  CHECK_EQ(need, 11u);
  CHECK_EQ(small[3], '\0');
  CHECK_EQ(r->getMetadataKey(5, small, sizeof small, &need), SQ_E_NOTFOUND);
  // wrong channel count in a read block
  float buf[8];
  float* p[1] = {buf};
  sq_audio_block blk{p, 1, 8, 0};
  uint32_t got = 99;
  CHECK_EQ(r->read(&blk, &got), SQ_E_INVALIDARG);
  CHECK_EQ(got, 0u);
  // unopened reader
  auto r2 = env().reader();
  sq_audio_file_format f{};
  f.struct_size = sizeof f;
  CHECK_EQ(r2->getFormat(&f), SQ_E_CLOSED);
  CHECK_EQ(r2->seek(0), SQ_E_CLOSED);
  // writer: unsupported formats are refused before anything is created
  auto w3 = env().writer("wav");
  auto bad = format(44100, 0, SQ_SAMPLE_I16);
  auto s3 = sq::makeMemoryStream();
  CHECK(SQ_FAILED(w3->openStream(s3.get(), &bad, nullptr)));
  auto bad2 = format(44100.5, 2, SQ_SAMPLE_I16);
  CHECK(SQ_FAILED(w3->openStream(s3.get(), &bad2, nullptr)));
  auto good = format(44100, 2, SQ_SAMPLE_I16);
  CHECK_OK(w3->openStream(s3.get(), &good, nullptr));
  CHECK_EQ(w3->openStream(s3.get(), &good, nullptr), SQ_E_EXISTS);
}

SQ_TEST("codec-wav: AIFF with fewer valid bits than the requested container stays readable") {
  // AIFF has no container width separate from the bit depth: 24-in-32 and 16-in-32 must be packed
  roundTrip("aif", SQ_SAMPLE_I24_IN32, 0, 2, 1.0 / 8388608 + 1e-7, "aif");
  roundTrip("aif", SQ_SAMPLE_I32, 16, 2, 1.0 / 32768 + 1e-7, "aif");
  roundTrip("aifc", SQ_SAMPLE_I32, 20, 1, 1.0 / 524288 + 1e-7, "aifc");
  // WAV keeps the wide container (WAVE_FORMAT_EXTENSIBLE valid bits)
  roundTrip("wav", SQ_SAMPLE_I24_IN32, 0, 2, 1.0 / 8388608 + 1e-7, "wav");
  roundTrip("wav", SQ_SAMPLE_I32, 16, 2, 1.0 / 32768 + 1e-7, "wav");
}
