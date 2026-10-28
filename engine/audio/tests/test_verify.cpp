// engine/audio - verifier regression tests: defects found in review, each case fails on the code
// before its fix.
//   - loop wrap with a transport placed after the loop end (must play on, not jump back)
//   - loop + plugin delay: the read-ahead wraps with the loop (output stays timeline-exact)
//   - fast lane honours mute / solo and the master volume (same level as the main path)
//   - malformed render / transport input (NaN, negative tail) is rejected
//   - transport commands report a full command queue
//   - audio clips on instrument tracks play (bounce-in-place result is audible)
//   - render observers are released when the task finished
//   - record stop on a stalled device returns the takes and frees the session later
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "fakes.hpp"
#include "sq/app/class_registry.hpp"
#include "sq/app/settings.hpp"
#include "sq/engine/audio/devices.hpp"
#include "sq/engine/audio/doc_builder.hpp"
#include "sq/engine/audio/engine.hpp"
#include "sq/engine/audio/render.hpp"
#include "sqtest.hpp"

using namespace sq::engine::audio;

namespace {

constexpr double kSr = 48000.0;

// Stereo media whose every sample is exact and unique: L = (i+1)/2^20, R = -L.
MediaPtr rampMedia(int64_t frames) {
  auto m = std::make_shared<MediaBuffer>();
  m->sampleRate = kSr;
  m->channels = 2;
  m->frames = frames;
  m->data.assign(2, std::vector<float>(static_cast<size_t>(frames)));
  for (int64_t i = 0; i < frames; ++i) {
    m->data[0][static_cast<size_t>(i)] = static_cast<float>(i + 1) / 1048576.0f;
    m->data[1][static_cast<size_t>(i)] = -static_cast<float>(i + 1) / 1048576.0f;
  }
  return m;
}

struct Capture {
  std::vector<float> l, r;
};

sq::Ptr<NullDevice> offlineDevice(uint32_t block, Capture* cap) {
  NullDeviceOptions o;
  o.offline = true;
  o.sampleRate = kSr;
  o.blockFrames = block;
  o.inputChannels = 1;
  o.outputChannels = 2;
  if (cap) {
    cap->l.reserve(1 << 20);
    cap->r.reserve(1 << 20);
    o.outputSink = [cap](int64_t, const float* const* ch, uint32_t nch, uint32_t n) {
      for (uint32_t i = 0; i < n; ++i) {
        cap->l.push_back(nch > 0 ? ch[0][i] : 0.0f);
        cap->r.push_back(nch > 1 ? ch[1][i] : 0.0f);
      }
    };
  }
  return sq::Ptr<NullDevice>::adopt(new NullDevice(o));
}

GraphDesc rampScene(const MediaPtr& m, uint32_t latency) {
  GraphDesc d;
  d.sampleRate = kSr;
  TrackDesc t;
  t.id = fakes::newGuid();
  t.name = "ramp";
  t.channels = 2;
  t.sources.push_back(std::make_shared<MediaNode>(m));
  if (latency) t.inserts.push_back(std::make_shared<DelayFx>(latency));
  d.tracks.push_back(t);
  return d;
}

class CountingObserver final : public sq::Object<CountingObserver, sq::IRenderObserver> {
 public:
  std::atomic<int> finished{0};
  void SQ_CALL onRenderProgress(sq::IRenderTask*, double) override {}
  void SQ_CALL onRenderFinished(sq::IRenderTask*, sq_result) override { finished.fetch_add(1); }
};

uint32_t refCount(sq::IUnknown* u) {
  u->addRef();
  return u->release();
}

}  // namespace

SQ_TEST("verify: transport placed after the loop end plays on (no jump back to the loop start)") {
  sq::app::ClassRegistry reg;
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, nullptr));
  REQUIRE_OK(eng->setGraphDesc(rampScene(rampMedia(1000), 0)));
  sq::Ptr<NullDevice> dev = offlineDevice(256, nullptr);
  REQUIRE_OK(eng->openDeviceObject(dev.get(), nullptr));
  REQUIRE_OK(eng->setLoop(1.0, 2.0, SQ_TRUE));  // frames 24000..48000 @120 bpm
  REQUIRE_OK(eng->locate(4.0));                 // 96000, after the loop
  REQUIRE_OK(eng->play());
  dev->pump(4);
  sq_transport_state ts{};
  ts.struct_size = sizeof ts;
  REQUIRE_OK(eng->getTransportState(&ts));
  CHECK((ts.flags & SQ_TRANSPORT_LOOPING) != 0);
  CHECK_EQ(ts.frames, int64_t(96000 + 1024));
  // inside the loop it still wraps
  REQUIRE_OK(eng->locate(1.9));  // 45600
  dev->pump(12);                 // 3072 frames: 2400 to the end, 672 after the wrap
  REQUIRE_OK(eng->getTransportState(&ts));
  CHECK_EQ(ts.frames, int64_t(24000 + 672));
  eng->closeDevice();
}

SQ_TEST("verify: looping with plugin delay keeps the output on the timeline across wraps") {
  sq::app::ClassRegistry reg;
  constexpr uint32_t kLat = 777;
  MediaPtr m = rampMedia(200000);
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, nullptr));
  REQUIRE_OK(eng->setGraphDesc(rampScene(m, kLat)));
  Capture cap;
  sq::Ptr<NullDevice> dev = offlineDevice(256, &cap);
  REQUIRE_OK(eng->openDeviceObject(dev.get(), nullptr));
  sq_engine_latency lat{};
  lat.struct_size = sizeof lat;
  REQUIRE_OK(eng->getLatency(&lat));
  REQUIRE_EQ(lat.plugin_delay_frames, kLat);
  const int64_t loopStart = 24000, loopEnd = 30000, startPos = 12000;  // short loop: several wraps
  REQUIRE_OK(eng->setLoop(1.0, 1.25, SQ_TRUE));
  REQUIRE_OK(eng->locate(0.5));
  REQUIRE_OK(eng->play());
  dev->pump(200);  // 51200 frames: 18000 to the loop end + > 5 wraps
  REQUIRE_EQ(cap.l.size(), size_t(51200));
  int64_t t = startPos;
  int64_t bad = 0, firstBad = -1, wraps = 0;
  for (size_t k = 0; k < cap.l.size(); ++k) {
    if (k >= kLat) {  // the first kLat frames come out of the (silent) delay line
      const float el = m->data[0][static_cast<size_t>(t)], er = m->data[1][static_cast<size_t>(t)];
      if (cap.l[k] != el || cap.r[k] != er) {
        if (firstBad < 0) firstBad = static_cast<int64_t>(k);
        ++bad;
      }
    }
    if (++t >= loopEnd) {
      t = loopStart;
      ++wraps;
    }
  }
  std::printf("verify: loop+PDC wraps %lld, misaligned frames %lld (first at %lld)\n", static_cast<long long>(wraps),
              static_cast<long long>(bad), static_cast<long long>(firstBad));
  CHECK(wraps >= 5);
  CHECK_EQ(bad, int64_t(0));
  eng->closeDevice();
}

namespace {
GraphDesc monitorScene(bool muteA, bool soloB, float masterVolume) {
  GraphDesc d;
  d.sampleRate = kSr;
  d.masterVolume = masterVolume;
  TrackDesc a;
  a.id = fakes::newGuid();
  a.channels = 1;
  a.monitor = true;
  a.inputChannels = {0};
  a.volume = 0.8f;
  a.pan = 0.25f;
  a.mute = muteA;
  d.tracks.push_back(a);
  TrackDesc b;
  b.id = fakes::newGuid();
  b.channels = 1;
  b.solo = soloB;
  d.tracks.push_back(b);
  return d;
}

// One block of 64 frames of device input through a fresh graph; returns the master output.
std::vector<std::vector<float>> monitorBlock(const GraphDesc& d, bool lowLatency) {
  GraphConfig cfg;
  cfg.maxBlock = 64;
  cfg.outputChannels = 2;
  std::unique_ptr<Graph> g = Graph::compile(d, cfg);
  std::vector<std::vector<float>> out(2, std::vector<float>(64, 0.0f));
  if (!g) return {};
  std::vector<float> in(64);
  for (uint32_t i = 0; i < 64; ++i) in[i] = 0.01f * static_cast<float>(i + 1);
  const float* ip[1] = {in.data()};
  float* op[2] = {out[0].data(), out[1].data()};
  ProcessContext ctx;
  ctx.sampleRate = kSr;
  ctx.frames = 64;
  ctx.input = ip;
  ctx.inputChannels = 1;
  g->process(ctx, op, 2, lowLatency, false);
  return out;
}

bool allZero(const std::vector<std::vector<float>>& x) {
  for (auto& c : x)
    for (float v : c)
      if (v != 0.0f) return false;
  return !x.empty();
}
}  // namespace

SQ_TEST("verify: fast lane honours mute, solo and the master volume") {
  // reference: main-path monitoring (no latency here, so both paths must match sample for sample)
  const auto mainOut = monitorBlock(monitorScene(false, false, 0.5f), false);
  const auto fastOut = monitorBlock(monitorScene(false, false, 0.5f), true);
  REQUIRE_EQ(mainOut.size(), size_t(2));
  REQUIRE_EQ(fastOut.size(), size_t(2));
  CHECK(!allZero(mainOut));
  for (uint32_t c = 0; c < 2; ++c)
    for (uint32_t i = 0; i < 64; ++i) CHECK_EQ(fastOut[c][i], mainOut[c][i]);
  // a muted monitored track is silent on both paths
  CHECK(allZero(monitorBlock(monitorScene(true, false, 1.0f), false)));
  CHECK(allZero(monitorBlock(monitorScene(true, false, 1.0f), true)));
  // another track's solo silences it on both paths
  CHECK(allZero(monitorBlock(monitorScene(false, true, 1.0f), false)));
  CHECK(allZero(monitorBlock(monitorScene(false, true, 1.0f), true)));
}

SQ_TEST("verify: malformed render and transport input is rejected") {
  sq::app::ClassRegistry reg;
  fakes::registerFakeCodec(&reg);
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, nullptr));
  REQUIRE_OK(eng->setGraphDesc(rampScene(rampMedia(1000), 0)));
  const double nan = std::numeric_limits<double>::quiet_NaN();
  sq_render_request rq{};
  rq.struct_size = sizeof rq;
  rq.kind = SQ_RENDER_MIXDOWN;
  rq.start = 0.0;
  rq.end = 0.1;
  rq.output = "mem://verify_mix.tst";
  rq.file_extension = "tst";
  rq.flags = SQ_RENDER_TAIL;
  rq.tail_seconds = -1.0;  // used to wrap to ~4.3e9 tail frames
  sq::Ptr<sq::IRenderTask> task;
  CHECK_EQ(eng->startRender(&rq, nullptr, task.put()), SQ_E_INVALIDARG);
  rq.tail_seconds = nan;
  CHECK_EQ(eng->startRender(&rq, nullptr, task.put()), SQ_E_INVALIDARG);
  rq.flags = 0;
  rq.start = nan;
  CHECK_EQ(eng->startRender(&rq, nullptr, task.put()), SQ_E_INVALIDARG);
  rq.start = 0.0;
  rq.end = std::numeric_limits<double>::infinity();
  CHECK_EQ(eng->startRender(&rq, nullptr, task.put()), SQ_E_INVALIDARG);
  rq.end = 1e-9;  // less than one frame
  CHECK_EQ(eng->startRender(&rq, nullptr, task.put()), SQ_E_INVALIDARG);
  rq.end = 0.1;
  rq.track_count = 3;  // count without ids
  CHECK_EQ(eng->startRender(&rq, nullptr, task.put()), SQ_E_INVALIDARG);
  rq.track_count = 0;
  CHECK(!task);
  CHECK_EQ(eng->locate(nan), SQ_E_INVALIDARG);
  CHECK_EQ(eng->setLoop(nan, 2.0, SQ_TRUE), SQ_E_INVALIDARG);
  CHECK_EQ(eng->setLoop(1.0, 1.0 + 1e-9, SQ_TRUE), SQ_E_INVALIDARG);
  // a valid request still works
  REQUIRE_OK(eng->startRender(&rq, nullptr, task.put()));
  REQUIRE_OK(task->wait(60000));
  CHECK_OK(task->getResult());
}

SQ_TEST("verify: transport commands report a full command queue") {
  sq::app::ClassRegistry reg;
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, nullptr));
  REQUIRE_OK(eng->setGraphDesc(rampScene(rampMedia(1000), 0)));
  sq::Ptr<NullDevice> dev = offlineDevice(64, nullptr);
  REQUIRE_OK(eng->openDeviceObject(dev.get(), nullptr));  // running, but nobody pumps it
  int busy = 0;
  for (int i = 0; i < 1500; ++i)
    if (eng->locate(static_cast<double>(i % 7)) == SQ_E_BUSY) ++busy;
  CHECK(busy > 0);
  CHECK_EQ(eng->play(), SQ_E_BUSY);
  dev->pump(1);  // the audio thread drains the queue
  CHECK_OK(eng->play());
  eng->closeDevice();
}

SQ_TEST("verify: audio clips on an instrument track are played (bounce-in-place result)") {
  sq::app::ClassRegistry reg;
  fakes::registerFakeCodec(&reg);
  fakes::putMemFile("mem://verify_inst.tst", kSr, std::vector<std::vector<float>>(1, std::vector<float>(48000, 0.5f)));
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  auto t = sq::Ptr<fakes::MTrack>::adopt(new fakes::MTrack());
  t->name = "Keys";
  t->info.kind = SQ_TRACK_INSTRUMENT;
  t->info.channels = 1;
  auto c = sq::Ptr<fakes::MClip>::adopt(new fakes::MClip());
  c->info.kind = SQ_CLIP_AUDIO;
  c->info.start = 0;
  c->info.length = 1;
  c->path = "mem://verify_inst.tst";
  t->clips.push_back(c);
  doc->tracks.push_back(t);
  MediaCache cache;
  GraphDesc d;
  REQUIRE_OK(buildGraphDesc(doc.get(), &reg, cache, 0.0, &d));
  REQUIRE_EQ(d.tracks.size(), size_t(1));
  CHECK_EQ(d.tracks[0].sources.size(), size_t(1));
  RenderJob job;
  job.start = 0;
  job.end = 1000;
  job.outputChannels = 1;
  std::vector<std::vector<float>> out;
  REQUIRE_OK(renderToMemory(d, job, &out));
  REQUIRE_EQ(out.size(), size_t(1));
  CHECK_EQ(out[0][500], 0.5f);
}

SQ_TEST("verify: render observer is released once the task finished") {
  sq::app::ClassRegistry reg;
  fakes::registerFakeCodec(&reg);
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, nullptr));
  REQUIRE_OK(eng->setGraphDesc(rampScene(rampMedia(48000), 0)));
  sq::Ptr<CountingObserver> obs = sq::Ptr<CountingObserver>::adopt(new CountingObserver());
  const uint32_t before = refCount(obs.get());
  sq_render_request rq{};
  rq.struct_size = sizeof rq;
  rq.kind = SQ_RENDER_MIXDOWN;
  rq.start = 0.0;
  rq.end = 1.0;
  rq.output = "mem://verify_obs.tst";
  rq.file_extension = "tst";
  sq::Ptr<sq::IRenderTask> task;
  REQUIRE_OK(eng->startRender(&rq, obs.get(), task.put()));
  REQUIRE_OK(task->wait(60000));
  CHECK_OK(task->getResult());
  CHECK_EQ(obs->finished.load(), 1);
  CHECK_EQ(refCount(obs.get()), before);  // task still alive, observer no longer held
}

SQ_TEST("verify: record stop on a stalled device returns the takes, session freed after a callback") {
  sq::app::ClassRegistry reg;
  fakes::registerFakeCodec(&reg);
  sq::app::Settings settings;
  settings.setString("Media.Folder", "mem://verify_rec");
  settings.setString("Media.RecordFormat", "tst");
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, &settings));
  GraphDesc d;
  d.sampleRate = kSr;
  TrackDesc t;
  t.id = fakes::newGuid();
  t.name = "Mic";
  t.channels = 1;
  t.inputChannels = {0};
  t.armed = true;
  d.tracks.push_back(t);
  REQUIRE_OK(eng->setGraphDesc(d));
  sq::Ptr<NullDevice> dev = offlineDevice(128, nullptr);
  REQUIRE_OK(eng->openDeviceObject(dev.get(), nullptr));
  REQUIRE_OK(eng->play());
  REQUIRE_OK(eng->record(SQ_TRUE));
  dev->pump(10);
  REQUIRE_OK(eng->record(SQ_FALSE));  // nobody pumps: the RT handshake times out (1 s)
  const auto takes = eng->lastTakes();
  REQUIRE_EQ(takes.size(), size_t(1));
  CHECK_EQ(takes[0].frames, int64_t(1280));
  dev->pump(3);  // callbacks pass: the lingering session may now be freed
  eng->idle();
  eng->closeDevice();
}

namespace {
class LoopSpy final : public sq::Object<LoopSpy, sq::IObserver> {
 public:
  int count = 0;
  double start = -1, end = -1;
  bool enabled = false;
  void SQ_CALL onNotify(sq::IUnknown*, const char* topic, sq::IAttributes* args) override {
    if (!topic || std::strcmp(topic, "transport.loop") != 0 || !args) return;
    ++count;
    start = args->getDouble("start", -1);
    end = args->getDouble("end", -1);
    enabled = args->getBool("enabled", SQ_FALSE) != 0;
  }
};
}  // namespace

SQ_TEST("verify: setLoop announces the requested loop at once (the song saves it)") {
  sq::app::ClassRegistry reg;
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, nullptr));
  REQUIRE_OK(eng->setGraphDesc(rampScene(rampMedia(1000), 0)));
  auto spy = sq::Ptr<LoopSpy>::adopt(new LoopSpy());
  REQUIRE_OK(eng->addObserver(spy.get()));
  REQUIRE_OK(eng->setLoop(4.0, 12.0, SQ_TRUE));
  CHECK_EQ(spy->count, 1);
  CHECK_EQ(spy->start, 4.0);
  CHECK_EQ(spy->end, 12.0);
  CHECK(spy->enabled);
  REQUIRE_OK(eng->setLoop(4.0, 12.0, SQ_FALSE));
  CHECK_EQ(spy->count, 2);
  CHECK(!spy->enabled);
  CHECK_EQ(eng->setLoop(2.0, 1.0, SQ_TRUE), SQ_E_INVALIDARG);  // refused: not announced
  CHECK_EQ(spy->count, 2);
  REQUIRE_OK(eng->removeObserver(spy.get()));
}
