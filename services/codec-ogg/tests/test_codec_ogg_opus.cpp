// codec-ogg Opus seam: decodes the ffmpeg/libopus fixture through a user-installed libopusfile
// (Services.codec-ogg.OpusfileLibrary / SQ_OPUSFILE_LIBRARY / default names). Without the library
// the case checks the clean SQ_E_UNSUPPORTED path and then SKIPs (exit 77), so CTest shows the
// decode as not run instead of passed.
#include <cstdlib>

#include "sqcodec/test_util.hpp"

using namespace sqct;

namespace {
CodecEnv& env() {
  static CodecEnv e("codec-ogg", sq::codec::kOggCodecClassId);
  return e;
}
int zeroCrossings(const std::vector<float>& v, size_t from, size_t to) {
  int n = 0;
  for (size_t i = from + 1; i < to && i < v.size(); ++i) n += (v[i - 1] < 0) != (v[i] < 0);
  return n;
}
}  // namespace

SQ_TEST("codec-ogg: Opus seam decodes with libopusfile") {
  REQUIRE(env().ok);
  auto bytes = loadFile(sq::joinPath(SQ_CODEC_TEST_DATA, "tone.opus"));
  REQUIRE(bytes.size() > 100);
  auto r = env().reader();
  REQUIRE(r);
  auto s = sq::makeMemoryStream(bytes.data(), bytes.size(), false);
  sq_result res = r->openStream(s.get());
  if (!env().svc->supports("opus", nullptr, SQ_CODEC_READ)) {
    CHECK_EQ(res, SQ_E_UNSUPPORTED);
    SQ_SKIP("libopusfile not found (set SQ_OPUSFILE_LIBRARY to run the Opus decode case)");
  }
  REQUIRE(SQ_SUCCEEDED(res));
  sq_audio_file_format f{};
  f.struct_size = sizeof f;
  CHECK_OK(r->getFormat(&f));
  CHECK_EQ(f.sample_rate, 48000.0);
  CHECK_EQ(f.channels, 1u);
  CHECK_EQ(f.frame_count, 48000);
  CHECK_EQ(metaText(r.get(), "tag:title"), std::string("Squizzy Opus"));
  Channels full = readAll(r.get(), 1);
  REQUIRE_EQ(full[0].size(), size_t(48000));
  CHECK(std::abs(zeroCrossings(full[0], 2400, 45600) - 1728) <= 8);
  CHECK(seekMatches(r.get(), full, 20, 1e-4));
}
