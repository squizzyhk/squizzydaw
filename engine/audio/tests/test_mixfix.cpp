// engine/audio - sample-rate conversion of media, master mute / pan, bus solo (solo-in-place
// through sends) and the song tempo in ProcessContext (synced delay).
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "fakes.hpp"
#include "sq/app/settings.hpp"
#include "sq/engine/audio/builtin.hpp"
#include "sq/engine/audio/devices.hpp"
#include "sq/engine/audio/doc_builder.hpp"
#include "sq/engine/audio/engine.hpp"
#include "sq/engine/audio/render.hpp"
#include "sq/engine/audio/resample.hpp"
#include "sqtest.hpp"

using namespace sq::engine::audio;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double freq, double rate, int64_t frames, double amp = 0.5) {
  std::vector<float> v(static_cast<size_t>(frames));
  for (int64_t i = 0; i < frames; ++i) v[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) / rate));
  return v;
}

// Max |x[i] - ideal(i)| over [from, to), ideal = amp sin(2 pi f i / rate).
double maxErr(const std::vector<float>& x, double freq, double rate, size_t from, size_t to, double amp = 0.5) {
  double e = 0.0;
  for (size_t i = from; i < to && i < x.size(); ++i)
    e = std::max(e, std::fabs(static_cast<double>(x[i]) - amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) / rate)));
  return e;
}

double rms(const std::vector<float>& x, size_t from, size_t to) {
  double s = 0.0;
  size_t n = 0;
  for (size_t i = from; i < to && i < x.size(); ++i, ++n) s += static_cast<double>(x[i]) * x[i];
  return n ? std::sqrt(s / static_cast<double>(n)) : 0.0;
}

// Frequency from rising zero crossings over [from, to).
double zcFreq(const std::vector<float>& x, double rate, size_t from, size_t to) {
  double first = -1.0, last = -1.0;
  int count = 0;
  for (size_t i = from + 1; i < to && i < x.size(); ++i) {
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
      const double t = static_cast<double>(i - 1) + static_cast<double>(-x[i - 1]) / static_cast<double>(x[i] - x[i - 1]);
      if (first < 0.0) first = t;
      else ++count;
      last = t;
    }
  }
  return count > 0 ? rate * count / (last - first) : 0.0;
}

MediaPtr makeMedia(uint32_t channels, int64_t frames, float (*fn)(int64_t, uint32_t)) {
  auto m = std::make_shared<MediaBuffer>();
  m->channels = channels;
  m->frames = frames;
  m->data.resize(channels);
  for (uint32_t c = 0; c < channels; ++c) {
    m->data[c].resize(static_cast<size_t>(frames));
    for (int64_t i = 0; i < frames; ++i) m->data[c][static_cast<size_t>(i)] = fn(i, c);
  }
  return m;
}

SourceProto clipOf(MediaPtr m, int64_t start, int64_t length) {
  ClipParams p;
  p.start = start;
  p.length = length;
  return std::make_shared<ClipNode>(std::make_shared<MediaNode>(std::move(m)), p);
}

std::vector<std::vector<float>> runGraph(Graph& g, int64_t from, int64_t frames, uint32_t block) {
  std::vector<std::vector<float>> out(g.outputChannels());
  std::vector<float> buf(static_cast<size_t>(g.outputChannels()) * block);
  std::vector<float*> ptr(g.outputChannels());
  for (uint32_t c = 0; c < g.outputChannels(); ++c) ptr[c] = &buf[static_cast<size_t>(c) * block];
  for (int64_t done = 0; done < frames;) {
    const uint32_t n = static_cast<uint32_t>(std::min<int64_t>(block, frames - done));
    ProcessContext ctx;
    ctx.frames = n;
    ctx.outputFrame = from + done;
    ctx.playing = true;
    g.process(ctx, ptr.data(), g.outputChannels(), false, false);
    for (uint32_t c = 0; c < g.outputChannels(); ++c) out[c].insert(out[c].end(), ptr[c], ptr[c] + n);
    done += n;
  }
  return out;
}

sq::Ptr<fakes::MTrack> addTrack(fakes::MDoc& doc, int32_t kind, uint32_t channels, const char* name) {
  auto t = sq::Ptr<fakes::MTrack>::adopt(new fakes::MTrack());
  t->name = name;
  t->info.kind = kind;
  t->info.channels = channels;
  doc.tracks.push_back(t);
  return t;
}

sq::Ptr<fakes::MClip> audioClip(const char* path, double start, double length, double offsetSeconds = 0.0) {
  auto c = sq::Ptr<fakes::MClip>::adopt(new fakes::MClip());
  c->info.start = start;
  c->info.length = length;
  c->info.source_offset_seconds = offsetSeconds;
  c->path = path;
  return c;
}

double peakOf(const std::vector<float>& x, size_t from, size_t to) {
  double p = 0.0;
  for (size_t i = from; i < to && i < x.size(); ++i) p = std::max(p, std::fabs(static_cast<double>(x[i])));
  return p;
}

}  // namespace

// ---------------------------------------------------------------- sample-rate conversion

SQ_TEST("resample: 44.1 kHz -> 48 kHz keeps pitch and length, error below -90 dB") {
  const auto in = sine(440.0, 44100.0, 44100);
  const auto out = resampleChannel(in.data(), static_cast<int64_t>(in.size()), 44100.0, 48000.0);
  CHECK_EQ(out.size(), size_t(48000));
  CHECK(std::fabs(zcFreq(out, 48000.0, 1000, 47000) - 440.0) < 0.01);
  // away from the edges (the source is cut there) the result is the ideal sine
  CHECK(maxErr(out, 440.0, 48000.0, 2000, 46000) < 0.5 * 3.2e-5);  // < -90 dBFS re the amplitude
  // a 18 kHz tone (inside the passband) too
  const auto hi = sine(18000.0, 44100.0, 44100);
  const auto hio = resampleChannel(hi.data(), static_cast<int64_t>(hi.size()), 44100.0, 48000.0);
  CHECK(maxErr(hio, 18000.0, 48000.0, 2000, 46000) < 0.5 * 3.2e-5);
}

SQ_TEST("resample: 22.05 kHz -> 48 kHz (no images) and 48 kHz -> 44.1 kHz (no aliasing)") {
  const auto in = sine(9000.0, 22050.0, 22050);
  const auto out = resampleChannel(in.data(), static_cast<int64_t>(in.size()), 22050.0, 48000.0);
  CHECK_EQ(out.size(), size_t(48000));
  CHECK(maxErr(out, 9000.0, 48000.0, 4000, 44000) < 0.5 * 3.2e-5);  // images would show as error
  // 23 kHz at 48 kHz is above the 44.1 kHz Nyquist: must be removed (> 90 dB down)
  const auto alias = sine(23000.0, 48000.0, 48000);
  const auto ao = resampleChannel(alias.data(), static_cast<int64_t>(alias.size()), 48000.0, 44100.0);
  CHECK_EQ(ao.size(), size_t(44100));
  CHECK(rms(ao, 4000, 40000) < 0.5 * 0.7071 * 3.2e-5);
  // a 1 kHz tone passes unchanged
  const auto lo = sine(1000.0, 48000.0, 48000);
  const auto loo = resampleChannel(lo.data(), static_cast<int64_t>(lo.size()), 48000.0, 44100.0);
  CHECK(maxErr(loo, 1000.0, 44100.0, 4000, 40000) < 0.5 * 3.2e-5);
}

SQ_TEST("resample: non-integer rate ratio (table kernel) is accurate too") {
  const auto in = sine(1000.0, 44100.0, 44100);
  const double outRate = 47999.25;
  const auto out = resampleChannel(in.data(), static_cast<int64_t>(in.size()), 44100.0, outRate);
  CHECK_EQ(out.size(), static_cast<size_t>(std::llround(44100.0 * outRate / 44100.0)));
  CHECK(maxErr(out, 1000.0, outRate, 4000, 44000) < 0.5 * 3.2e-5);
  // identity
  const auto same = resampleChannel(in.data(), static_cast<int64_t>(in.size()), 44100.0, 44100.0);
  CHECK(same == in);
}

SQ_TEST("resample: media cache converts once per (path, rate); a 44.1 kHz clip plays at pitch and full length") {
  sq::app::ClassRegistry reg;
  fakes::registerFakeCodec(&reg);
  fakes::putMemFile("mem://sine441.tst", 44100.0, std::vector<std::vector<float>>(2, sine(440.0, 44100.0, 44100 * 2)));
  MediaCache cache;
  MediaPtr a, b, native;
  REQUIRE_OK(cache.get(&reg, "mem://sine441.tst", 48000.0, &a));
  REQUIRE_OK(cache.get(&reg, "mem://sine441.tst", 48000.0, &b));
  REQUIRE_OK(cache.get(&reg, "mem://sine441.tst", &native));
  CHECK(a.get() == b.get());
  CHECK_EQ(a->sampleRate, 48000.0);
  CHECK_EQ(a->frames, int64_t(96000));
  CHECK_EQ(a->channels, 2u);
  CHECK_EQ(native->sampleRate, 44100.0);
  MediaPtr same;
  REQUIRE_OK(cache.get(&reg, "mem://sine441.tst", 44100.0, &same));
  CHECK(same.get() == native.get());

  // document at 48 kHz, 120 bpm: clip 0..8 ppq = 4 s holding the 2 s file; a second clip starts
  // 0.5 s into the source
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  auto t = addTrack(*doc, SQ_TRACK_AUDIO, 2, "Sine");
  t->clips.push_back(audioClip("mem://sine441.tst", 0, 8));
  auto t2 = addTrack(*doc, SQ_TRACK_AUDIO, 2, "Offset");
  t2->info.flags |= SQ_TRACK_MUTE;
  t2->clips.push_back(audioClip("mem://sine441.tst", 0, 2, 0.5));
  GraphDesc d;
  DocBuildResult info;
  REQUIRE_OK(buildGraphDesc(doc.get(), &reg, cache, 48000.0, &d, &info));
  RenderJob job;
  job.end = 192000;
  std::vector<std::vector<float>> m;
  REQUIRE_OK(renderToMemory(d, job, &m));
  // stereo track at centre: the balance law keeps unity gain
  CHECK(std::fabs(zcFreq(m[0], 48000.0, 1000, 95000) - 440.0) < 0.05);  // not 479 Hz
  CHECK(peakOf(m[0], 93000, 95500) > 0.4);                           // still sounding near 2 s
  CHECK(peakOf(m[0], 96500, 190000) < 1e-3);                          // the file ends at 2 s, not 1.84 s
  CHECK(maxErr(m[0], 440.0, 48000.0, 2000, 94000) < 1e-4);

  // source offset in source seconds: unmute the offset track, mute the first
  t->info.flags |= SQ_TRACK_MUTE;
  t2->info.flags &= ~SQ_TRACK_MUTE;
  GraphDesc d2;
  REQUIRE_OK(buildGraphDesc(doc.get(), &reg, cache, 48000.0, &d2, &info));
  REQUIRE_OK(renderToMemory(d2, job, &m));
  double e = 0.0;  // output i = source time 0.5 s + i / 48000
  for (size_t i = 2000; i < 40000; ++i)
    e = std::max(e, std::fabs(m[0][i] - 0.5 * std::sin(2.0 * kPi * 440.0 * (0.5 + static_cast<double>(i) / 48000.0))));
  CHECK(e < 1e-4);
}

// ---------------------------------------------------------------- master mute / pan

SQ_TEST("master: mute and pan of the master strip apply offline and through RT commands") {
  sq::app::ClassRegistry reg;
  fakes::registerFakeCodec(&reg);
  fakes::putMemFile("mem://dc.tst", 48000.0, std::vector<std::vector<float>>(1, std::vector<float>(96000 * 2, 0.5f)));
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  auto master = addTrack(*doc, SQ_TRACK_MASTER, 2, "Master");
  auto t = addTrack(*doc, SQ_TRACK_AUDIO, 1, "Audio");
  t->info.pan = 0.0;
  t->clips.push_back(audioClip("mem://dc.tst", 0, 8));
  MediaCache cache;
  RenderJob job;
  job.end = 48000;
  std::vector<std::vector<float>> m;
  auto render = [&] {
    GraphDesc d;
    REQUIRE_OK(buildGraphDesc(doc.get(), &reg, cache, 48000.0, &d));
    REQUIRE_OK(renderToMemory(d, job, &m));
    return d;
  };
  render();
  const float centre = m[0][1000];
  CHECK(centre > 0.3f);
  CHECK(std::fabs(m[1][1000] - centre) < 1e-6f);

  master->info.flags |= SQ_TRACK_MUTE;
  GraphDesc d = render();
  CHECK(d.masterMute);
  CHECK(peakOf(m[0], 0, 48000) < 1e-9);
  CHECK(peakOf(m[1], 0, 48000) < 1e-9);

  master->info.flags &= ~SQ_TRACK_MUTE;
  master->info.pan = 0.5;  // R50: left at 0.5 (balance law), right untouched
  d = render();
  CHECK(std::fabs(d.masterPan - 0.5f) < 1e-6f);
  CHECK(std::fabs(m[0][1000] - 0.5f * centre) < 1e-5f);
  CHECK(std::fabs(m[1][1000] - centre) < 1e-5f);

  // live graph: RT setters (what the MasterPan / MasterMute commands call)
  master->info.pan = 0.0;
  GraphDesc ld;
  REQUIRE_OK(buildGraphDesc(doc.get(), &reg, cache, 48000.0, &ld));
  GraphConfig cfg;
  cfg.maxBlock = 256;
  auto g = Graph::compile(ld, cfg);
  REQUIRE(g);
  auto out = runGraph(*g, 0, 1024, 256);
  CHECK(std::fabs(out[0][900] - centre) < 1e-5f);
  g->setMasterPan(-1.0f);
  out = runGraph(*g, 1024, 1024, 256);
  CHECK(std::fabs(out[1][900]) < 1e-6f);
  CHECK(std::fabs(out[0][900] - centre) < 1e-5f);
  g->setMasterMute(true);
  out = runGraph(*g, 2048, 1024, 256);
  CHECK(peakOf(out[0], 300, 1024) < 1e-9);
}

SQ_TEST("master: master mute / pan document edits reach the live engine without a recompile") {
  sq::app::ClassRegistry reg;
  fakes::registerFakeCodec(&reg);
  registerNullDriver(&reg);
  fakes::putMemFile("mem://live.tst", 48000.0, std::vector<std::vector<float>>(1, std::vector<float>(96000 * 4, 0.25f)));
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  auto master = addTrack(*doc, SQ_TRACK_MASTER, 2, "Master");
  auto t = addTrack(*doc, SQ_TRACK_AUDIO, 1, "Audio");
  t->clips.push_back(audioClip("mem://live.tst", 0, 16));
  auto bus = addTrack(*doc, SQ_TRACK_BUS, 2, "Bus");
  NullDriver::defaults()[1].blockFrames = 128;
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, nullptr));
  REQUIRE_OK(eng->setDocument(doc.get()));
  REQUIRE_OK(eng->openDevice(&kNullDriverClassId, "offline", nullptr));
  REQUIRE_OK(eng->play());
  auto* dev = static_cast<NullDevice*>(eng->device());
  dev->pump(4);
  const uint64_t compiles = eng->compileCount();
  sq_meter_value mv[2];
  uint32_t n = 0;
  auto masterPeak = [&](uint32_t ch) {
    dev->pump(10);
    REQUIRE_OK(eng->getMeter(nullptr, mv, 2, &n));  // resets the peak hold
    dev->pump(10);
    REQUIRE_OK(eng->getMeter(nullptr, mv, 2, &n));
    return ch < n ? mv[ch].peak : -1.0f;
  };
  CHECK(masterPeak(0) > 0.1f);

  master->info.flags |= SQ_TRACK_MUTE;
  doc->fireChanged();
  CHECK_EQ(eng->compileCount(), compiles);
  CHECK(masterPeak(0) < 1e-6f);
  CHECK(masterPeak(1) < 1e-6f);

  master->info.flags &= ~SQ_TRACK_MUTE;
  master->info.pan = 1.0;  // hard right: the left channel goes silent
  doc->fireChanged();
  CHECK_EQ(eng->compileCount(), compiles);
  CHECK(masterPeak(0) < 1e-6f);
  CHECK(masterPeak(1) > 0.1f);

  // bus solo: the audio track goes straight to the master and does not feed the bus -> silent
  master->info.pan = 0.0;
  bus->info.flags |= SQ_TRACK_SOLO;
  doc->fireChanged();
  CHECK_EQ(eng->compileCount(), compiles);
  CHECK(masterPeak(0) < 1e-6f);
  // the track routed into the soloed bus is heard again (structural edit: recompile)
  t->info.output = bus->id;
  doc->fireChanged();
  CHECK(masterPeak(0) > 0.1f);
  bus->info.flags &= ~SQ_TRACK_SOLO;
  doc->fireChanged();
  CHECK(masterPeak(0) > 0.1f);
  eng->closeDevice();
  NullDriver::defaults()[1].blockFrames = 256;
}

// ---------------------------------------------------------------- bus solo

SQ_TEST("solo: a soloed bus keeps itself and what feeds it, through outputs and sends; others are silenced") {
  auto one = makeMedia(1, 100000, [](int64_t, uint32_t) { return 1.0f; });
  // track 0 -> master (dry) + send to bus 1 (FX return); track 1 -> bus 0 (group); track 2 -> master
  GraphDesc d;
  d.buses.resize(2);
  TrackDesc t;
  t.channels = 1;
  t.pan = -1.0f;  // everything hard left: out[0] carries the sum
  t.sources.push_back(clipOf(one, 0, 100000));
  t.output = -1;
  t.volume = 0.5f;
  t.sends.push_back(SendDesc{1, 0.125f, false});
  d.tracks.push_back(t);  // 0: dry 0.5, wet 0.5 * 0.125 * (send pan law: mono to stereo, hard left)
  t.sends.clear();
  t.output = 0;
  t.volume = 0.25f;
  d.tracks.push_back(t);  // 1: via group 0.25
  t.output = -1;
  t.volume = 0.0625f;
  d.tracks.push_back(t);  // 2: dry 0.0625
  GraphConfig cfg;
  cfg.maxBlock = 128;
  auto g = Graph::compile(d, cfg);
  REQUIRE(g);
  auto level = [&](int64_t at) { return runGraph(*g, at, 512, 128)[0][400]; };
  const float all = level(0);
  // send: post fader (0.5) * 0.125, mono source hard left into a stereo bus -> 0.0625 at left
  CHECK(std::fabs(all - (0.5f + 0.0625f + 0.25f + 0.0625f)) < 1e-5f);

  // solo the FX return: only the wet signal of track 0 (its dry output is silenced)
  g->bus(1).setSolo(true);
  g->updateSolo();
  CHECK(std::fabs(level(1000) - 0.0625f) < 1e-5f);
  // solo the group too: + track 1 through the group
  g->bus(0).setSolo(true);
  g->updateSolo();
  CHECK(std::fabs(level(2000) - (0.0625f + 0.25f)) < 1e-5f);
  // solo track 0 only: its dry and its FX return (solo in place)
  g->bus(0).setSolo(false);
  g->bus(1).setSolo(false);
  g->track(0).setSolo(true);
  g->updateSolo();
  CHECK(std::fabs(level(3000) - (0.5f + 0.0625f)) < 1e-5f);
  // track 2 soloed: only its dry (the FX return gets nothing from un-soloed track 0)
  g->track(0).setSolo(false);
  g->track(2).setSolo(true);
  g->updateSolo();
  CHECK(std::fabs(level(4000) - 0.0625f) < 1e-5f);
  // nothing soloed: everything again
  g->track(2).setSolo(false);
  g->updateSolo();
  CHECK(std::fabs(level(5000) - all) < 1e-5f);
}

SQ_TEST("solo: nested buses - soloing an inner bus keeps the outer bus it reaches the master through") {
  auto one = makeMedia(1, 100000, [](int64_t, uint32_t) { return 1.0f; });
  GraphDesc d;
  d.buses.resize(2);
  d.buses[0].output = 1;  // inner -> outer -> master
  TrackDesc t;
  t.channels = 1;
  t.pan = -1.0f;
  t.sources.push_back(clipOf(one, 0, 100000));
  t.output = 0;
  t.volume = 0.5f;
  d.tracks.push_back(t);  // into the inner bus
  t.output = 1;
  t.volume = 0.25f;
  d.tracks.push_back(t);  // straight into the outer bus
  GraphConfig cfg;
  cfg.maxBlock = 128;
  auto g = Graph::compile(d, cfg);
  REQUIRE(g);
  g->bus(0).setSolo(true);
  g->updateSolo();
  CHECK(std::fabs(runGraph(*g, 0, 512, 128)[0][400] - 0.5f) < 1e-5f);
  g->bus(0).setSolo(false);
  g->bus(1).setSolo(true);
  g->updateSolo();
  CHECK(std::fabs(runGraph(*g, 512, 512, 128)[0][400] - 0.75f) < 1e-5f);
}

SQ_TEST("solo: the document's bus solo flag reaches the graph description") {
  sq::app::ClassRegistry reg;
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  auto bus = addTrack(*doc, SQ_TRACK_BUS, 2, "Bus");
  auto fx = addTrack(*doc, SQ_TRACK_FX, 2, "FX");
  bus->info.flags |= SQ_TRACK_SOLO;
  MediaCache cache;
  GraphDesc d;
  REQUIRE_OK(buildGraphDesc(doc.get(), &reg, cache, 48000.0, &d));
  REQUIRE(d.buses.size() == 2);
  CHECK(d.buses[0].solo);
  CHECK(!d.buses[1].solo);
  (void)fx;
}

// ---------------------------------------------------------------- tempo in ProcessContext

namespace {
double g_seenTempo = -1.0;
class TempoProbe final : public FxProcessor {
 public:
  ProcessorPtr clone() const override { return std::make_unique<TempoProbe>(); }
  bool prepare(double, uint32_t, uint32_t) override { return true; }
  void process(const ProcessContext& ctx, float* const*, uint32_t) noexcept override { g_seenTempo = ctx.tempo; }
};

size_t argmaxAbs(const std::vector<float>& x, size_t from) {
  size_t k = from;
  for (size_t i = from; i < x.size(); ++i)
    if (std::fabs(x[i]) > std::fabs(x[k])) k = i;
  return k;
}
}  // namespace

SQ_TEST("tempo: the graph passes the song tempo at the block start to processors") {
  GraphDesc d;
  d.tempo = {TempoSegment{0, 0.0, 90.0}, TempoSegment{48000, 1.5, 140.0}};
  TrackDesc t;
  t.inserts.push_back(std::make_shared<TempoProbe>());
  d.tracks.push_back(t);
  GraphConfig cfg;
  cfg.maxBlock = 256;
  auto g = Graph::compile(d, cfg);
  REQUIRE(g);
  runGraph(*g, 0, 256, 256);
  CHECK_EQ(g_seenTempo, 90.0);
  runGraph(*g, 48000, 256, 256);
  CHECK_EQ(g_seenTempo, 140.0);
}

SQ_TEST("tempo: a synced delay follows the song tempo (offline), its own Tempo knob is only the fallback") {
  auto impulse = makeMedia(2, 8, [](int64_t i, uint32_t) { return i == 0 ? 1.0f : 0.0f; });
  // delay: stereo mode, sync 1/4 (index 9), feedback 0, mix 100 %
  std::string err;
  ProcessorProto delay = makeBuiltin("delay", {{0, 0.0}, {1, 9.0}, {5, 0.0}, {9, 100.0}}, &err);
  REQUIRE(delay != nullptr);
  auto echoAt = [&](double bpm) {
    GraphDesc d;
    d.tempo = {TempoSegment{0, 0.0, bpm}};
    TrackDesc t;
    t.sources.push_back(clipOf(impulse, 0, 8));
    t.inserts.push_back(delay);
    d.tracks.push_back(t);
    RenderJob job;
    job.end = 60000;
    std::vector<std::vector<float>> m;
    REQUIRE_OK(renderToMemory(d, job, &m));
    return argmaxAbs(m[0], 100);
  };
  const size_t e90 = echoAt(90.0);
  CHECK(e90 >= 31998 && e90 <= 32002);  // a quarter at 90 bpm = 0.667 s, not 0.5 s
  const size_t e120 = echoAt(120.0);
  CHECK(e120 >= 23998 && e120 <= 24002);

  // outside a graph (no tempo in the context): the device's Tempo parameter (here 60 bpm)
  ProcessorProto own = makeBuiltin("delay", {{0, 0.0}, {1, 9.0}, {4, 60.0}, {5, 0.0}, {9, 100.0}}, &err);
  REQUIRE(own != nullptr);
  ProcessorPtr p = own->clone();
  REQUIRE(p->prepare(48000.0, 512, 1));
  std::vector<float> buf(512), all;
  for (int b = 0; b < 120; ++b) {
    std::fill(buf.begin(), buf.end(), 0.0f);
    if (b == 0) buf[0] = 1.0f;
    float* io[1] = {buf.data()};
    ProcessContext ctx;
    ctx.frames = 512;
    ctx.playing = true;
    p->process(ctx, io, 1);
    all.insert(all.end(), buf.begin(), buf.end());
  }
  const size_t e = argmaxAbs(all, 100);
  CHECK(e >= 47998 && e <= 48002);
}
