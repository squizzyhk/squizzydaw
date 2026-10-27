// engine/audio - exit test: plugin delay compensation with a mock latent processor across tracks,
// parallel branches and buses, plus the engine's latency bookkeeping and live (device) alignment.
#include <cmath>
#include <vector>

#include "fakes.hpp"
#include "sq/app/class_registry.hpp"
#include "sq/engine/audio/devices.hpp"
#include "sq/engine/audio/engine.hpp"
#include "sq/engine/audio/render.hpp"
#include "sqtest.hpp"

using namespace sq::engine::audio;

namespace {

// Mock latent processor written for the test: a lookahead-style effect whose output is its input
// `d` frames late and which reports exactly that latency (own ring buffer, not DelayFx).
class MockLatentFx final : public FxProcessor {
 public:
  explicit MockLatentFx(uint32_t d) : d_(d) {}
  ProcessorPtr clone() const override { return std::make_unique<MockLatentFx>(d_); }
  bool prepare(double, uint32_t, uint32_t channels) override {
    ring_.assign(channels, std::vector<float>(d_ + 1, 0.0f));
    idx_ = 0;
    return true;
  }
  void process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept override {
    const size_t len = d_ + 1;
    size_t idx = idx_;
    for (uint32_t i = 0; i < ctx.frames; ++i) {
      for (uint32_t c = 0; c < channels; ++c) {
        ring_[c][idx] = io[c][i];
        io[c][i] = ring_[c][(idx + 1) % len];  // oldest sample = d frames ago
      }
      idx = (idx + 1) % len;
    }
    idx_ = idx;
  }
  uint32_t latency() const noexcept override { return d_; }

 private:
  uint32_t d_;
  std::vector<std::vector<float>> ring_;
  size_t idx_ = 0;
};

MediaPtr impulseAt(int64_t frame, int64_t len) {
  auto m = std::make_shared<MediaBuffer>();
  m->channels = 1;
  m->frames = len;
  m->data.assign(1, std::vector<float>(static_cast<size_t>(len), 0.0f));
  m->data[0][static_cast<size_t>(frame)] = 1.0f;
  return m;
}

TrackDesc impulseTrack(int64_t at, std::vector<ProcessorProto> inserts, int32_t output = -1) {
  TrackDesc t;
  t.id = fakes::newGuid();
  t.channels = 1;
  t.pan = -1.0f;  // hard left: gain exactly 1
  ClipParams p;
  p.start = 0;
  p.length = 20000;
  t.sources.push_back(std::make_shared<ClipNode>(std::make_shared<MediaNode>(impulseAt(at, 20000)), p));
  t.inserts = std::move(inserts);
  t.output = output;
  return t;
}

// returns indices of non-zero samples of the left channel
std::vector<std::pair<size_t, float>> peaks(const std::vector<float>& x) {
  std::vector<std::pair<size_t, float>> r;
  for (size_t i = 0; i < x.size(); ++i)
    if (std::fabs(x[i]) > 1e-9f) r.emplace_back(i, x[i]);
  return r;
}

}  // namespace

SQ_TEST("pdc: mock latent processor is self-consistent") {
  MockLatentFx fx(5);
  fx.prepare(48000, 16, 1);
  float x[16] = {1.0f};
  float* p[1] = {x};
  ProcessContext ctx;
  ctx.frames = 16;
  fx.process(ctx, p, 1);
  for (int i = 0; i < 16; ++i) CHECK_EQ(x[i], i == 5 ? 1.0f : 0.0f);
}

SQ_TEST("pdc: tracks with different latency arrive aligned and on the timeline") {
  GraphDesc d;
  d.tracks.push_back(impulseTrack(1000, {std::make_shared<MockLatentFx>(300)}));
  d.tracks.push_back(impulseTrack(1000, {}));
  RenderJob job;
  job.start = 0;
  job.end = 4000;
  job.blockFrames = 128;
  std::vector<std::vector<float>> out;
  REQUIRE_OK(renderToMemory(d, job, &out));
  auto pk = peaks(out[0]);
  REQUIRE_EQ(pk.size(), size_t(1));
  CHECK_EQ(pk[0].first, size_t(1000));  // timeline position, not 1300
  CHECK_EQ(pk[0].second, 2.0f);         // both paths summed in the same sample
  auto g = Graph::compile(d, GraphConfig{});
  REQUIRE(g);
  CHECK_EQ(g->latency(), 300u);
  CHECK_EQ(g->trackCompensation(0), 0u);
  CHECK_EQ(g->trackCompensation(1), 300u);
}

SQ_TEST("pdc: parallel branches are compensated inside the processor") {
  auto par = std::make_shared<ParallelProcessor>();
  par->addBranch({std::make_shared<MockLatentFx>(128)});
  par->addBranch({});  // dry
  par->addBranch({std::make_shared<MockLatentFx>(40), std::make_shared<MockLatentFx>(10)});
  GraphDesc d;
  d.tracks.push_back(impulseTrack(2000, {par}));
  auto g = Graph::compile(d, GraphConfig{});
  REQUIRE(g);
  CHECK_EQ(g->latency(), 128u);
  RenderJob job;
  job.start = 0;
  job.end = 5000;
  job.blockFrames = 100;
  std::vector<std::vector<float>> out;
  REQUIRE_OK(renderToMemory(d, job, &out));
  auto pk = peaks(out[0]);
  REQUIRE_EQ(pk.size(), size_t(1));
  CHECK_EQ(pk[0].first, size_t(2000));
  CHECK_EQ(pk[0].second, 3.0f);
}

SQ_TEST("pdc: bus chains and nested buses") {
  GraphDesc d;
  d.buses.resize(2);
  d.buses[0].inserts.push_back(std::make_shared<MockLatentFx>(200));  // bus0 -> bus1
  d.buses[0].output = 1;
  d.buses[1].output = -1;
  d.buses[0].pan = 0.0f;
  d.tracks.push_back(impulseTrack(1500, {std::make_shared<MockLatentFx>(50)}, 0));  // 50 + 200 via bus0
  d.tracks.push_back(impulseTrack(1500, {}, 1));                                      // straight into bus1
  d.tracks.push_back(impulseTrack(1500, {std::make_shared<MockLatentFx>(10)}, -1));  // to master
  auto g = Graph::compile(d, GraphConfig{});
  REQUIRE(g);
  CHECK_EQ(g->busArrival(0), 250u);
  CHECK_EQ(g->busArrival(1), 250u);
  CHECK_EQ(g->latency(), 250u);
  CHECK_EQ(g->trackCompensation(1), 250u);
  CHECK_EQ(g->trackCompensation(2), 240u);
  RenderJob job;
  job.start = 0;
  job.end = 3000;
  job.blockFrames = 64;
  std::vector<std::vector<float>> out;
  REQUIRE_OK(renderToMemory(d, job, &out));
  auto pk = peaks(out[0]);
  REQUIRE_EQ(pk.size(), size_t(1));
  CHECK_EQ(pk[0].first, size_t(1500));
  CHECK_EQ(pk[0].second, 3.0f);
}

SQ_TEST("pdc: engine latency bookkeeping and live playback alignment through a device") {
  sq::app::ClassRegistry reg;
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, nullptr));
  GraphDesc d;
  d.tracks.push_back(impulseTrack(1000, {std::make_shared<MockLatentFx>(300)}));
  d.tracks.push_back(impulseTrack(1000, {}));
  const sq_guid idA = d.tracks[0].id, idB = d.tracks[1].id;
  REQUIRE_OK(eng->setGraphDesc(d));

  NullDeviceOptions o;
  o.offline = true;
  o.blockFrames = 64;
  o.inputLatency = 32;
  o.outputLatency = 16;
  std::vector<float> captured;
  o.outputSink = [&captured](int64_t, const float* const* ch, uint32_t, uint32_t n) {
    sq::engine::audio::rt::Unscope u;  // test capture may allocate
    captured.insert(captured.end(), ch[0], ch[0] + n);
  };
  sq::Ptr<NullDevice> dev = sq::Ptr<NullDevice>::adopt(new NullDevice(o));
  REQUIRE_OK(eng->openDeviceObject(dev.get(), nullptr));

  sq_engine_latency lat{};
  lat.struct_size = sizeof lat;
  REQUIRE_OK(eng->getLatency(&lat));
  CHECK_EQ(lat.device_input_frames, 64u + 32u);
  CHECK_EQ(lat.device_output_frames, 64u + 16u);
  CHECK_EQ(lat.block_frames, 64u);
  CHECK_EQ(lat.plugin_delay_frames, 300u);
  CHECK_EQ(lat.main_roundtrip_frames, 96u + 80u + 300u);
  CHECK_EQ(lat.fast_lane_roundtrip_frames, 96u + 80u);
  sq_latency_info nl{};
  REQUIRE_OK(eng->getNodeLatency(&idA, &nl));
  CHECK_EQ(nl.process_frames, 300u);
  REQUIRE_OK(eng->getNodeLatency(&idB, &nl));
  CHECK_EQ(nl.process_frames, 0u);

  // live playback from 0: the device output frame k carries timeline frame k
  REQUIRE_OK(eng->play());
  dev->pump(40);
  auto pk = peaks(captured);
  REQUIRE_EQ(pk.size(), size_t(1));
  CHECK_EQ(pk[0].first, size_t(1000));
  CHECK_EQ(pk[0].second, 2.0f);
  eng->closeDevice();
}
