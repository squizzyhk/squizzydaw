// engine/audio - exit test: no heap allocation on the realtime thread.
// The test binary replaces global operator new/delete (and, with the MSVC debug CRT, hooks
// malloc/realloc/free) and counts every call made while rt::inRealtimeContext() is true. It
// then drives the engine through its full feature set on the NullDevice's AudioRenderThread and
// through offline renders and requires the count to stay zero.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "fakes.hpp"
#include "sq/app/class_registry.hpp"
#include "sq/app/settings.hpp"
#include "sq/engine/audio/devices.hpp"
#include "sq/engine/audio/engine.hpp"
#include "sq/engine/audio/render.hpp"
#include "sq/platform.h"
#include "sqtest.hpp"

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {
std::atomic<uint64_t> g_rtAllocs{0};
std::atomic<bool> g_armed{false};
inline void noteAlloc() noexcept {
  if (g_armed.load(std::memory_order_relaxed) && sq::engine::audio::rt::inRealtimeContext())
    g_rtAllocs.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

void* operator new(std::size_t n) {
  noteAlloc();
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
  noteAlloc();
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  noteAlloc();
  return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  noteAlloc();
  return std::malloc(n ? n : 1);
}
void operator delete(void* p) noexcept {
  if (p) noteAlloc();
  std::free(p);
}
void operator delete[](void* p) noexcept {
  if (p) noteAlloc();
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  if (p) noteAlloc();
  std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  if (p) noteAlloc();
  std::free(p);
}

#if defined(_MSC_VER) && defined(_DEBUG)
static int crtHook(int allocType, void*, size_t, int blockType, long, const unsigned char*, int) {
  if (blockType != _CRT_BLOCK && (allocType == _HOOK_ALLOC || allocType == _HOOK_REALLOC || allocType == _HOOK_FREE)) noteAlloc();
  return 1;
}
#endif

using namespace sq::engine::audio;

namespace {

MediaPtr media(uint32_t ch, int64_t frames) {
  auto m = std::make_shared<MediaBuffer>();
  m->channels = ch;
  m->frames = frames;
  m->data.assign(ch, std::vector<float>(static_cast<size_t>(frames)));
  for (uint32_t c = 0; c < ch; ++c)
    for (int64_t i = 0; i < frames; ++i) m->data[c][static_cast<size_t>(i)] = static_cast<float>((i * 7 + c) % 101) / 101.0f - 0.5f;
  return m;
}

GraphDesc richScene(int variant) {
  GraphDesc d;
  d.tempo.push_back(TempoSegment{0, 0.0, 120.0});
  d.tempo.push_back(TempoSegment{96000, 4.0, 140.0});
  BusDesc bus;
  bus.id = fakes::newGuid();
  bus.inserts.push_back(std::make_shared<DelayFx>(37 + variant));
  d.buses.push_back(bus);
  // audio track: clip + modification + latent parallel insert, into the bus
  TrackDesc a;
  a.id = fakes::newGuid();
  a.name = "a";
  a.channels = 2;
  ClipParams p;
  p.start = 100;
  p.length = 90000;
  p.fadeIn = 500;
  p.fadeOut = 500;
  p.gain = 0.7f;
  auto clip = std::make_shared<ClipNode>(std::make_shared<FileNode>(media(2, 100000)), p);
  a.sources.push_back(std::make_shared<ModificationNode>(clip, std::vector<GainPoint>{{0, 0.0f}, {48000, 1.0f}}));
  auto par = std::make_shared<ParallelProcessor>();
  par->addBranch({std::make_shared<DelayFx>(64)});
  par->addBranch({std::make_shared<GainFx>(0.5f)});
  a.inserts.push_back(par);
  a.output = 0;
  a.inputChannels = {0, 1};
  a.monitor = true;
  a.armed = true;
  d.tracks.push_back(a);
  // instrument track: event node + sine synth
  TrackDesc s;
  s.id = fakes::newGuid();
  s.name = "s";
  s.channels = 1;
  std::vector<TimedMidi> ev;
  for (int k = 0; k < 64; ++k) {
    ev.push_back(TimedMidi{k * 3000, {0x90, static_cast<uint8_t>(48 + k % 24), 100}, 3});
    ev.push_back(TimedMidi{k * 3000 + 2500, {0x80, static_cast<uint8_t>(48 + k % 24), 0}, 3});
  }
  s.sources.push_back(std::make_shared<EventNode>(ev, std::make_shared<SineSynth>()));
  s.solo = variant % 2 == 1;
  d.tracks.push_back(s);
  // input-only track
  TrackDesc in;
  in.id = fakes::newGuid();
  in.name = "in";
  in.channels = 1;
  in.sources.push_back(std::make_shared<InputNode>(std::vector<uint32_t>{1}));
  in.inputChannels = {1};
  in.armed = true;
  d.tracks.push_back(in);
  return d;
}

}  // namespace

SQ_TEST("rtsafety: the allocation hook detects RT allocations (sanity)") {
  g_rtAllocs.store(0);
  g_armed.store(true);
  {
    rt::Scope s;
    auto* v = new std::vector<int>(10);
    delete v;
  }
  {
    std::vector<int> outside(10);  // not RT: not counted
  }
  g_armed.store(false);
  CHECK(g_rtAllocs.load() >= 2);
}

SQ_TEST("rtsafety: no allocation on the audio thread across engine features") {
#if defined(_MSC_VER) && defined(_DEBUG)
  _CrtSetAllocHook(&crtHook);
#endif
  sq::app::ClassRegistry reg;
  fakes::registerFakeCodec(&reg);
  sq::app::Settings settings;
  settings.setString("Media.Folder", "mem://rt");
  settings.setString("Media.RecordFormat", "tst");
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, &settings));
  REQUIRE_OK(eng->setGraphDesc(richScene(0)));
  NullDeviceOptions o;
  o.blockFrames = 128;
  o.speed = 8.0;
  o.inputGenerator = [](int64_t pos, float* const* ch, uint32_t nch, uint32_t n) {
    for (uint32_t c = 0; c < nch; ++c)
      for (uint32_t i = 0; i < n; ++i) ch[c][i] = static_cast<float>((pos + i + c) % 50) / 100.0f;
  };
  sq::Ptr<NullDevice> dev = sq::Ptr<NullDevice>::adopt(new NullDevice(o));
  REQUIRE_OK(eng->openDeviceObject(dev.get(), nullptr));

  g_rtAllocs.store(0);
  g_armed.store(true);
  const sq_guid trackA = eng->graphDesc().tracks[0].id;
  REQUIRE_OK(eng->setMetronome(SQ_TRUE));
  REQUIRE_OK(eng->setLoop(0.5, 3.0, SQ_TRUE));
  REQUIRE_OK(eng->play());
  REQUIRE_OK(eng->record(SQ_TRUE));
  for (int i = 0; i < 60; ++i) {
    sq_thread_sleep_ms(10);
    switch (i % 6) {
      case 0: eng->setTrackVolume(trackA, 0.1f * static_cast<float>(i % 10)); break;
      case 1: eng->setTrackPan(trackA, (i % 3) - 1.0f); break;
      case 2: eng->setLowLatencyMonitoring((i / 6) % 2 ? SQ_TRUE : SQ_FALSE); break;
      case 3: eng->setTrackSolo(trackA, (i / 6) % 2 == 0); break;
      case 4: REQUIRE_OK(eng->setGraphDesc(richScene(i))); break;  // lock-free graph swap
      default: eng->locate(static_cast<double>(i % 4)); break;
    }
    sq_meter_value mv[8];
    uint32_t cnt = 0;
    eng->getMeter(nullptr, mv, 8, &cnt);
    sq_transport_state ts{};
    eng->getTransportState(&ts);
    eng->idle();
  }
  REQUIRE_OK(eng->record(SQ_FALSE));
  REQUIRE_OK(eng->stop());
  eng->closeDevice();
  g_armed.store(false);
  const uint64_t live = g_rtAllocs.load();
  std::printf("rtsafety: live RT allocations %llu, callbacks %llu\n", static_cast<unsigned long long>(live),
              static_cast<unsigned long long>(dev->callbacks()));
  CHECK_EQ(live, uint64_t(0));
  CHECK(dev->callbacks() > 100);

  // offline renders run their blocks under the same RT rules
  g_rtAllocs.store(0);
  g_armed.store(true);
  RenderJob job;
  job.start = 0;
  job.end = 150000;
  job.blockFrames = 256;
  std::vector<std::vector<float>> out;
  std::vector<std::vector<std::vector<float>>> stems;
  REQUIRE_OK(renderToMemory(richScene(1), job, &out));
  job.kind = SQ_RENDER_STEMS;
  job.mix64 = true;
  REQUIRE_OK(renderToMemory(richScene(2), job, &out, &stems));
  g_armed.store(false);
#if defined(_MSC_VER) && defined(_DEBUG)
  _CrtSetAllocHook(nullptr);
#endif
  CHECK_EQ(g_rtAllocs.load(), uint64_t(0));
}
