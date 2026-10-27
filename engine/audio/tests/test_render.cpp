// engine/audio - exit test: offline render is bit-exact against an independently computed
// reference (32-bit and 64-bit mix bus), invariant to block size and plugin delay, and the
// render tasks (mixdown / stems / click) write the same samples through the codec interface.
#include <cmath>
#include <cstring>
#include <vector>

#include "fakes.hpp"
#include "sq/app/class_registry.hpp"
#include "sq/engine/audio/engine.hpp"
#include "sq/engine/audio/render.hpp"
#include "sqtest.hpp"

using namespace sq::engine::audio;

namespace {

constexpr double kSr = 48000.0;
constexpr int64_t kLenA = 100000, kLenB = 120000;

float noise(int64_t i, uint32_t c) {
  uint32_t x = static_cast<uint32_t>(i) * 2654435761u + c * 40503u + 12345u;
  x ^= x >> 13;
  x *= 0x5bd1e995u;
  x ^= x >> 15;
  return static_cast<float>(x & 0xFFFFu) / 65536.0f - 0.5f;
}

MediaPtr makeMedia(uint32_t channels, int64_t frames, uint32_t salt) {
  auto m = std::make_shared<MediaBuffer>();
  m->sampleRate = kSr;
  m->channels = channels;
  m->frames = frames;
  m->data.resize(channels);
  for (uint32_t c = 0; c < channels; ++c) {
    m->data[c].resize(static_cast<size_t>(frames));
    for (int64_t i = 0; i < frames; ++i) m->data[c][static_cast<size_t>(i)] = noise(i, c + salt);
  }
  return m;
}

struct Scene {
  MediaPtr a = makeMedia(1, kLenA, 0);
  MediaPtr b = makeMedia(2, kLenB, 7);
  // track A: mono clip @1000 len 90000 gain .5, vol .8 pan -.3 -> master
  // track B: stereo clip @0 offset 500 len 110000, vol .6 pan .2 -> bus (vol .9) -> master
  GraphDesc desc(uint32_t latentA = 0, uint32_t latentBus = 0) const {
    GraphDesc d;
    d.sampleRate = kSr;
    BusDesc bus;
    bus.channels = 2;
    bus.volume = 0.9f;
    if (latentBus) bus.inserts.push_back(std::make_shared<DelayFx>(latentBus));
    d.buses.push_back(bus);
    TrackDesc ta;
    ta.id = fakes::newGuid();
    ta.name = "A";
    ta.channels = 1;
    ClipParams pa;
    pa.start = 1000;
    pa.length = 90000;
    pa.gain = 0.5f;
    ta.sources.push_back(std::make_shared<ClipNode>(std::make_shared<MediaNode>(a), pa));
    ta.volume = 0.8f;
    ta.pan = -0.3f;
    if (latentA) ta.inserts.push_back(std::make_shared<DelayFx>(latentA));
    d.tracks.push_back(ta);
    TrackDesc tb;
    tb.id = fakes::newGuid();
    tb.name = "B";
    tb.channels = 2;
    ClipParams pb;
    pb.start = 0;
    pb.length = 110000;
    pb.sourceOffset = 500;
    tb.sources.push_back(std::make_shared<ClipNode>(std::make_shared<MediaNode>(b), pb));
    tb.volume = 0.6f;
    tb.pan = 0.2f;
    tb.output = 0;
    d.tracks.push_back(tb);
    return d;
  }
  // Reference: written independently with the same float operation order the engine documents.
  std::vector<std::vector<float>> reference(int64_t start, int64_t end, bool mix64) const {
    float al, ar, bl, br;
    panGainsMono(-0.3f, &al, &ar);
    panGainsStereo(0.2f, &bl, &br);
    const float gAL = 0.8f * al, gAR = 0.8f * ar, gBL = 0.6f * bl, gBR = 0.6f * br;
    const float busL = 0.9f * 1.0f, busR = 0.9f * 1.0f;
    std::vector<std::vector<float>> out(2);
    for (int64_t t = start; t < end; ++t) {
      float xa = 0.0f;
      if (t >= 1000 && t < 91000) xa = 0.0f + a->data[0][static_cast<size_t>(t - 1000)] * 0.5f;
      float xbl = 0.0f, xbr = 0.0f;
      if (t >= 0 && t < 110000 && t + 500 < kLenB) {
        xbl = 0.0f + b->data[0][static_cast<size_t>(t + 500)] * 1.0f;
        xbr = 0.0f + b->data[1][static_cast<size_t>(t + 500)] * 1.0f;
      }
      const float trackAL = xa * gAL, trackAR = xa * gAR;
      const float trackBL = xbl * gBL, trackBR = xbr * gBR;
      float l, r;
      if (mix64) {
        const float bL = static_cast<float>(0.0 + static_cast<double>(trackBL)) * busL;
        const float bR = static_cast<float>(0.0 + static_cast<double>(trackBR)) * busR;
        l = static_cast<float>((0.0 + static_cast<double>(trackAL)) + static_cast<double>(bL)) * 1.0f;
        r = static_cast<float>((0.0 + static_cast<double>(trackAR)) + static_cast<double>(bR)) * 1.0f;
      } else {
        const float bL = (0.0f + trackBL) * busL;
        const float bR = (0.0f + trackBR) * busR;
        l = ((0.0f + trackAL) + bL) * 1.0f;
        r = ((0.0f + trackAR) + bR) * 1.0f;
      }
      out[0].push_back(l);
      out[1].push_back(r);
    }
    return out;
  }
};

bool bitEqual(const std::vector<float>& a, const std::vector<float>& b, size_t* firstDiff = nullptr) {
  if (a.size() != b.size()) {
    if (firstDiff) *firstDiff = std::min(a.size(), b.size());
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0) {
      if (firstDiff) *firstDiff = i;
      return false;
    }
  }
  return true;
}

}  // namespace

SQ_TEST("render: offline mixdown is bit-exact vs reference (32-bit bus), any block size") {
  Scene s;
  const auto ref = s.reference(200, 100000, false);
  for (uint32_t block : {1024u, 64u, 333u}) {
    RenderJob job;
    job.start = 200;
    job.end = 100000;
    job.blockFrames = block;
    std::vector<std::vector<float>> out;
    REQUIRE_OK(renderToMemory(s.desc(), job, &out));
    size_t d = 0;
    const bool ok = bitEqual(out[0], ref[0], &d) && bitEqual(out[1], ref[1], &d);
    if (!ok) std::printf("block %u: first difference at %zu\n", block, d);
    CHECK(ok);
  }
}

SQ_TEST("render: 64-bit mix bus is bit-exact vs its reference") {
  Scene s;
  const auto ref = s.reference(0, 95000, true);
  RenderJob job;
  job.start = 0;
  job.end = 95000;
  job.mix64 = true;
  std::vector<std::vector<float>> out;
  REQUIRE_OK(renderToMemory(s.desc(), job, &out));
  CHECK(bitEqual(out[0], ref[0]));
  CHECK(bitEqual(out[1], ref[1]));
}

SQ_TEST("render: plugin delay does not move a sample (PDC + pre-roll discard)") {
  Scene s;
  const auto ref = s.reference(200, 100000, false);
  RenderJob job;
  job.start = 200;
  job.end = 100000;
  job.blockFrames = 512;
  std::vector<std::vector<float>> out;
  // pure-delay inserts on track A (777) and on the bus (313): total path latency differs
  REQUIRE_OK(renderToMemory(s.desc(777, 313), job, &out));
  CHECK(bitEqual(out[0], ref[0]));
  CHECK(bitEqual(out[1], ref[1]));
}

SQ_TEST("render: tasks write mixdown, stems and click through the codec interface") {
  sq::app::ClassRegistry reg;
  fakes::registerFakeCodec(&reg);
  Scene s;
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, nullptr));
  REQUIRE_OK(eng->setGraphDesc(s.desc()));
  // 120 bpm @48k: 1 ppq = 24000 frames. Range 0..4 ppq = 0..96000.
  sq_render_request rq{};
  rq.struct_size = sizeof rq;
  rq.kind = SQ_RENDER_MIXDOWN;
  rq.start = 0;
  rq.end = 4;
  rq.output = "mem://mix.tst";
  rq.file_extension = "tst";
  sq::Ptr<sq::IRenderTask> task;
  REQUIRE_OK(eng->startRender(&rq, nullptr, task.put()));
  REQUIRE_OK(task->wait(60000));
  REQUIRE_OK(task->getResult());
  CHECK_EQ(task->getOutputCount(), 1u);
  fakes::MemFile mf;
  REQUIRE(fakes::getMemFile("mem://mix.tst", &mf));
  CHECK(mf.finalized);
  const auto ref = s.reference(0, 96000, false);
  CHECK(bitEqual(mf.data[0], ref[0]));
  CHECK(bitEqual(mf.data[1], ref[1]));

  // stems: one file per track, timeline aligned, post-fader
  rq.kind = SQ_RENDER_STEMS;
  rq.output = "mem://stems";
  REQUIRE_OK(eng->startRender(&rq, nullptr, task.put()));
  REQUIRE_OK(task->wait(60000));
  REQUIRE_OK(task->getResult());
  CHECK_EQ(task->getOutputCount(), 2u);
  fakes::MemFile sa;
  REQUIRE(fakes::getMemFile(sq::joinPath("mem://stems", "A.tst"), &sa));
  CHECK_EQ(sa.frames(), int64_t(96000));
  float al, ar;
  panGainsMono(-0.3f, &al, &ar);
  CHECK_EQ(sa.data[0][5000], (0.0f + s.a->data[0][4000] * 0.5f) * (0.8f * al));

  // click: engine-private kind, metronome only
  rq.kind = kRenderClick;
  rq.output = "mem://click.tst";
  REQUIRE_OK(eng->startRender(&rq, nullptr, task.put()));
  REQUIRE_OK(task->wait(60000));
  REQUIRE_OK(task->getResult());
  fakes::MemFile ck;
  REQUIRE(fakes::getMemFile("mem://click.tst", &ck));
  CHECK_EQ(ck.frames(), int64_t(96000));
  CHECK(std::fabs(ck.data[0][24010]) > 0.0f);
  CHECK_EQ(ck.data[0][23990], 0.0f);

  // normalize: peak of the written file is 1.0
  rq.kind = SQ_RENDER_MIXDOWN;
  rq.flags = SQ_RENDER_NORMALIZE;
  rq.output = "mem://norm.tst";
  REQUIRE_OK(eng->startRender(&rq, nullptr, task.put()));
  REQUIRE_OK(task->wait(60000));
  REQUIRE_OK(task->getResult());
  fakes::MemFile nm;
  REQUIRE(fakes::getMemFile("mem://norm.tst", &nm));
  float peak = 0;
  for (auto& ch : nm.data)
    for (float v : ch) peak = std::max(peak, std::fabs(v));
  CHECK(std::fabs(peak - 1.0f) < 1e-6f);

  // cancel
  rq.flags = 0;
  rq.end = 4000;
  rq.output = "mem://cancel.tst";
  REQUIRE_OK(eng->startRender(&rq, nullptr, task.put()));
  task->cancel();
  REQUIRE_OK(task->wait(60000));
  CHECK_EQ(task->getResult(), SQ_E_CLOSED);
  fakes::MemFile cf;
  CHECK(!fakes::getMemFile("mem://cancel.tst", &cf));
}
