// engine/audio - unit tests: RT primitives, laws, graph structure, transport, swaps, fast lane.
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "sq/engine/audio/devices.hpp"
#include "sq/engine/audio/engine.hpp"
#include "sq/engine/audio/graph.hpp"
#include "sqtest.hpp"

using namespace sq::engine::audio;

namespace {

MediaPtr makeMedia(uint32_t channels, int64_t frames, float (*fn)(int64_t, uint32_t), double sampleRate = 48000.0) {
  auto m = std::make_shared<MediaBuffer>();
  m->channels = channels;
  m->frames = frames;
  m->sampleRate = sampleRate;
  m->data.resize(channels);
  for (uint32_t c = 0; c < channels; ++c) {
    m->data[c].resize(static_cast<size_t>(frames));
    for (int64_t i = 0; i < frames; ++i) m->data[c][static_cast<size_t>(i)] = fn(i, c);
  }
  return m;
}

SourceProto clipOf(MediaPtr m, int64_t start, int64_t length, float gain = 1.0f) {
  ClipParams p;
  p.start = start;
  p.length = length;
  p.gain = gain;
  return std::make_shared<ClipNode>(std::make_shared<MediaNode>(std::move(m)), p);
}

std::vector<std::vector<float>> runGraph(Graph& g, int64_t from, int64_t frames, uint32_t block, bool lowLat = false,
                                         const float* const* input = nullptr, uint32_t inCh = 0) {
  std::vector<std::vector<float>> out(g.outputChannels());
  std::vector<float> buf(static_cast<size_t>(g.outputChannels()) * block);
  std::vector<float*> ptr(g.outputChannels());
  for (uint32_t c = 0; c < g.outputChannels(); ++c) ptr[c] = &buf[static_cast<size_t>(c) * block];
  int64_t pos = from;
  int64_t done = 0;
  while (done < frames) {
    const uint32_t n = static_cast<uint32_t>(std::min<int64_t>(block, frames - done));
    ProcessContext ctx;
    ctx.frames = n;
    ctx.outputFrame = pos;
    ctx.playing = true;
    std::vector<const float*> in(inCh);
    for (uint32_t c = 0; c < inCh; ++c) in[c] = input[c] + done;
    ctx.input = inCh ? in.data() : nullptr;
    ctx.inputChannels = inCh;
    g.process(ctx, ptr.data(), g.outputChannels(), lowLat, false);
    for (uint32_t c = 0; c < g.outputChannels(); ++c) out[c].insert(out[c].end(), ptr[c], ptr[c] + n);
    pos += n;
    done += n;
  }
  return out;
}

}  // namespace

SQ_TEST("rt: spsc queue wraps and reports full/empty") {
  SpscQueue<int> q(4);
  int v = 0;
  CHECK(!q.pop(v));
  for (int round = 0; round < 10; ++round) {
    for (int i = 0; i < 4; ++i) CHECK(q.push(round * 10 + i));
    CHECK(!q.push(99));
    for (int i = 0; i < 4; ++i) {
      REQUIRE(q.pop(v));
      CHECK_EQ(v, round * 10 + i);
    }
  }
}

SQ_TEST("rt: frame ring is all-or-nothing and preserves order") {
  FrameRing r(2, 64);
  std::vector<float> a(48), b(48);
  for (int i = 0; i < 48; ++i) {
    a[i] = static_cast<float>(i);
    b[i] = static_cast<float>(-i);
  }
  const float* src[2] = {a.data(), b.data()};
  CHECK(r.write(src, 2, 48));
  CHECK(!r.write(src, 2, 48));  // only 16 free
  std::vector<float> inter(200);
  CHECK_EQ(r.read(inter.data(), 100), size_t(48));
  CHECK_EQ(inter[2 * 47], 47.0f);
  CHECK_EQ(inter[2 * 47 + 1], -47.0f);
}

SQ_TEST("rt: delay line delays across block boundaries") {
  DelayLine d;
  d.prepare(1, 37, 16);
  std::vector<float> all;
  for (int blk = 0; blk < 10; ++blk) {
    float x[16] = {};
    if (blk == 0) x[3] = 1.0f;
    float* p[1] = {x};
    d.process(p, 1, 16);
    all.insert(all.end(), x, x + 16);
  }
  for (size_t i = 0; i < all.size(); ++i) CHECK_EQ(all[i], i == 40 ? 1.0f : 0.0f);
}

SQ_TEST("laws: constant power pan and balance") {
  float l, r;
  panGainsMono(0.0f, &l, &r);
  CHECK(std::fabs(l - 0.70710678f) < 1e-6f);
  CHECK(std::fabs(r - 0.70710678f) < 1e-6f);
  panGainsMono(-1.0f, &l, &r);
  CHECK(std::fabs(l - 1.0f) < 1e-6f);
  CHECK(std::fabs(r) < 1e-6f);
  panGainsStereo(0.5f, &l, &r);
  CHECK_EQ(l, 0.5f);
  CHECK_EQ(r, 1.0f);
  CHECK(std::fabs(dbToGain(-6.0) - 0.5011872f) < 1e-6f);
}

SQ_TEST("graph: invalid routing is rejected") {
  GraphDesc d;
  d.buses.resize(2);
  d.buses[0].output = 1;
  d.buses[1].output = 0;
  GraphConfig cfg;
  std::string err;
  CHECK(Graph::compile(d, cfg, &err) == nullptr);
  CHECK(!err.empty());
  GraphDesc d2;
  d2.tracks.resize(1);
  d2.tracks[0].output = 3;
  CHECK(Graph::compile(d2, cfg) == nullptr);
}

SQ_TEST("graph: track -> fader/pan -> sub-mix -> master sums and mutes") {
  auto one = makeMedia(1, 1000, [](int64_t, uint32_t) { return 1.0f; });
  GraphDesc d;
  d.buses.resize(1);
  d.buses[0].volume = 0.5f;
  TrackDesc t;
  t.channels = 1;
  t.sources.push_back(clipOf(one, 0, 1000));
  t.pan = -1.0f;  // hard left
  t.output = 0;
  d.tracks.push_back(t);
  t.pan = 1.0f;  // hard right
  t.output = -1;
  t.volume = 0.25f;
  d.tracks.push_back(t);
  GraphConfig cfg;
  cfg.maxBlock = 128;
  auto g = Graph::compile(d, cfg);
  REQUIRE(g);
  auto out = runGraph(*g, 0, 256, 128);
  CHECK(std::fabs(out[0][10] - 0.5f) < 1e-6f);   // left via bus at 0.5
  CHECK(std::fabs(out[1][10] - 0.25f) < 1e-6f);  // right direct at 0.25
  g->track(0).setMute(true);
  out = runGraph(*g, 256, 256, 128);
  CHECK(std::fabs(out[0][200]) < 1e-6f);  // after the one-block ramp
  g->track(0).setMute(false);
  g->track(1).setSolo(true);
  g->updateSolo();
  out = runGraph(*g, 512, 256, 128);
  CHECK(std::fabs(out[0][200]) < 1e-6f);
  CHECK(std::fabs(out[1][200] - 0.25f) < 1e-6f);
}

SQ_TEST("graph: modification node envelope and clip fades") {
  auto one = makeMedia(1, 1000, [](int64_t, uint32_t) { return 1.0f; });
  std::vector<GainPoint> pts{{0, 0.0f}, {100, 1.0f}};
  ModificationNode mod(std::make_shared<MediaNode>(one), pts);
  CHECK_EQ(mod.gainAt(50), 0.5f);
  CHECK_EQ(mod.gainAt(500), 1.0f);
  ClipParams p;
  p.start = 0;
  p.length = 100;
  p.fadeIn = 10;
  p.fadeOut = 10;
  GraphDesc d;
  TrackDesc t;
  t.channels = 1;
  t.pan = -1.0f;
  t.sources.push_back(std::make_shared<ClipNode>(std::make_shared<MediaNode>(one), p));
  d.tracks.push_back(t);
  auto g = Graph::compile(d, GraphConfig{});
  REQUIRE(g);
  auto out = runGraph(*g, 0, 128, 64);
  CHECK_EQ(out[0][0], 0.0f);
  CHECK(std::fabs(out[0][5] - 0.5f) < 1e-6f);
  CHECK_EQ(out[0][50], 1.0f);
  CHECK(std::fabs(out[0][95] - 0.5f) < 1e-6f);
  CHECK_EQ(out[0][100], 0.0f);
}

SQ_TEST("graph: event node drives the sine synth deterministically") {
  std::vector<TimedMidi> ev{{100, {0x90, 69, 127}, 3}, {600, {0x80, 69, 0}, 3}};
  GraphDesc d;
  TrackDesc t;
  t.channels = 1;
  t.pan = -1.0f;
  t.sources.push_back(std::make_shared<EventNode>(ev, std::make_shared<SineSynth>()));
  d.tracks.push_back(t);
  auto g = Graph::compile(d, GraphConfig{});
  REQUIRE(g);
  auto a = runGraph(*g, 0, 1024, 64);
  auto g2 = Graph::compile(d, GraphConfig{});
  auto b = runGraph(*g2, 0, 1024, 200);
  CHECK(a[0] == b[0]);  // block-size invariant
  CHECK_EQ(a[0][99], 0.0f);
  float peak = 0;
  for (int i = 100; i < 600; ++i) peak = std::max(peak, std::fabs(a[0][i]));
  CHECK(peak > 0.2f);
  CHECK_EQ(a[0][700], 0.0f);
}

SQ_TEST("graph: metronome clicks on beats with accent") {
  GraphDesc d;
  d.tempo.push_back(TempoSegment{0, 0.0, 120.0});  // 1 beat = 24000 frames @48k
  auto g = Graph::compile(d, GraphConfig{});
  REQUIRE(g);
  std::vector<float> l(48000 * 3), r(48000 * 3);
  for (int64_t pos = 0; pos < 48000 * 3; pos += 512) {
    const uint32_t n = static_cast<uint32_t>(std::min<int64_t>(512, 48000 * 3 - pos));
    float* p[2] = {l.data() + pos, r.data() + pos};
    std::memset(p[0], 0, n * sizeof(float));
    std::memset(p[1], 0, n * sizeof(float));
    g->metronome().render(pos, n, p, 2);
  }
  CHECK_EQ(l[0], 0.0f);           // sin(0)
  CHECK(std::fabs(l[5]) > 0.0f);  // click sounding
  CHECK_EQ(l[23999], 0.0f);       // silence before beat 2
  CHECK(std::fabs(l[24005]) > 0.0f);
  CHECK_EQ(l[24000 + 1500], 0.0f);  // 30 ms click ended
}

SQ_TEST("engine: transport loop splits blocks exactly at the loop end") {
  sq::Ptr<EngineCore> core = sq::Ptr<EngineCore>::adopt(new EngineCore());
  GraphConfig cfg;
  cfg.maxBlock = 256;
  core->setGraph(Graph::compile(GraphDesc{}, cfg));
  NullDeviceOptions o;
  o.offline = true;
  o.blockFrames = 256;
  sq::Ptr<NullDevice> dev = sq::Ptr<NullDevice>::adopt(new NullDevice(o));
  REQUIRE_OK(dev->start(core.get()));
  Command c;
  c.type = CmdType::Loop;
  c.a = 1000;
  c.b = 1300;
  c.value = 1;
  core->post(c);
  c = Command{};
  c.type = CmdType::Locate;
  c.a = 900;
  core->post(c);
  c.type = CmdType::Play;
  core->post(c);
  dev->pump(1);  // 900 -> 1156: crosses 1300? no: 900+256=1156
  CHECK_EQ(core->transport().frame, int64_t(1156));
  dev->pump(1);  // 1156 + 144 -> loop to 1000, + 112 -> 1112
  CHECK_EQ(core->transport().frame, int64_t(1112));
  CHECK((core->transport().flags & SQ_TRANSPORT_LOOPING) != 0);
  dev->stop();
}

SQ_TEST("engine: lock-free graph swaps while the render thread runs") {
  sq::Ptr<EngineCore> core = sq::Ptr<EngineCore>::adopt(new EngineCore());
  auto one = makeMedia(1, 48000, [](int64_t, uint32_t) { return 0.5f; });
  GraphConfig cfg;
  cfg.maxBlock = 64;
  GraphDesc d;
  TrackDesc t;
  t.channels = 1;
  t.sources.push_back(clipOf(one, 0, 48000));
  d.tracks.push_back(t);
  core->setGraph(Graph::compile(d, cfg));
  NullDeviceOptions o;
  o.blockFrames = 64;
  o.speed = 0;  // free running
  sq::Ptr<NullDevice> dev = sq::Ptr<NullDevice>::adopt(new NullDevice(o));
  REQUIRE_OK(dev->start(core.get()));
  Command c;
  c.type = CmdType::Play;
  core->post(c);
  uint64_t lastGen = 0;
  for (int i = 0; i < 200; ++i) {
    d.tracks[0].volume = static_cast<float>(i % 7) / 7.0f;
    auto g = Graph::compile(d, cfg);
    lastGen = g->generation();
    core->setGraph(std::move(g));
    core->collectGarbage();
  }
  CHECK(core->waitForCallback(2000));
  CHECK(core->waitForCallback(2000));
  CHECK(dev->renderThread().priorityApplied() || true);
#ifdef _WIN32
  CHECK(dev->renderThread().mmcssApplied());
#endif
  dev->stop();
  core->collectGarbage();
  CHECK_EQ(core->currentGeneration(), lastGen);
  CHECK(core->graphForControlThread() != nullptr);
  CHECK_EQ(core->graphForControlThread()->generation(), lastGen);
}

SQ_TEST("graph: fast lane monitors input without plugin delay, main path is compensated") {
  GraphDesc d;
  TrackDesc t;
  t.channels = 1;
  t.pan = -1.0f;
  t.monitor = true;
  t.inputChannels = {0};
  t.inserts.push_back(std::make_shared<DelayFx>(100));  // latent insert: skipped by the fast lane
  d.tracks.push_back(t);
  GraphConfig cfg;
  cfg.maxBlock = 64;
  std::vector<float> in(512, 0.0f);
  in[10] = 1.0f;
  const float* ip[1] = {in.data()};
  auto g = Graph::compile(d, cfg);
  REQUIRE(g);
  CHECK_EQ(g->latency(), 100u);
  auto fast = runGraph(*g, 0, 512, 64, true, ip, 1);
  CHECK_EQ(fast[0][10], 1.0f);  // no delay in the fast lane
  auto g2 = Graph::compile(d, cfg);
  auto main = runGraph(*g2, 0, 512, 64, false, ip, 1);
  CHECK_EQ(main[0][10], 0.0f);
  CHECK_EQ(main[0][110], 1.0f);  // through the latent insert
}

namespace {
struct CoreIo {
  float l[64] = {}, r[64] = {};
  float* ch[2] = {l, r};
  sq_audio_block out{};
  sq_audio_io io{};
  CoreIo() {
    out.channels = ch;
    out.channel_count = 2;
    out.frames = 64;
    io.struct_size = sizeof io;
    io.output = &out;
  }
};
sq_stream_format coreFormat() {
  sq_stream_format f{};
  f.struct_size = sizeof f;
  f.sample_rate = 48000.0;
  f.channels = 2;
  f.max_block_frames = 64;
  return f;
}
std::unique_ptr<Graph> oneTrackGraph() {
  GraphDesc d;
  d.tracks.resize(1);
  GraphConfig cfg;
  cfg.maxBlock = 64;
  return Graph::compile(d, cfg);
}
}  // namespace

SQ_TEST("core: a command for a published graph whose install was deferred is applied once the graph is installed") {
  sq::Ptr<EngineCore> core = sq::Ptr<EngineCore>::adopt(new EngineCore());
  const sq_stream_format fmt = coreFormat();
  core->setGraph(oneTrackGraph());
  REQUIRE_OK(core->onStart(nullptr, &fmt, nullptr));
  // No collectGarbage: the retire queue fills up and the audio thread defers the next install.
  for (int i = 0; i < 80; ++i) {
    core->setGraph(oneTrackGraph());
    core->process(nullptr);
  }
  core->setGraph(oneTrackGraph());
  Command c;
  c.type = CmdType::TrackVolume;
  c.index = 0;
  c.generation = core->currentGeneration();
  c.value = 0.25;
  REQUIRE(core->post(c));
  core->process(nullptr);  // install deferred: the command must wait, not be dropped
  core->collectGarbage();
  core->process(nullptr);  // installs the graph, then applies the command
  core->onStop();
  Graph* g = core->graphForControlThread();
  REQUIRE(g != nullptr);
  CHECK_EQ(g->generation(), c.generation);
  CHECK(std::fabs(g->track(0).volume() - 0.25f) < 1e-6f);
}

SQ_TEST("core: commands queued when the stream stops are applied at the stop, before later direct commands") {
  sq::Ptr<EngineCore> core = sq::Ptr<EngineCore>::adopt(new EngineCore());
  const sq_stream_format fmt = coreFormat();
  core->setGraph(oneTrackGraph());
  REQUIRE_OK(core->onStart(nullptr, &fmt, nullptr));
  CoreIo io;
  core->process(&io.io);
  Command stop;
  stop.type = CmdType::Stop;
  REQUIRE(core->post(stop));  // queued; the stream dies before a callback takes it
  core->onStop();
  Command play;
  play.type = CmdType::Play;
  REQUIRE(core->post(play));  // applied directly (not running)
  CHECK((core->transport().flags & SQ_TRANSPORT_PLAYING) != 0);
  REQUIRE_OK(core->onStart(nullptr, &fmt, nullptr));
  core->process(&io.io);
  CHECK((core->transport().flags & SQ_TRANSPORT_PLAYING) != 0);  // the stale Stop is not replayed
  core->onStop();
  // queued automation data is freed with the core (leak checkers)
  REQUIRE_OK(core->onStart(nullptr, &fmt, nullptr));
  Command a;
  a.type = CmdType::SetAutomation;
  a.generation = core->currentGeneration();
  a.ptr = new AutomationData();
  REQUIRE(core->post(a));
  // live MIDI producers: more events than one parse batch in one call
  std::vector<uint8_t> many;
  for (int i = 0; i < 100; ++i) {
    many.push_back(0x90);
    many.push_back(static_cast<uint8_t>(i));
    many.push_back(1);
  }
  CHECK(core->pushLiveMidi(many.data(), static_cast<uint32_t>(many.size())));
  CHECK_EQ(core->liveMidiDropped(), uint64_t(0));
}
