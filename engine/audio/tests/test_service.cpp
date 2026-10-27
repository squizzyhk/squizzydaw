// engine/audio - IAudioEngine / ITransport / ISubject against a mock document (document.h is
// implemented by another unit), the NullDriver through the class registry, recording commits and
// bounce-in-place edits through IEditor.
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "fakes.hpp"
#include "sq/app/class_registry.hpp"
#include "sq/app/host_context.hpp"
#include "sq/app/settings.hpp"
#include "sq/engine/audio/devices.hpp"
#include "sq/engine/audio/engine.hpp"
#include "sq/platform.h"
#include "sqtest.hpp"

using namespace sq::engine::audio;

namespace {

class TopicLog final : public sq::Object<TopicLog, sq::IObserver> {
 public:
  std::vector<std::string> topics;
  void SQ_CALL onNotify(sq::IUnknown*, const char* topic, sq::IAttributes*) override { topics.emplace_back(topic); }
  int count(const char* t) const {
    int n = 0;
    for (auto& s : topics) n += s == t ? 1 : 0;
    return n;
  }
};

struct Fixture {
  sq::app::ClassRegistry reg;
  sq::Ptr<fakes::MDoc> doc = sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc());
  sq::Ptr<fakes::MTrack> t1, t2;
  Fixture() {
    fakes::registerFakeCodec(&reg);
    registerNullDriver(&reg);
    std::vector<std::vector<float>> mono(1, std::vector<float>(96000 * 2, 0.25f));
    fakes::putMemFile("mem://src1.tst", 48000.0, mono);
    t1 = sq::Ptr<fakes::MTrack>::adopt(new fakes::MTrack());
    t1->name = "Drums";
    t1->info.channels = 1;
    t1->info.pan = -1.0;
    auto c = sq::Ptr<fakes::MClip>::adopt(new fakes::MClip());
    c->info.start = 0;
    c->info.length = 4;  // 0..4 ppq = 0..96000 frames @120 bpm
    c->path = "mem://src1.tst";
    t1->clips.push_back(c);
    doc->tracks.push_back(t1);
    t2 = sq::Ptr<fakes::MTrack>::adopt(new fakes::MTrack());
    t2->name = "Keys";
    t2->info.kind = SQ_TRACK_INSTRUMENT;
    t2->info.channels = 1;
    auto m = sq::Ptr<fakes::MClip>::adopt(new fakes::MClip());
    m->info.kind = SQ_CLIP_MIDI;
    m->info.start = 0;
    m->info.length = 4;
    sq_clip_event e{};
    e.struct_size = sizeof e;
    e.position = 1.0;  // starts exactly where the test locates (no note chasing yet)
    e.length = 1.0;
    e.bytes[0] = 0x90;
    e.bytes[1] = 60;
    e.bytes[2] = 100;
    e.size = 3;
    m->events.push_back(e);
    t2->clips.push_back(m);
    doc->tracks.push_back(t2);
  }
};

NullDevice* offlineDevice(AudioEngine& eng) { return static_cast<NullDevice*>(eng.device()); }

}  // namespace

SQ_TEST("service: open the offline device through the registry, play a document, meters, transport") {
  Fixture f;
  NullDriver::defaults()[1].blockFrames = 128;
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&f.reg, nullptr));
  sq::Ptr<TopicLog> log = sq::Ptr<TopicLog>::adopt(new TopicLog());
  REQUIRE_OK(eng->addObserver(log.get()));
  CHECK_EQ(eng->addObserver(log.get()), SQ_S_FALSE);
  REQUIRE_OK(eng->setDocument(f.doc.get()));
  REQUIRE_OK(eng->openDevice(&kNullDriverClassId, "offline", nullptr));
  CHECK(log->count("engine.device") >= 1);

  sq_engine_status st{};
  st.struct_size = sizeof st;
  REQUIRE_OK(eng->getStatus(&st));
  CHECK((st.flags & SQ_ENGINE_DEVICE_OPEN) != 0);
  CHECK((st.flags & SQ_ENGINE_RUNNING) != 0);
  CHECK_EQ(st.block_frames, 128u);
  CHECK(std::strcmp(st.device_id, "offline") == 0);
  CHECK(st.driver_class == kNullDriverClassId);

  sq::Ptr<sq::ITransport> tr = eng.as<sq::ITransport>();
  REQUIRE(tr);
  sq::Ptr<sq::ISubject> subj = eng.as<sq::ISubject>();
  REQUIRE(subj);
  REQUIRE_OK(tr->locate(1.0));  // 24000 frames
  REQUIRE_OK(tr->play());
  offlineDevice(*eng)->pump(10);
  sq_transport_state ts{};
  ts.struct_size = sizeof ts;
  REQUIRE_OK(tr->getTransportState(&ts));
  CHECK((ts.flags & SQ_TRANSPORT_PLAYING) != 0);
  CHECK_EQ(ts.frames, int64_t(24000 + 1280));
  CHECK(std::fabs(ts.position - (1.0 + 1280.0 / 24000.0)) < 1e-9);
  CHECK_EQ(ts.bpm, 120.0);
  CHECK_EQ(ts.numerator, 4u);

  sq_meter_value mv[4];
  uint32_t n = 0;
  sq_guid id1 = f.t1->id;
  REQUIRE_OK(eng->getMeter(&id1, mv, 4, &n));
  CHECK_EQ(n, 2u);
  CHECK(std::fabs(mv[0].peak - 0.25f) < 1e-6f);  // hard-left mono clip at 0.25
  REQUIRE_OK(eng->getMeter(nullptr, mv, 4, &n));
  CHECK(mv[0].peak > 0.25f);  // master = drums + keys
  sq_guid bogus = fakes::newGuid();
  CHECK_EQ(eng->getMeter(&bogus, mv, 4, &n), SQ_E_NOTFOUND);

  // structural document.changed (clip edit) -> rebuild (new graph generation)
  const uint64_t gen = eng->core().currentGeneration();
  f.t1->clips[0]->info.length = 3.5;
  f.doc->fireChanged();
  CHECK(eng->core().currentGeneration() != gen);
  offlineDevice(*eng)->pump(2);

  REQUIRE_OK(tr->stop());
  offlineDevice(*eng)->pump(1);  // commands are applied by the audio thread
  REQUIRE_OK(tr->getTransportState(&ts));
  CHECK((ts.flags & SQ_TRANSPORT_PLAYING) == 0);
  CHECK(log->count("transport.state") >= 3);
  REQUIRE_OK(eng->closeDevice());
  REQUIRE_OK(eng->getStatus(&st));
  CHECK((st.flags & SQ_ENGINE_DEVICE_OPEN) == 0);
  REQUIRE_OK(eng->setDocument(nullptr));
  CHECK(f.doc->observers.empty());
}

SQ_TEST("service: recording through the transport commits clips in one transaction") {
  Fixture f;
  f.t1->info.flags |= SQ_TRACK_ARMED;
  NullDriver::defaults()[1].blockFrames = 256;
  NullDriver::defaults()[1].inputGenerator = [](int64_t, float* const* ch, uint32_t nch, uint32_t frames) {
    for (uint32_t c = 0; c < nch; ++c)
      for (uint32_t i = 0; i < frames; ++i) ch[c][i] = 0.5f;
  };
  sq::app::Settings settings;
  settings.setString("Media.Folder", "mem://media");
  settings.setString("Media.RecordFormat", "tst");
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&f.reg, &settings));
  REQUIRE_OK(eng->setDocument(f.doc.get()));
  REQUIRE_OK(eng->openDevice(&kNullDriverClassId, "offline", nullptr));
  REQUIRE_OK(eng->locate(2.0));
  REQUIRE_OK(eng->play());
  REQUIRE_OK(eng->record(SQ_TRUE));
  offlineDevice(*eng)->pump(100);  // 25600 frames
  REQUIRE_OK(eng->record(SQ_FALSE));
  NullDriver::defaults()[1].inputGenerator = nullptr;
  const auto takes = eng->lastTakes();
  REQUIRE_EQ(takes.size(), size_t(1));
  CHECK_EQ(takes[0].frames, int64_t(25600));
  CHECK_EQ(takes[0].startFrame, int64_t(48000 - 512));  // transport 2 ppq minus in+out latency
  fakes::MemFile mf;
  REQUIRE(fakes::getMemFile(takes[0].path, &mf));
  CHECK_EQ(mf.data[0][100], 0.5f);
  auto& lg = f.doc->editor->log;
  REQUIRE(lg.size() >= 3);
  CHECK(lg[0].action == "begin");
  CHECK(lg[1].action == "add");
  CHECK(lg[1].node == f.t1->id);
  CHECK(std::fabs(lg[1].a - (48000.0 - 512.0) / 24000.0) < 1e-9);
  CHECK(std::fabs(lg[1].b - 25600.0 / 24000.0) < 1e-9);
  CHECK(lg[1].source == takes[0].path);
  CHECK(lg[2].action == "commit");
  eng->closeDevice();
  NullDriver::defaults()[1].blockFrames = 256;
}

SQ_TEST("service: bounce in place renders the range and replaces clips in one undo step") {
  Fixture f;
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&f.reg, nullptr));
  REQUIRE_OK(eng->setDocument(f.doc.get()));
  sq_guid id1 = f.t1->id;
  sq_render_request rq{};
  rq.struct_size = sizeof rq;
  rq.kind = SQ_RENDER_BOUNCE_IN_PLACE;
  rq.start = 1.0;
  rq.end = 3.0;
  rq.track_ids = &id1;
  rq.track_count = 1;
  rq.output = "mem://bounce";
  rq.file_extension = "tst";
  sq::Ptr<sq::IRenderTask> task;
  REQUIRE_OK(eng->startRender(&rq, nullptr, task.put()));
  for (int i = 0; i < 2000 && task->getResult() == SQ_E_BUSY; ++i) {
    eng->idle();  // applies the document edit on the main thread once rendering is done
    task->wait(5);
  }
  REQUIRE_OK(task->getResult());
  REQUIRE_EQ(task->getOutputCount(), 1u);
  char path[512];
  uint32_t need = 0;
  REQUIRE_OK(task->getOutputPath(0, path, sizeof path, &need));
  fakes::MemFile mf;
  REQUIRE(fakes::getMemFile(path, &mf));
  CHECK_EQ(mf.frames(), int64_t(48000));
  CHECK_EQ(mf.data.size(), size_t(1));  // pre-fader, track channels
  CHECK_EQ(mf.data[0][10], 0.25f);
  auto& lg = f.doc->editor->log;
  // begin, split@1 (left part kept), split@3, remove middle, add bounced clip, commit
  REQUIRE_EQ(lg.size(), size_t(6));
  CHECK(lg[0].action == "begin");
  CHECK(lg[1].action == "split");
  CHECK_EQ(lg[1].a, 1.0);
  CHECK(lg[2].action == "split");
  CHECK_EQ(lg[2].a, 3.0);
  CHECK(lg[3].action == "remove");
  CHECK(lg[3].node == lg[2].node);
  CHECK(lg[4].action == "add");
  CHECK_EQ(lg[4].a, 1.0);
  CHECK_EQ(lg[4].b, 2.0);
  CHECK(lg[4].source == std::string(path));
  CHECK(lg[5].action == "commit");
}

// Regression: a device whose stream dies (WASAPI stream error / unplugged endpoint) used to leave
// the core on the "running" path forever (commands queued for nobody, graphs parked in pending_,
// stopRecording waiting 1 s, RUNNING reported). idle() must notice, close, notify and reopen.
SQ_TEST("service: device loss is detected by idle(), the device is reopened and transport works again") {
  Fixture f;
  NullDriver::defaults()[1].blockFrames = 128;
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&f.reg, nullptr));
  sq::Ptr<TopicLog> log = sq::Ptr<TopicLog>::adopt(new TopicLog());
  REQUIRE_OK(eng->addObserver(log.get()));
  REQUIRE_OK(eng->setDocument(f.doc.get()));
  REQUIRE_OK(eng->openDevice(&kNullDriverClassId, "offline", nullptr));
  REQUIRE_OK(eng->play());
  offlineDevice(*eng)->pump(4);
  NullDevice* first = offlineDevice(*eng);
  const int devTopics = log->count("engine.device");

  first->simulateLoss();  // driver thread: notifies the listener, no onStop()
  sq_engine_status st{};
  st.struct_size = sizeof st;
  REQUIRE_OK(eng->getStatus(&st));
  CHECK((st.flags & SQ_ENGINE_RUNNING) == 0);  // no longer claims to run on a dead device

  eng->idle();
  CHECK_EQ(eng->deviceRecoveries(), 1u);
  CHECK_OK(eng->lastRecoveryResult());
  CHECK(log->count("engine.device") >= devTopics + 2);  // closed + reopened
  REQUIRE(eng->device() != nullptr);
  CHECK(eng->core().running());
  REQUIRE_OK(eng->getStatus(&st));
  CHECK((st.flags & SQ_ENGINE_RUNNING) != 0);
  CHECK(std::strcmp(st.device_id, "offline") == 0);

  // Commands reach the new audio thread again (no stuck SPSC queue / pending graph).
  REQUIRE_OK(eng->locate(1.0));
  REQUIRE_OK(eng->play());
  const uint64_t gen = eng->core().currentGeneration();
  f.t2->clips[0]->info.length = 3.0;
  f.doc->fireChanged();
  CHECK(eng->core().currentGeneration() != gen);
  offlineDevice(*eng)->pump(10);
  sq_transport_state ts{};
  ts.struct_size = sizeof ts;
  REQUIRE_OK(eng->getTransportState(&ts));
  CHECK((ts.flags & SQ_TRANSPORT_PLAYING) != 0);
  CHECK_EQ(ts.frames, int64_t(24000 + 1280));

  // A second idle() with a healthy device does nothing.
  eng->idle();
  CHECK_EQ(eng->deviceRecoveries(), 1u);
  eng->closeDevice();
  NullDriver::defaults()[1].blockFrames = 256;
}

SQ_TEST("service: device lost without notification is caught by polling; falls back to the default device") {
  Fixture f;
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&f.reg, nullptr));
  REQUIRE_OK(eng->setDocument(f.doc.get()));
  REQUIRE_OK(eng->openDevice(&kNullDriverClassId, "offline", nullptr));
  NullDevice* first = offlineDevice(*eng);
  first->setListeners(nullptr);  // a driver that never notifies
  NullDriver::defaults()[1].id = "offline-unplugged";  // the endpoint is gone for good
  first->simulateLoss();
  eng->idle();
  NullDriver::defaults()[1].id = "offline";
  CHECK_EQ(eng->deviceRecoveries(), 1u);
  CHECK_OK(eng->lastRecoveryResult());
  sq_engine_status st{};
  st.struct_size = sizeof st;
  REQUIRE_OK(eng->getStatus(&st));
  CHECK((st.flags & SQ_ENGINE_DEVICE_OPEN) != 0);
  CHECK((st.flags & SQ_ENGINE_RUNNING) != 0);
  CHECK(std::strcmp(st.device_id, "null") == 0);  // driver default (index 0)
  REQUIRE_OK(eng->closeDevice());
}

SQ_TEST("service: device loss while recording commits the take without waiting on dead callbacks") {
  Fixture f;
  f.t1->info.flags |= SQ_TRACK_ARMED;
  NullDriver::defaults()[1].inputGenerator = [](int64_t, float* const* ch, uint32_t nch, uint32_t frames) {
    for (uint32_t c = 0; c < nch; ++c)
      for (uint32_t i = 0; i < frames; ++i) ch[c][i] = 0.5f;
  };
  sq::app::Settings settings;
  settings.setString("Media.Folder", "mem://media-loss");
  settings.setString("Media.RecordFormat", "tst");
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&f.reg, &settings));
  REQUIRE_OK(eng->setDocument(f.doc.get()));
  REQUIRE_OK(eng->openDevice(&kNullDriverClassId, "offline", nullptr));
  REQUIRE_OK(eng->play());
  REQUIRE_OK(eng->record(SQ_TRUE));
  offlineDevice(*eng)->pump(40);  // 10240 frames
  const std::vector<NullDeviceOptions> saved = NullDriver::defaults();
  NullDriver::defaults().clear();  // every endpoint is gone: nothing to fall back to
  offlineDevice(*eng)->simulateLoss();
  const uint64_t t0 = sq_time_monotonic_ns();
  eng->idle();
  const double ms = static_cast<double>(sq_time_monotonic_ns() - t0) / 1e6;
  NullDriver::defaults() = saved;
  NullDriver::defaults()[1].inputGenerator = nullptr;
  CHECK(ms < 500.0);  // used to block ~1 s in waitForCallback
  CHECK(SQ_FAILED(eng->lastRecoveryResult()));
  CHECK(eng->device() == nullptr);
  CHECK(!eng->core().running());
  const auto takes = eng->lastTakes();
  REQUIRE_EQ(takes.size(), size_t(1));
  CHECK_EQ(takes[0].frames, int64_t(10240));
  // Engine closed: transport and graph changes take the direct (non-running) path.
  REQUIRE_OK(eng->stop());
  REQUIRE_OK(eng->locate(2.0));
  sq_transport_state ts{};
  ts.struct_size = sizeof ts;
  REQUIRE_OK(eng->getTransportState(&ts));
  CHECK_EQ(ts.frames, int64_t(48000));
  CHECK((ts.flags & SQ_TRANSPORT_PLAYING) == 0);
}

// Regression: every document.changed (rename, colour, fader move...) used to recompile the whole
// graph - new plugin instances, empty PDC delay lines, restarted fader ramps - an audible glitch
// per edit. Mixer-only and cosmetic edits are now RT commands on the installed graph.
SQ_TEST("service: mixer-only and cosmetic document edits do not recompile the graph; structural edits do") {
  Fixture f;
  NullDriver::defaults()[1].blockFrames = 128;
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&f.reg, nullptr));
  REQUIRE_OK(eng->setDocument(f.doc.get()));
  REQUIRE_OK(eng->openDevice(&kNullDriverClassId, "offline", nullptr));
  REQUIRE_OK(eng->locate(0.5));
  REQUIRE_OK(eng->play());
  offlineDevice(*eng)->pump(4);
  const uint64_t gen = eng->core().currentGeneration();
  const uint64_t compiles = eng->compileCount();
  sq_meter_value mv[2];
  uint32_t n = 0;
  sq_guid id1 = f.t1->id;
  REQUIRE_OK(eng->getMeter(&id1, mv, 2, &n));
  CHECK(mv[0].peak > 0.1f);  // audible before the fader move

  // rename: nothing to do for the audio graph
  f.t1->name = "Drums (renamed)";
  f.doc->fireChanged();
  CHECK_EQ(eng->core().currentGeneration(), gen);
  CHECK_EQ(eng->compileCount(), compiles);

  // fader move: -inf -> applied as a TrackVolume command on the same graph
  f.t1->info.volume_db = -200.0;
  f.doc->fireChanged();
  CHECK_EQ(eng->core().currentGeneration(), gen);
  CHECK_EQ(eng->compileCount(), compiles);
  offlineDevice(*eng)->pump(20);  // fader ramp settles
  REQUIRE_OK(eng->getMeter(&id1, mv, 2, &n));  // peak hold is reset by a read
  offlineDevice(*eng)->pump(20);
  REQUIRE_OK(eng->getMeter(&id1, mv, 2, &n));
  CHECK(mv[0].peak < 1e-3f);
  CHECK(eng->graphDesc().tracks[0].volume < 1e-6f);

  // mute / solo / pan on the other track: still commands
  f.t2->info.flags |= SQ_TRACK_MUTE;
  f.t2->info.pan = 0.5;
  f.doc->fireChanged();
  CHECK_EQ(eng->compileCount(), compiles);

  // structural edit (clip moved) -> recompile
  f.t1->clips[0]->info.start = 0.25;
  f.doc->fireChanged();
  CHECK(eng->core().currentGeneration() != gen);
  CHECK_EQ(eng->compileCount(), compiles + 1);
  // the recompiled graph carries the document's mixer values
  offlineDevice(*eng)->pump(20);
  REQUIRE_OK(eng->getMeter(&id1, mv, 2, &n));
  offlineDevice(*eng)->pump(20);
  REQUIRE_OK(eng->getMeter(&id1, mv, 2, &n));
  CHECK(mv[0].peak < 1e-3f);
  eng->closeDevice();
  NullDriver::defaults()[1].blockFrames = 256;
}

SQ_TEST("service: registers as the IAudioEngine host service") {
  sq::app::Settings settings;
  sq::app::ClassRegistry reg;
  sq::app::HostContext host(settings, reg);
  sq::Ptr<AudioEngine> eng = sq::Ptr<AudioEngine>::adopt(new AudioEngine(&reg, &settings));
  REQUIRE_OK(eng->registerService(&host));
  sq::Ptr<sq::IAudioEngine> q;
  REQUIRE_OK(host.queryService(&sq::IAudioEngine::iid, q.putVoid()));
  CHECK(q.get() == static_cast<sq::IAudioEngine*>(eng.get()));
  q = nullptr;
  eng->unregisterService(&host);
  CHECK_EQ(host.serviceCount(), size_t(0));
}
