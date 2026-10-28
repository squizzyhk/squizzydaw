// engine/audio - track instruments (one instance per track, resolver fallback), live MIDI while
// stopped, MIDI recording, automation (offline render, live swap without recompile), sends with
// PDC and feedback guards, live parameter edits without recompile, metronome.
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "fakes.hpp"
#include "sq/app/settings.hpp"
#include "sq/engine/audio/builtin.hpp"
#include "sq/engine/audio/devices.hpp"
#include "sq/engine/audio/doc_builder.hpp"
#include "sq/engine/audio/engine.hpp"
#include "sq/engine/audio/render.hpp"
#include "sqtest.hpp"

using namespace sq::engine::audio;

namespace {

std::atomic<int> g_synthClones{0};
std::atomic<int> g_probeValueMilli{-1};
std::atomic<int> g_probeLastIndex{-1};   // last setParameter call on any ProbeFx (any index)
std::atomic<int> g_probeLastMilli{-1};

// Instrument that counts its instances and plays 0.5 per sounding note. CC64 sustains released
// notes until the pedal goes up; CC120/CC123 silence everything. Optional reported latency.
class CountingSynth final : public SynthProcessor {
 public:
  explicit CountingSynth(uint32_t latency = 0) : latency_(latency) {}
  ProcessorPtr clone() const override {
    ++g_synthClones;
    return std::make_unique<CountingSynth>(latency_);
  }
  bool prepare(double, uint32_t, uint32_t) override { return true; }
  void reset() noexcept override {
    held_ = sustained_ = 0;
    pedal_ = false;
  }
  uint32_t latency() const noexcept override { return latency_; }
  void process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept override {
    uint32_t ev = 0;
    for (uint32_t i = 0; i < ctx.frames; ++i) {
      while (ev < ctx.midiCount && ctx.midi[ev].frame_offset <= i) {
        const auto& e = ctx.midi[ev++];
        const uint8_t st = e.bytes[0] & 0xF0u;
        if (st == 0x90 && e.bytes[2] > 0) ++held_;
        else if ((st == 0x80 || st == 0x90) && held_ > 0) {
          --held_;
          if (pedal_) ++sustained_;
        } else if (st == 0xB0 && e.bytes[1] == 64) {
          pedal_ = e.bytes[2] >= 64;
          if (!pedal_) sustained_ = 0;
        } else if (st == 0xB0 && (e.bytes[1] == 120 || e.bytes[1] == 123)) {
          held_ = sustained_ = 0;
        }
      }
      const int sounding = held_ + sustained_;
      if (sounding > 0)
        for (uint32_t c = 0; c < channels; ++c) io[c][i] += 0.5f * static_cast<float>(sounding);
    }
  }

 private:
  uint32_t latency_;
  int held_ = 0, sustained_ = 0;
  bool pedal_ = false;
};


// Gain insert with one parameter (index 0 = linear gain) that reports the live value.
class ProbeFx final : public FxProcessor {
 public:
  explicit ProbeFx(float g) : gain_(g) {}
  ProcessorPtr clone() const override { return std::make_unique<ProbeFx>(gain_); }
  bool prepare(double, uint32_t, uint32_t) override { return true; }
  void process(const ProcessContext& ctx, float* const* io, uint32_t channels) noexcept override {
    for (uint32_t c = 0; c < channels; ++c)
      for (uint32_t i = 0; i < ctx.frames; ++i) io[c][i] *= gain_;
  }
  uint32_t parameterCount() const noexcept override { return 1; }
  void setParameter(uint32_t index, float v) noexcept override {
    g_probeLastIndex.store(static_cast<int>(index));
    g_probeLastMilli.store(static_cast<int>(std::lround(v * 1000.0f)));
    if (index != 0) return;
    gain_ = v;
    g_probeValueMilli.store(static_cast<int>(std::lround(v * 1000.0f)));
  }
  float parameter(uint32_t index) const noexcept override { return index == 0 ? gain_ : 0.0f; }

 private:
  float gain_;
};

// "test|probe||0=<gain>" -> ProbeFx (default gain 1); "test|synth|" -> CountingSynth.
ProcessorProto testResolver(const std::string& spec, std::string* err) {
  SpecParts p;
  if (!parseSpec(spec, &p) || p.format != "test") {
    if (err) *err = "not a test spec";
    return nullptr;
  }
  if (p.id == "synth") return std::make_shared<CountingSynth>();
  if (p.id == "probe") {
    float g = 1.0f;
    for (auto& [i, v] : p.params)
      if (i == 0) g = static_cast<float>(v);
    return std::make_shared<ProbeFx>(g);
  }
  if (err) *err = "unknown";
  return nullptr;
}

sq::Ptr<fakes::MTrack> addTrack(fakes::MDoc& doc, int32_t kind, uint32_t channels, const char* name) {
  auto t = sq::Ptr<fakes::MTrack>::adopt(new fakes::MTrack());
  t->name = name;
  t->info.kind = kind;
  t->info.channels = channels;
  doc.tracks.push_back(t);
  return t;
}

sq::Ptr<fakes::MClip> midiClip(double start, double length, std::vector<std::pair<double, double>> notes) {
  auto m = sq::Ptr<fakes::MClip>::adopt(new fakes::MClip());
  m->info.kind = SQ_CLIP_MIDI;
  m->info.start = start;
  m->info.length = length;
  for (auto& [pos, len] : notes) {
    sq_clip_event e{};
    e.struct_size = sizeof e;
    e.position = pos;
    e.length = len;
    e.bytes[0] = 0x90;
    e.bytes[1] = 60;
    e.bytes[2] = 100;
    e.size = 3;
    m->events.push_back(e);
  }
  return m;
}

std::string guidText(const sq_guid& g) { return sq::toString(g); }

NullDevice* dev(AudioEngine& eng) { return static_cast<NullDevice*>(eng.device()); }

struct OutCapture {
  std::mutex m;
  float peak = 0.0f;
  void reset() {
    std::lock_guard<std::mutex> lk(m);
    peak = 0.0f;
  }
  float take() {
    std::lock_guard<std::mutex> lk(m);
    float p = peak;
    peak = 0.0f;
    return p;
  }
};

}  // namespace

SQ_TEST("instruments: one instrument instance per track, merged clips, resolver fallback to the default") {
  sq::app::ClassRegistry reg;
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  auto keys = addTrack(*doc, SQ_TRACK_INSTRUMENT, 1, "Keys");
  keys->strProps["user.instrument"] = "test|synth|";
  keys->clips.push_back(midiClip(0, 2, {{0.0, 4.0}}));  // note longer than the clip: cut at 2 ppq
  keys->clips.push_back(midiClip(2, 2, {{0.0, 1.0}}));
  auto empty = addTrack(*doc, SQ_TRACK_INSTRUMENT, 1, "Empty");  // no clips: instrument still exists
  empty->strProps["user.instrument"] = "nope|missing|";
  InsertResolver res = testResolver;
  int defaultAsked = 0;
  InstrumentResolver inst = [&](const std::string& spec, std::string* err) -> ProcessorProto {
    if (spec == kDefaultInstrument) {
      ++defaultAsked;
      return std::make_shared<CountingSynth>();
    }
    return testResolver(spec, err);
  };
  MediaCache cache;
  GraphDesc d;
  DocBuildResult info;
  REQUIRE_OK(buildGraphDesc(doc.get(), &reg, cache, 48000.0, &d, &info, &res, &inst));
  REQUIRE_EQ(d.tracks.size(), size_t(2));
  CHECK(d.tracks[0].instrument != nullptr);
  CHECK(d.tracks[0].sources.empty());  // MIDI clips are not sources any more
  CHECK_EQ(d.tracks[0].midi.size(), size_t(4));  // 2 note-ons + 2 note-offs
  CHECK_EQ(d.tracks[0].midi[1].frame, int64_t(48000));  // first note cut at the clip end (2 ppq @120)
  CHECK(d.tracks[1].instrument != nullptr);
  CHECK_EQ(defaultAsked, 1);
  CHECK(!info.warnings.empty());

  g_synthClones = 0;
  GraphConfig cfg;
  cfg.maxBlock = 256;
  auto g = Graph::compile(d, cfg);
  REQUIRE(g);
  CHECK_EQ(g_synthClones.load(), 2);  // exactly one per track, whatever the clip count
  CHECK(g->track(0).instrument() != nullptr);

  // offline render: the note sounds 0..2 ppq on clip 1 and 2..3 ppq on clip 2, silence after
  RenderJob job;
  job.start = 0;
  job.end = 96000 + 24000;
  job.outputChannels = 1;
  std::vector<std::vector<float>> master;
  d.tracks.pop_back();
  REQUIRE_OK(renderToMemory(d, job, &master));
  CHECK(master[0][100] > 0.4f);
  CHECK(master[0][60000] > 0.4f);
  CHECK(std::fabs(master[0][80000]) < 1e-6f);
  CHECK(std::fabs(master[0][110000]) < 1e-6f);
}

SQ_TEST("instruments: live MIDI plays the target track's instrument while the transport is stopped") {
  sq::app::ClassRegistry reg;
  registerNullDriver(&reg);
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  auto keys = addTrack(*doc, SQ_TRACK_INSTRUMENT, 1, "Keys");
  keys->strProps["user.instrument"] = "test|synth|";
  auto other = addTrack(*doc, SQ_TRACK_INSTRUMENT, 1, "Other");
  other->strProps["user.instrument"] = "test|synth|";
  OutCapture cap;
  NullDriver::defaults()[1].blockFrames = 128;
  NullDriver::defaults()[1].outputSink = [&cap](int64_t, const float* const* ch, uint32_t n, uint32_t frames) {
    std::lock_guard<std::mutex> lk(cap.m);
    for (uint32_t c = 0; c < n; ++c)
      for (uint32_t i = 0; i < frames; ++i) cap.peak = std::max(cap.peak, std::fabs(ch[c][i]));
  };
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, nullptr));
  eng->setInsertResolver(testResolver);
  eng->setInstrumentResolver(testResolver);
  REQUIRE_OK(eng->setDocument(doc.get()));
  REQUIRE_OK(eng->openDevice(&kNullDriverClassId, "offline", nullptr));
  dev(*eng)->pump(4);
  CHECK(cap.take() < 1e-6f);

  // no target: nothing sounds
  const uint8_t on[3] = {0x90, 60, 100}, off[3] = {0x80, 60, 0};
  CHECK(eng->pushLiveMidi(on, 3));
  dev(*eng)->pump(4);
  CHECK(cap.take() < 1e-6f);
  CHECK(eng->pushLiveMidi(off, 3));
  dev(*eng)->pump(2);

  // selected track = target; transport stopped
  eng->setLiveMidiTarget(keys->id);
  REQUIRE_EQ(eng->liveMidiTracks().size(), size_t(1));
  const uint64_t compiles = eng->compileCount();
  CHECK(eng->pushLiveMidi(on, 3));
  dev(*eng)->pump(4);
  sq_meter_value mv[2];
  uint32_t n = 0;
  sq_guid kid = keys->id, oid = other->id;
  REQUIRE_OK(eng->getMeter(&kid, mv, 2, &n));
  CHECK(mv[0].peak > 0.3f);
  REQUIRE_OK(eng->getMeter(&oid, mv, 2, &n));
  CHECK(mv[0].peak < 1e-6f);
  CHECK(cap.take() > 0.3f);
  CHECK(eng->pushLiveMidi(off, 3));
  dev(*eng)->pump(4);
  cap.take();
  dev(*eng)->pump(2);
  CHECK(cap.take() < 1e-6f);

  // arming wins over the selection: running status, two messages in one buffer
  other->info.flags |= SQ_TRACK_ARMED;
  doc->fireChanged();
  CHECK_EQ(eng->compileCount(), compiles);  // arm = live routing command, no recompile
  REQUIRE_EQ(eng->liveMidiTracks().size(), size_t(1));
  CHECK(eng->liveMidiTracks()[0] == other->id);
  const uint8_t two[5] = {0x90, 62, 90, 64, 90};
  CHECK(eng->pushLiveMidi(two, 5));
  dev(*eng)->pump(4);
  REQUIRE_OK(eng->getMeter(&oid, mv, 2, &n));
  CHECK(mv[0].peak > 0.6f);  // two held notes (1.0 at -3 dB centre pan)
  REQUIRE_OK(eng->getMeter(&kid, mv, 2, &n));
  CHECK(mv[0].peak < 1e-6f);
  eng->closeDevice();
  NullDriver::defaults()[1].outputSink = nullptr;
  NullDriver::defaults()[1].blockFrames = 256;
}

SQ_TEST("instruments: recording with an armed instrument track creates a MIDI clip with the played notes") {
  sq::app::ClassRegistry reg;
  registerNullDriver(&reg);
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  auto keys = addTrack(*doc, SQ_TRACK_INSTRUMENT, 1, "Keys");
  keys->info.flags |= SQ_TRACK_ARMED;
  NullDriver::defaults()[1].blockFrames = 240;
  sq::app::Settings settings;
  settings.setString("Media.Folder", "mem://media");
  settings.setString("Media.RecordFormat", "tst");
  fakes::registerFakeCodec(&reg);
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, &settings));
  REQUIRE_OK(eng->setDocument(doc.get()));
  REQUIRE_OK(eng->openDevice(&kNullDriverClassId, "offline", nullptr));
  REQUIRE_OK(eng->locate(4.0));  // 96000
  REQUIRE_OK(eng->play());
  REQUIRE_OK(eng->record(SQ_TRUE));
  dev(*eng)->pump(100);  // 24000 frames = 1 ppq
  const uint8_t on[3] = {0x90, 64, 80}, off[3] = {0x80, 64, 0}, cc[3] = {0xB0, 1, 33};
  eng->pushLiveMidi(on, 3);
  eng->pushLiveMidi(cc, 3);
  dev(*eng)->pump(50);  // 0.5 ppq
  eng->pushLiveMidi(off, 3);
  dev(*eng)->pump(50);
  REQUIRE_OK(eng->record(SQ_FALSE));
  REQUIRE_EQ(eng->lastMidiTake().size(), size_t(3));
  CHECK(eng->lastTakes().empty());  // instrument tracks do not record device audio
  auto& lg = doc->editor->log;
  REQUIRE(lg.size() >= 4);
  CHECK(lg[0].action == "begin");
  CHECK(lg[1].action == "add");
  CHECK(lg[1].node == keys->id);
  CHECK_EQ(lg[1].kind, int64_t(SQ_CLIP_MIDI));
  CHECK(std::fabs(lg[1].a - 4.0) < 1e-9);  // clip starts at the record start
  CHECK(lg[2].action == "events");
  REQUIRE_EQ(lg[2].events.size(), size_t(2));  // note (with length) + CC
  CHECK_EQ(lg[2].events[0].bytes[0], 0x90);
  CHECK_EQ(lg[2].events[0].bytes[1], 64);
  CHECK(std::fabs(lg[2].events[0].position - 1.0) < 0.02);
  CHECK(std::fabs(lg[2].events[0].length - 0.5) < 0.02);
  CHECK_EQ(lg[2].events[1].bytes[0], 0xB0);
  CHECK(lg[3].action == "commit");
  eng->closeDevice();
  NullDriver::defaults()[1].blockFrames = 256;
}

SQ_TEST("automation: a volume lane changes the level over time in an offline mixdown; lanes on buses") {
  sq::app::ClassRegistry reg;
  fakes::registerFakeCodec(&reg);
  fakes::putMemFile("mem://auto.tst", 48000.0, std::vector<std::vector<float>>(1, std::vector<float>(96000 * 2, 0.5f)));
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  auto bus = addTrack(*doc, SQ_TRACK_BUS, 1, "Bus");
  auto t = addTrack(*doc, SQ_TRACK_AUDIO, 1, "Audio");
  t->info.output = bus->id;
  auto c = sq::Ptr<fakes::MClip>::adopt(new fakes::MClip());
  c->info.start = 0;
  c->info.length = 8;
  c->path = "mem://auto.tst";
  t->clips.push_back(c);
  t->strProps["automation"] = "volume 0:0 4:-20.0000\n";
  bus->strProps["automation"] = "mute 0:0 6:1\n";
  MediaCache cache;
  GraphDesc d;
  DocBuildResult info;
  REQUIRE_OK(buildGraphDesc(doc.get(), &reg, cache, 48000.0, &d, &info));
  REQUIRE(d.automation != nullptr);
  RenderJob job;
  job.start = 0;
  job.end = 192000;
  job.outputChannels = 1;
  std::vector<std::vector<float>> m;
  REQUIRE_OK(renderToMemory(d, job, &m));
  CHECK(std::fabs(m[0][10] - 0.5f) < 1e-3f);                 // 0 dB at the start
  CHECK(std::fabs(m[0][48000] - 0.5f * dbToGain(-10.0)) < 0.01f);  // -10 dB half way
  CHECK(std::fabs(m[0][120000] - 0.5f * dbToGain(-20.0)) < 0.01f);
  CHECK(std::fabs(m[0][170000]) < 1e-6f);                    // bus muted from 6 ppq

  // the same document through the engine's mixdown task
  registerNullDriver(&reg);
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, nullptr));
  REQUIRE_OK(eng->setDocument(doc.get()));
  sq_render_request rq{};
  rq.struct_size = sizeof rq;
  rq.kind = SQ_RENDER_MIXDOWN;
  rq.start = 0;
  rq.end = 8;
  rq.channels = 1;
  rq.output = "mem://auto_mix.tst";
  rq.file_extension = "tst";
  sq::Ptr<sq::IRenderTask> task;
  REQUIRE_OK(eng->startRender(&rq, nullptr, task.put()));
  REQUIRE_OK(task->wait(30000));
  REQUIRE_OK(task->getResult());
  eng->idle();
  fakes::MemFile mf;
  REQUIRE(fakes::getMemFile("mem://auto_mix.tst", &mf));
  CHECK(std::fabs(mf.data[0][48000] - 0.5f * dbToGain(-10.0)) < 0.01f);
  CHECK(std::fabs(mf.data[0][170000]) < 1e-6f);
}

SQ_TEST("automation + params: editing parameters, send levels and automation does not recompile") {
  sq::app::ClassRegistry reg;
  registerNullDriver(&reg);
  fakes::registerFakeCodec(&reg);
  fakes::putMemFile("mem://p.tst", 48000.0, std::vector<std::vector<float>>(1, std::vector<float>(96000 * 4, 0.5f)));
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  auto fx = addTrack(*doc, SQ_TRACK_FX, 2, "Reverb");
  auto t = addTrack(*doc, SQ_TRACK_AUDIO, 1, "Audio");
  auto c = sq::Ptr<fakes::MClip>::adopt(new fakes::MClip());
  c->info.start = 0;
  c->info.length = 16;
  c->path = "mem://p.tst";
  t->clips.push_back(c);
  t->strProps["user.inserts"] = "test|probe||0=0.5\n!test|probe||0=0\ntest|probe|";
  t->strProps["user.sends"] = guidText(fx->id) + " -6";
  NullDriver::defaults()[1].blockFrames = 128;
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, nullptr));
  eng->setInsertResolver(testResolver);
  REQUIRE_OK(eng->setDocument(doc.get()));
  REQUIRE(eng->buildWarnings().empty());
  REQUIRE_EQ(eng->graphDesc().tracks[0].inserts.size(), size_t(2));  // the bypassed line is left out
  REQUIRE_OK(eng->openDevice(&kNullDriverClassId, "offline", nullptr));
  REQUIRE_OK(eng->play());
  dev(*eng)->pump(8);
  const uint64_t compiles = eng->compileCount();
  const uint64_t gen = eng->core().currentGeneration();

  // parameter edit in a spec -> RT setParameter on the live processor
  t->strProps["user.inserts"] = "test|probe||0=0.25\n!test|probe||0=0\ntest|probe|";
  doc->fireChanged();
  dev(*eng)->pump(2);
  CHECK_EQ(eng->compileCount(), compiles);
  CHECK_EQ(eng->core().currentGeneration(), gen);
  CHECK_EQ(g_probeValueMilli.load(), 250);
  // back to the default (value removed from the spec): the default reaches the processor
  t->strProps["user.inserts"] = "test|probe|\n!test|probe||0=0\ntest|probe|";
  doc->fireChanged();
  dev(*eng)->pump(2);
  CHECK_EQ(eng->compileCount(), compiles);
  CHECK_EQ(g_probeValueMilli.load(), 1000);

  // send level edit
  sq_guid fid = fx->id;
  sq_meter_value mv[2];
  uint32_t n = 0;
  dev(*eng)->pump(8);
  REQUIRE_OK(eng->getMeter(&fid, mv, 2, &n));
  const float before = mv[0].peak;
  CHECK(before > 0.05f);
  t->strProps["user.sends"] = guidText(fx->id) + " -200";
  doc->fireChanged();
  CHECK_EQ(eng->compileCount(), compiles);
  dev(*eng)->pump(8);
  REQUIRE_OK(eng->getMeter(&fid, mv, 2, &n));
  dev(*eng)->pump(8);
  REQUIRE_OK(eng->getMeter(&fid, mv, 2, &n));
  CHECK(mv[0].peak < 1e-4f);

  // automation edit (insert parameter lane on slot 2 = chain index 1) -> swapped live
  t->strProps["automation"] = "insert.2.0 0:0.125 100:0.125\n";
  doc->fireChanged();
  CHECK_EQ(eng->compileCount(), compiles);
  dev(*eng)->pump(2);
  CHECK_EQ(g_probeValueMilli.load(), 125);
  // stop: automated parameters go back to their static values
  REQUIRE_OK(eng->stop());
  dev(*eng)->pump(2);
  CHECK_EQ(g_probeValueMilli.load(), 1000);

  // structural edit (bypass toggled) still recompiles
  t->strProps["user.inserts"] = "test|probe|\ntest|probe||0=0\ntest|probe|";
  doc->fireChanged();
  CHECK_EQ(eng->compileCount(), compiles + 1);
  eng->closeDevice();
  NullDriver::defaults()[1].blockFrames = 256;
}

SQ_TEST("sends: route audio to a bus (pre/post fader) with delay compensation; feedback loops are ignored") {
  // graph level: track (fader -inf) -> pre-fader send -> bus; post-fader send silent
  GraphDesc d;
  BusDesc fx;
  fx.channels = 1;
  fx.inserts.push_back(std::make_shared<DelayFx>(64));  // latent bus
  d.buses.push_back(fx);
  TrackDesc a;
  a.channels = 1;
  a.volume = 0.0f;
  std::vector<std::vector<float>> data(1, std::vector<float>(4800, 0.0f));
  data[0][1000] = 1.0f;
  auto media = std::make_shared<MediaBuffer>();
  media->channels = 1;
  media->frames = 4800;
  media->data = data;
  a.sources.push_back(std::make_shared<MediaNode>(media));
  a.sends.push_back(SendDesc{0, 0.5f, true});
  TrackDesc b = a;  // dry track to master (unity) with a post-fader send
  b.volume = 1.0f;
  b.sends = {SendDesc{0, 0.5f, false}};
  d.tracks.push_back(a);
  d.tracks.push_back(b);
  GraphConfig cfg;
  cfg.maxBlock = 128;
  cfg.outputChannels = 1;
  auto g = Graph::compile(d, cfg);
  REQUIRE(g);
  CHECK_EQ(g->latency(), 64u);
  CHECK_EQ(g->trackCompensation(1), 64u);  // the dry path waits for the latent send path
  CHECK_EQ(g->sendCompensation(0, 0), 0u);
  RenderJob job;
  job.start = 0;
  job.end = 4800;
  job.outputChannels = 1;
  std::vector<std::vector<float>> m;
  REQUIRE_OK(renderToMemory(d, job, &m));
  // impulse at 1000: dry (track b) 1.0 + send of a (pre, 0.5) + send of b (post, 0.5) all aligned
  CHECK(std::fabs(m[0][1000] - 2.0f) < 1e-4f);
  CHECK(std::fabs(m[0][999]) < 1e-6f);
  CHECK(std::fabs(m[0][1001]) < 1e-6f);

  // document level: cycles between buses are dropped with a warning
  sq::app::ClassRegistry reg;
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  auto b1 = addTrack(*doc, SQ_TRACK_BUS, 2, "B1");
  auto b2 = addTrack(*doc, SQ_TRACK_FX, 2, "B2");
  b1->strProps["user.sends"] = guidText(b2->id) + " 0";
  b2->strProps["user.sends"] = guidText(b1->id) + " 0\n" + guidText(b2->id) + " 0 pre";
  MediaCache cache;
  GraphDesc dd;
  DocBuildResult info;
  REQUIRE_OK(buildGraphDesc(doc.get(), &reg, cache, 48000.0, &dd, &info));
  CHECK_EQ(dd.buses[0].sends.size(), size_t(1));
  CHECK_EQ(dd.buses[1].sends.size(), size_t(0));
  CHECK_EQ(info.warnings.size(), size_t(2));
  GraphConfig cfg2;
  CHECK(Graph::compile(dd, cfg2) != nullptr);
}

SQ_TEST("metronome: clicks on every beat, accent on the bar start, during playback") {
  GraphDesc d;
  RenderJob job;
  job.kind = kRenderClick;
  job.start = 0;
  job.end = 48000 * 2;  // one 4/4 bar at 120 bpm
  job.outputChannels = 1;
  std::vector<std::vector<float>> m;
  REQUIRE_OK(renderToMemory(d, job, &m));
  auto peak = [&](int64_t from) {
    float p = 0;
    for (int64_t i = from; i < from + 1000; ++i) p = std::max(p, std::fabs(m[0][static_cast<size_t>(i)]));
    return p;
  };
  const float bar = peak(0), beat2 = peak(24000), beat3 = peak(48000), gap = peak(12000);
  CHECK(bar > 0.3f);
  CHECK(beat2 > 0.1f);
  CHECK(beat3 > 0.1f);
  CHECK(bar > beat2 * 1.3f);
  CHECK(gap < 1e-6f);

  // through the engine: transport playing + metronome on -> audible clicks on the device
  sq::app::ClassRegistry reg;
  registerNullDriver(&reg);
  OutCapture cap;
  NullDriver::defaults()[1].outputSink = [&cap](int64_t, const float* const* ch, uint32_t n, uint32_t frames) {
    std::lock_guard<std::mutex> lk(cap.m);
    for (uint32_t c = 0; c < n; ++c)
      for (uint32_t i = 0; i < frames; ++i) cap.peak = std::max(cap.peak, std::fabs(ch[c][i]));
  };
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, nullptr));
  REQUIRE_OK(eng->openDevice(&kNullDriverClassId, "offline", nullptr));
  REQUIRE_OK(eng->setMetronome(SQ_TRUE));
  dev(*eng)->pump(8);
  CHECK(cap.take() < 1e-6f);  // stopped: no click
  REQUIRE_OK(eng->play());
  dev(*eng)->pump(8);
  CHECK(cap.take() > 0.3f);
  eng->closeDevice();
  NullDriver::defaults()[1].outputSink = nullptr;
}

SQ_TEST("instruments: built-in synth parameter edits reach the live instance without a recompile") {
  const DeviceInfo* info = findBuiltin("synth");
  REQUIRE(info != nullptr);
  REQUIRE(!info->params.empty());
  const ParamInfo& pi = info->params[0];
  const double v1 = pi.min + (pi.max - pi.min) * 0.25, v2 = pi.min + (pi.max - pi.min) * 0.75;
  sq::app::ClassRegistry reg;
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  auto keys = addTrack(*doc, SQ_TRACK_INSTRUMENT, 2, "Keys");
  keys->strProps["user.instrument"] = specSetParam(kDefaultInstrument, 0, v1);
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, nullptr));
  eng->setInstrumentResolver([](const std::string& spec, std::string* err) -> ProcessorProto {
    SpecParts p;
    if (!parseSpec(spec, &p) || p.format != "builtin") return nullptr;
    return makeBuiltin(p.id, p.params, err);
  });
  REQUIRE_OK(eng->setDocument(doc.get()));
  Graph* g = eng->core().graphForControlThread();  // no device: the control thread owns the graph
  REQUIRE(g != nullptr);
  REQUIRE(g->track(0).instrument() != nullptr);
  CHECK(std::fabs(g->track(0).instrument()->parameter(0) - static_cast<float>(v1)) < 1e-4f);
  const uint64_t compiles = eng->compileCount();
  keys->strProps["user.instrument"] = specSetParam(kDefaultInstrument, 0, v2);
  doc->fireChanged();
  CHECK_EQ(eng->compileCount(), compiles);
  CHECK(eng->core().graphForControlThread() == g);
  CHECK(std::fabs(g->track(0).instrument()->parameter(0) - static_cast<float>(v2)) < 1e-4f);
  // the value removed from the spec -> back to the default
  keys->strProps["user.instrument"] = kDefaultInstrument;
  doc->fireChanged();
  CHECK_EQ(eng->compileCount(), compiles);
  CHECK(std::fabs(g->track(0).instrument()->parameter(0) - pi.def) < 1e-4f);
}

namespace {
struct LiveRig {
  sq::app::ClassRegistry reg;
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  sq::Ptr<fakes::MTrack> keys, other;
  sq::Ptr<AudioEngine> eng;
  LiveRig() {
    registerNullDriver(&reg);
    keys = addTrack(*doc, SQ_TRACK_INSTRUMENT, 1, "Keys");
    keys->strProps["user.instrument"] = "test|synth|";
    other = addTrack(*doc, SQ_TRACK_INSTRUMENT, 1, "Other");
    other->strProps["user.instrument"] = "test|synth|";
    NullDriver::defaults()[1].blockFrames = 128;
    eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, nullptr));
    eng->setInsertResolver(testResolver);
    eng->setInstrumentResolver(testResolver);
  }
  ~LiveRig() {
    eng->closeDevice();
    NullDriver::defaults()[1].blockFrames = 256;
  }
  // Peak of the track meter over `blocks` fresh blocks (the meter peak is reset first).
  float peak(const sq_guid& track, uint32_t blocks) {
    sq_meter_value mv[2];
    uint32_t n = 0;
    sq_guid id = track;
    eng->getMeter(&id, mv, 2, &n);
    dev(*eng)->pump(blocks);
    if (SQ_FAILED(eng->getMeter(&id, mv, 2, &n)) || n == 0) return -1.0f;
    return mv[0].peak;
  }
};
}  // namespace

SQ_TEST("instruments: changing the live MIDI target or arm state releases the old track's held notes and sustain pedal") {
  LiveRig rig;
  AudioEngine& eng = *rig.eng;
  REQUIRE_OK(eng.setDocument(rig.doc.get()));
  REQUIRE_OK(eng.openDevice(&kNullDriverClassId, "offline", nullptr));
  const uint8_t on[3] = {0x90, 60, 100}, off[3] = {0x80, 60, 0};
  const uint8_t pedalDown[3] = {0xB0, 64, 127}, pedalUp[3] = {0xB0, 64, 0};
  // key held on Keys, then the selection moves to Other before the key is released
  eng.setLiveMidiTarget(rig.keys->id);
  CHECK(eng.pushLiveMidi(on, 3));
  CHECK(rig.peak(rig.keys->id, 4) > 0.3f);
  eng.setLiveMidiTarget(rig.other->id);
  CHECK(eng.pushLiveMidi(off, 3));
  dev(eng)->pump(2);
  CHECK(rig.peak(rig.keys->id, 4) < 1e-6f);  // not stuck on the old target
  CHECK(rig.peak(rig.other->id, 4) < 1e-6f);

  // sustain pedal down on Keys, key released (sustained), then an arm moves the live MIDI away
  eng.setLiveMidiTarget(rig.keys->id);
  CHECK(eng.pushLiveMidi(pedalDown, 3));
  CHECK(eng.pushLiveMidi(on, 3));
  CHECK(eng.pushLiveMidi(off, 3));
  CHECK(rig.peak(rig.keys->id, 4) > 0.3f);  // sustained
  rig.other->info.flags |= SQ_TRACK_ARMED;
  rig.doc->fireChanged();
  REQUIRE_EQ(eng.liveMidiTracks().size(), size_t(1));
  CHECK(eng.liveMidiTracks()[0] == rig.other->id);
  CHECK(eng.pushLiveMidi(pedalUp, 3));
  dev(eng)->pump(2);
  CHECK(rig.peak(rig.keys->id, 4) < 1e-6f);
}

namespace {
sq_clip_event clipEvent(double pos, double len, uint8_t b0, uint8_t b1, uint8_t b2) {
  sq_clip_event e{};
  e.struct_size = sizeof e;
  e.position = pos;
  e.length = len;
  e.bytes[0] = b0;
  e.bytes[1] = b1;
  e.bytes[2] = b2;
  e.size = 3;
  return e;
}
}  // namespace

SQ_TEST("instruments: stop, locate and loop wrap release a sequenced sustain pedal") {
  LiveRig rig;
  AudioEngine& eng = *rig.eng;
  auto clip = midiClip(0, 8, {});
  clip->events.push_back(clipEvent(0.0, 0.0, 0xB0, 64, 127));
  clip->events.push_back(clipEvent(0.0, 1.0, 0x90, 60, 100));
  clip->events.push_back(clipEvent(6.0, 0.0, 0xB0, 64, 0));
  rig.keys->clips.push_back(clip);
  REQUIRE_OK(eng.setDocument(rig.doc.get()));
  REQUIRE_OK(eng.openDevice(&kNullDriverClassId, "offline", nullptr));
  REQUIRE_OK(eng.play());
  CHECK(rig.peak(rig.keys->id, 40) > 0.3f);  // 0..~0.2 ppq: note + pedal
  REQUIRE_OK(eng.stop());
  dev(eng)->pump(2);
  CHECK(rig.peak(rig.keys->id, 8) < 1e-6f);  // pedal released with the transport stop
  // later live notes are not sustained forever either
  eng.setLiveMidiTarget(rig.keys->id);
  const uint8_t on[3] = {0x90, 62, 100}, off[3] = {0x80, 62, 0};
  CHECK(eng.pushLiveMidi(on, 3));
  CHECK(rig.peak(rig.keys->id, 2) > 0.3f);
  CHECK(eng.pushLiveMidi(off, 3));
  dev(eng)->pump(2);
  CHECK(rig.peak(rig.keys->id, 4) < 1e-6f);
  eng.setLiveMidiTarget(sq_guid{});

  // locate forward while the pedal holds the note: the jump releases the pedal as well
  REQUIRE_OK(eng.locate(0.0));
  REQUIRE_OK(eng.play());
  dev(eng)->pump(40);
  REQUIRE_OK(eng.locate(7.0));  // past the clip's own pedal-up
  dev(eng)->pump(2);
  CHECK(rig.peak(rig.keys->id, 8) < 1e-6f);
  REQUIRE_OK(eng.stop());
}

SQ_TEST("automation: removing an insert parameter lane during playback restores the parameter's static value") {
  sq::app::ClassRegistry reg;
  registerNullDriver(&reg);
  fakes::registerFakeCodec(&reg);
  fakes::putMemFile("mem://rm.tst", 48000.0, std::vector<std::vector<float>>(1, std::vector<float>(96000 * 4, 0.5f)));
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  auto t = addTrack(*doc, SQ_TRACK_AUDIO, 1, "Audio");
  auto c = sq::Ptr<fakes::MClip>::adopt(new fakes::MClip());
  c->info.start = 0;
  c->info.length = 16;
  c->path = "mem://rm.tst";
  t->clips.push_back(c);
  t->strProps["user.inserts"] = "test|probe|";
  t->strProps["automation"] = "insert.0.0 0:0.125 100:0.125\n";
  NullDriver::defaults()[1].blockFrames = 128;
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, nullptr));
  eng->setInsertResolver(testResolver);
  REQUIRE_OK(eng->setDocument(doc.get()));
  REQUIRE_OK(eng->openDevice(&kNullDriverClassId, "offline", nullptr));
  REQUIRE_OK(eng->play());
  dev(*eng)->pump(4);
  CHECK_EQ(g_probeValueMilli.load(), 125);
  const uint64_t compiles = eng->compileCount();
  t->strProps["automation"] = "";  // the lane is deleted while playing
  doc->fireChanged();
  dev(*eng)->pump(2);
  CHECK_EQ(eng->compileCount(), compiles);
  CHECK_EQ(g_probeValueMilli.load(), 1000);
  REQUIRE_OK(eng->stop());
  dev(*eng)->pump(2);
  CHECK_EQ(g_probeValueMilli.load(), 1000);
  eng->closeDevice();
  NullDriver::defaults()[1].blockFrames = 256;
}

SQ_TEST("automation: a parameter without a known static value is not forced to 0 when the automation stops") {
  sq::app::ClassRegistry reg;
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  auto t = addTrack(*doc, SQ_TRACK_INSTRUMENT, 1, "Keys");
  t->strProps["user.instrument"] = "test|synth|";
  t->strProps["user.inserts"] = "test|probe|";  // ProbeFx reports 1 parameter: index 7 is unknown
  t->strProps["automation"] = "insert.0.7 0:0.5 100:0.5\n";
  InsertResolver res = testResolver;
  InstrumentResolver inst = testResolver;
  MediaCache cache;
  GraphDesc d;
  DocBuildResult info;
  REQUIRE_OK(buildGraphDesc(doc.get(), &reg, cache, 48000.0, &d, &info, &res, &inst));
  REQUIRE(d.automation != nullptr);
  REQUIRE_EQ(d.automation->tracks[0].size(), size_t(1));
  CHECK(std::isnan(d.automation->tracks[0][0].staticValue));
  GraphConfig cfg;
  cfg.maxBlock = 64;
  auto g = Graph::compile(d, cfg);
  REQUIRE(g);
  std::vector<float> l(64), r(64);
  float* out[2] = {l.data(), r.data()};
  ProcessContext ctx;
  ctx.sampleRate = 48000.0;
  ctx.frames = 64;
  ctx.playing = true;
  g->process(ctx, out, 2, false, false);
  CHECK_EQ(g_probeLastIndex.load(), 7);
  CHECK_EQ(g_probeLastMilli.load(), 500);
  ctx.playing = false;
  ctx.outputFrame = 64;
  g->process(ctx, out, 2, false, false);
  CHECK_EQ(g_probeLastMilli.load(), 500);  // not reset to 0
  // known static values (spec / prototype) still come back
  t->strProps["automation"] = "insert.0.0 0:0.25 100:0.25\n";
  REQUIRE_OK(buildGraphDesc(doc.get(), &reg, cache, 48000.0, &d, &info, &res, &inst));
  CHECK(std::fabs(d.automation->tracks[0][0].staticValue - 1.0f) < 1e-6f);
}

SQ_TEST("instruments: instrument latency is part of the track latency (PDC)") {
  GraphDesc d;
  TrackDesc inst;
  inst.channels = 1;
  inst.instrument = std::make_shared<CountingSynth>(100u);
  TrackDesc audio;
  audio.channels = 1;
  d.tracks = {inst, audio};
  GraphConfig cfg;
  cfg.maxBlock = 64;
  auto g = Graph::compile(d, cfg);
  REQUIRE(g);
  CHECK_EQ(g->trackArrival(0), 100u);
  CHECK_EQ(g->latency(), 100u);
  CHECK_EQ(g->trackCompensation(1), 100u);
  CHECK_EQ(g->trackCompensation(0), 0u);
}

SQ_TEST("automation: fader and insert lanes follow the audio through the latency in front of them") {
  constexpr uint32_t kLat = 4800;  // 0.2 ppq at 120 bpm / 48 kHz
  GraphDesc d;
  TrackDesc t;
  t.channels = 1;
  t.instrument = std::make_shared<CountingSynth>();
  t.midi = {TimedMidi{24000, {0x90, 60, 100}, 3}};  // held from 1 ppq on: constant 0.5
  t.inserts = {std::make_shared<DelayFx>(kLat), std::make_shared<ProbeFx>(1.0f)};
  t.insertParams = {{}, {}};
  d.tracks = {t};
  auto a = std::make_shared<AutomationData>();
  a->tracks.resize(1);
  AutoLane vol;
  vol.target = AutoTarget::Volume;
  vol.lane.target = "volume";
  vol.lane.points = {{0.0, 0.0}, {3.0, 0.0}, {3.0, -200.0}};  // fader cut at 3 ppq (72000)
  a->tracks[0].push_back(vol);
  AutoLane ins;
  ins.target = AutoTarget::Insert;
  ins.slot = 1;
  ins.param = 0;
  ins.staticValue = 1.0f;
  ins.lane.target = "insert.1.0";
  ins.lane.points = {{0.0, 1.0}, {2.0, 1.0}, {2.0, 0.5}};  // probe gain halves at 2 ppq (48000)
  a->tracks[0].push_back(ins);
  d.automation = a;
  RenderJob job;
  job.start = 0;
  job.end = 96000;
  job.outputChannels = 1;
  job.blockFrames = 256;
  std::vector<std::vector<float>> master;
  REQUIRE_OK(renderToMemory(d, job, &master));
  REQUIRE(master[0].size() >= 90000);
  CHECK(std::fabs(master[0][48000 - 1000] - 0.5f) < 1e-3f);   // before the insert step
  CHECK(std::fabs(master[0][48000 + 1000] - 0.25f) < 1e-3f);  // after it
  CHECK(std::fabs(master[0][72000 - 1000] - 0.25f) < 1e-3f);  // before the fader step
  CHECK(std::fabs(master[0][72000 + 1000]) < 1e-3f);          // after it
}

SQ_TEST("instruments: a note shorter than one frame still ends after it starts") {
  sq::app::ClassRegistry reg;
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  auto keys = addTrack(*doc, SQ_TRACK_INSTRUMENT, 1, "Keys");
  keys->strProps["user.instrument"] = "test|synth|";
  keys->clips.push_back(midiClip(0, 2, {{0.5, 0.00001}}));
  InsertResolver res = testResolver;
  InstrumentResolver inst = testResolver;
  MediaCache cache;
  GraphDesc d;
  DocBuildResult info;
  REQUIRE_OK(buildGraphDesc(doc.get(), &reg, cache, 48000.0, &d, &info, &res, &inst));
  REQUIRE_EQ(d.tracks[0].midi.size(), size_t(2));
  CHECK_EQ(d.tracks[0].midi[0].bytes[0], 0x90);
  CHECK_EQ(d.tracks[0].midi[1].bytes[0], 0x80);
  CHECK(d.tracks[0].midi[1].frame > d.tracks[0].midi[0].frame);
  // the offline render does not leave it hanging
  RenderJob job;
  job.start = 0;
  job.end = 48000;
  job.outputChannels = 1;
  std::vector<std::vector<float>> master;
  REQUIRE_OK(renderToMemory(d, job, &master));
  CHECK(std::fabs(master[0][40000]) < 1e-6f);
}

SQ_TEST("instruments: loop recording closes a note held across the loop wrap at the loop end") {
  sq::app::ClassRegistry reg;
  registerNullDriver(&reg);
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  auto keys = addTrack(*doc, SQ_TRACK_INSTRUMENT, 1, "Keys");
  keys->info.flags |= SQ_TRACK_ARMED;
  NullDriver::defaults()[1].blockFrames = 240;
  sq::app::Settings settings;
  settings.setString("Media.Folder", "mem://media");
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, &settings));
  REQUIRE_OK(eng->setDocument(doc.get()));
  REQUIRE_OK(eng->openDevice(&kNullDriverClassId, "offline", nullptr));
  REQUIRE_OK(eng->setLoop(4.0, 5.0, SQ_TRUE));
  REQUIRE_OK(eng->locate(4.0));
  REQUIRE_OK(eng->play());
  REQUIRE_OK(eng->record(SQ_TRUE));
  dev(*eng)->pump(80);  // 0.8 ppq into the loop (minus one block of output latency)
  const uint8_t on[3] = {0x90, 64, 80}, off[3] = {0x80, 64, 0};
  eng->pushLiveMidi(on, 3);
  dev(*eng)->pump(50);  // wraps after ~0.2 ppq, then ~0.3 ppq into the next pass
  eng->pushLiveMidi(off, 3);
  dev(*eng)->pump(10);
  REQUIRE_OK(eng->record(SQ_FALSE));
  auto& lg = doc->editor->log;
  REQUIRE(lg.size() >= 3);
  REQUIRE(lg[2].action == "events");
  REQUIRE_EQ(lg[2].events.size(), size_t(2));  // the pass-1 part and the pass-2 part
  for (const auto& e : lg[2].events) {
    CHECK_EQ(e.bytes[0], 0x90);
    CHECK(e.length > 0.15);  // never the 1/256 ppq of a note-off paired before its note-on
  }
  CHECK(std::fabs(lg[2].events[0].position - 0.79) < 0.02);
  CHECK(std::fabs(lg[2].events[0].length - 0.21) < 0.02);  // closed at the loop end
  CHECK(std::fabs(lg[2].events[1].position - 0.0) < 0.02);
  CHECK(std::fabs(lg[2].events[1].length - 0.29) < 0.02);  // re-opened at the loop start
  eng->closeDevice();
  NullDriver::defaults()[1].blockFrames = 256;
}

SQ_TEST("instruments: a note held through several loop wraps records every pass (whole passes in between)") {
  sq::app::ClassRegistry reg;
  registerNullDriver(&reg);
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  auto keys = addTrack(*doc, SQ_TRACK_INSTRUMENT, 1, "Keys");
  keys->info.flags |= SQ_TRACK_ARMED;
  NullDriver::defaults()[1].blockFrames = 240;
  sq::app::Settings settings;
  settings.setString("Media.Folder", "mem://media");
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, &settings));
  REQUIRE_OK(eng->setDocument(doc.get()));
  REQUIRE_OK(eng->openDevice(&kNullDriverClassId, "offline", nullptr));
  REQUIRE_OK(eng->setLoop(4.0, 5.0, SQ_TRUE));
  REQUIRE_OK(eng->locate(4.0));
  REQUIRE_OK(eng->play());
  REQUIRE_OK(eng->record(SQ_TRUE));
  dev(*eng)->pump(80);  // 0.8 ppq into the loop
  const uint8_t on[3] = {0x90, 64, 80}, off[3] = {0x80, 64, 0};
  eng->pushLiveMidi(on, 3);
  dev(*eng)->pump(150);  // wraps twice (no live event in between), then ~0.3 ppq into pass 3
  eng->pushLiveMidi(off, 3);
  dev(*eng)->pump(10);
  REQUIRE_OK(eng->record(SQ_FALSE));
  auto& lg = doc->editor->log;
  REQUIRE(lg.size() >= 3);
  REQUIRE(lg[2].action == "events");
  REQUIRE_EQ(lg[2].events.size(), size_t(3));  // pass 1 tail, the whole pass 2, pass 3 head
  CHECK(std::fabs(lg[2].events[0].position - 0.79) < 0.02);
  CHECK(std::fabs(lg[2].events[0].length - 0.21) < 0.02);
  CHECK(std::fabs(lg[2].events[1].position - 0.0) < 0.02);
  CHECK(std::fabs(lg[2].events[1].length - 1.0) < 0.02);
  CHECK(std::fabs(lg[2].events[2].position - 0.0) < 0.02);
  CHECK(std::fabs(lg[2].events[2].length - 0.29) < 0.02);
  eng->closeDevice();
  NullDriver::defaults()[1].blockFrames = 256;
}

SQ_TEST("instruments: recorded MIDI is placed where the player heard the transport (output latency)") {
  sq::app::ClassRegistry reg;
  registerNullDriver(&reg);
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  auto keys = addTrack(*doc, SQ_TRACK_INSTRUMENT, 1, "Keys");
  keys->info.flags |= SQ_TRACK_ARMED;
  NullDriver::defaults()[1].blockFrames = 240;
  NullDriver::defaults()[1].outputLatency = 4560;  // + one block = 4800 frames = 0.2 ppq
  sq::app::Settings settings;
  settings.setString("Media.Folder", "mem://media");
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, &settings));
  REQUIRE_OK(eng->setDocument(doc.get()));
  REQUIRE_OK(eng->openDevice(&kNullDriverClassId, "offline", nullptr));
  REQUIRE_OK(eng->locate(4.0));
  REQUIRE_OK(eng->play());
  REQUIRE_OK(eng->record(SQ_TRUE));
  dev(*eng)->pump(100);  // the transport is 1 ppq in; the player hears 0.8 ppq
  const uint8_t on[3] = {0x90, 64, 80}, off[3] = {0x80, 64, 0};
  eng->pushLiveMidi(on, 3);
  dev(*eng)->pump(50);
  eng->pushLiveMidi(off, 3);
  dev(*eng)->pump(50);
  REQUIRE_OK(eng->record(SQ_FALSE));
  auto& lg = doc->editor->log;
  REQUIRE(lg.size() >= 3);
  REQUIRE(lg[2].action == "events");
  REQUIRE_EQ(lg[2].events.size(), size_t(1));
  CHECK(std::fabs(lg[2].events[0].position - 0.8) < 0.02);
  CHECK(std::fabs(lg[2].events[0].length - 0.5) < 0.02);
  eng->closeDevice();
  NullDriver::defaults()[1].blockFrames = 256;
  NullDriver::defaults()[1].outputLatency = 0;
}
