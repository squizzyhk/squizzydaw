// End-to-end tests: the real app shell (sq::app::Shell, the same object SquizzyStudio.exe uses)
// with every service module from bin/services loaded through the real loader. Each case proves a
// plan exit criterion across units, never through mocks:
//   M1  create -> save -> close -> reopen, identical document
//   M2  record 2 tracks at 64-frame blocks through the NullDevice, takes written by codec-wav,
//       committed as clips, bit-exact against the device input
//   M3  keyboard-addressable editing workflow (commands only), exact undo/redo; browser search
//       over 1000 indexed items (sqlite-index)
//   M4  out-of-process scan survives a crashing and a hanging plugin; the CLAP test gain plugin
//       inserted on a track halves the rendered mixdown, delay-compensated
//   M6  a user script package adds a menu command that edits the document with full undo; a
//       bundled script command works against the real engine
//   M7  export WAV + FLAC (identical audio), MIDI file (read back), DAWproject (reimported)
//   M4 (VST3)  the VST3 test plugin scanned and inserted like the CLAP one, PDC exact
//   M5/M8      stock themes + theme switch, remote pages, control surfaces, OSC -> commands,
//              audio device listing/selection, optional services (account, tempo sync, video)
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <vector>

#include "sq/app/shell.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/engine/audio/devices.hpp"
#include "sq/engine/audio/engine.hpp"
#include "sq/interfaces/codec.h"
#include "sq/interfaces/midi.h"
#include "sq/interfaces/remote.h"
#include "sq/interfaces/ui.h"
#include "sq/platform.hpp"
#include "sq/ui/theme.hpp"
#include "sqtest.hpp"

using namespace sq;

namespace {

std::string tempDir(const char* tag) {
  sq_guid g{};
  sq_guid_generate(&g);
  std::string d = joinPath(specialDir(SQ_DIR_TEMP), std::string("sqe2e-") + tag + "-" + std::to_string(g.data1));
  sq_fs_create_dirs(d.c_str());
  return d;
}

// A shell with all services, no audio device, its own config files under `dir`.
struct TestShell {
  app::Shell sh;
  std::string dir;
  explicit TestShell(const char* tag, std::function<void(app::Settings&)> cfg = {}) : dir(tempDir(tag)) {
    app::Shell::registerDefaults(sh.settings);
    sh.settings.setString("Media.Folder", joinPath(dir, "Media").c_str());
    sh.settings.setString("Media.Browser.Index", joinPath(dir, "content.tags.db").c_str());
    sh.settings.setString("Services.pluginscanner.Manifest", joinPath(dir, "plugins.json").c_str());
    sh.settings.setBool("Services.pluginscanner.UseDefaultPaths", SQ_FALSE);
    if (cfg) cfg(sh.settings);
    app::ShellOptions o;
    o.audioDriver = "none";
    REQUIRE_OK(sh.start(o));
    REQUIRE(sh.hasEngine());
  }
  ~TestShell() {
    sh.stop();
    sq_fs_remove_all(dir.c_str());
  }
  sq_result run(const char* cmd, std::function<void(IAttributes*)> args = {}, IAttributes* res = nullptr) {
    auto a = makeAttributes();
    if (args) args(a.get());
    sq_result r = sh.execute(cmd, a.get(), res);
    sh.idle();
    if (SQ_FAILED(r)) std::printf("    %s -> %s %s\n", cmd, sq_result_string(r), sh.lastMessage().c_str());
    return r;
  }
  Ptr<IDocument> doc() { return sh.activeDocument(); }
  Ptr<IEditor> editor() {
    Ptr<IEditor> e;
    if (auto d = doc()) d->getEditor(e.put());
    return e;
  }
  sq_guid exec(const sq_guid& action, std::function<void(IAttributes*)> params) {
    auto p = makeAttributes();
    params(p.get());
    auto res = makeAttributes();
    REQUIRE_OK(editor()->execute(&action, p.get(), res.get()));
    sq_guid g{};
    res->getGuid("node", &g);
    return g;
  }
};

std::string name(INode* n) {
  char b[256];
  uint32_t need = 0;
  return n && n->getName(b, sizeof b, &need) == SQ_OK ? std::string(b) : std::string();
}

// Canonical text of everything persistent in a document (tracks, clips, events, markers, tempo).
std::string docHash(IDocument* d) {
  std::string s;
  char b[256];
  sq_document_info di{};
  di.struct_size = sizeof di;
  d->getDocumentInfo(&di);
  for (uint32_t i = 0; i < di.track_count; ++i) {
    Ptr<ITrack> t;
    d->getTrack(i, t.put());
    sq_track_info ti{};
    ti.struct_size = sizeof ti;
    t->getTrackInfo(&ti);
    auto props = makeAttributes();
    t->getProperties(props.get());
    std::snprintf(b, sizeof b, "T%d %s k%d c%u f%x v%.6f p%.6f|", i, name(t.get()).c_str(), ti.kind, ti.channels, ti.flags, ti.volume_db, ti.pan);
    s += b + getAttrString(props.get(), "user.inserts") + "\n";
    for (uint32_t k = 0; k < ti.clip_count; ++k) {
      Ptr<IClip> c;
      t->getClip(k, c.put());
      sq_clip_info ci{};
      ci.struct_size = sizeof ci;
      c->getClipInfo(&ci);
      std::snprintf(b, sizeof b, " C%d s%.6f l%.6f o%.6f g%.4f fi%.4f fo%.4f f%x e%u|", ci.kind, ci.start, ci.length, ci.source_offset_seconds,
                    ci.gain_db, ci.fade_in_seconds, ci.fade_out_seconds, ci.flags, ci.event_count);
      s += b;
      for (uint32_t e = 0; e < ci.event_count; ++e) {
        sq_clip_event ev{};
        ev.struct_size = sizeof ev;
        c->getEvent(e, &ev);
        std::snprintf(b, sizeof b, "%.4f/%.4f/%02x%02x%02x ", ev.position, ev.length, ev.bytes[0], ev.bytes[1], ev.bytes[2]);
        s += b;
      }
      s += "\n";
    }
  }
  for (uint32_t m = 0; m < d->getMarkerCount(); ++m) {
    sq_doc_marker mk{};
    mk.struct_size = sizeof mk;
    d->getMarker(m, &mk);
    std::snprintf(b, sizeof b, "M %.6f %s\n", mk.position, mk.name);
    s += b;
  }
  Ptr<ITempoMap> tm;
  d->getTempoMap(tm.put());
  for (uint32_t i = 0; i < tm->getTempoCount(); ++i) {
    sq_tempo_point tp{};
    tp.struct_size = sizeof tp;
    tm->getTempo(i, &tp);
    std::snprintf(b, sizeof b, "B %.6f %.6f\n", tp.position, tp.bpm);
    s += b;
  }
  return s;
}

// Mono WAV through the real codec-wav service.
void writeWav(app::Shell& sh, const std::string& path, double sr, uint32_t frames, const std::function<float(uint32_t)>& fn) {
  Ptr<ICodecService> codec = Ptr<ICodecService>::adopt(codec::findCodec(&sh.registry, "wav", nullptr, SQ_CODEC_WRITE));
  REQUIRE(codec);
  Ptr<IAudioFileWriter> w;
  REQUIRE_OK(codec->createWriter("wav", w.put()));
  sq_audio_file_format f{};
  f.struct_size = sizeof f;
  f.sample_rate = sr;
  f.channels = 1;
  f.sample_format = SQ_SAMPLE_F32;
  f.bits_per_sample = 32;
  REQUIRE_OK(w->open(path.c_str(), &f, nullptr));
  std::vector<float> x(frames);
  for (uint32_t i = 0; i < frames; ++i) x[i] = fn(i);
  float* ch[1] = {x.data()};
  sq_audio_block blk{ch, 1, frames, 0};
  REQUIRE_OK(w->write(&blk));
  REQUIRE_OK(w->finalize());
}

struct Audio {
  double sr = 0;
  std::vector<std::vector<float>> ch;
  size_t frames() const { return ch.empty() ? 0 : ch[0].size(); }
};
bool readAudio(app::Shell& sh, const std::string& path, Audio& out) {
  size_t dot = path.find_last_of('.');
  std::string ext = path.substr(dot + 1);
  Ptr<ICodecService> codec = Ptr<ICodecService>::adopt(codec::findCodec(&sh.registry, ext.c_str(), nullptr, SQ_CODEC_READ));
  if (!codec) return false;
  Ptr<IAudioFileReader> r;
  if (codec->createReader(r.put()) != SQ_OK || r->open(path.c_str()) != SQ_OK) return false;
  sq_audio_file_format f{};
  f.struct_size = sizeof f;
  r->getFormat(&f);
  out.sr = f.sample_rate;
  out.ch.assign(f.channels, {});
  std::vector<std::vector<float>> buf(f.channels, std::vector<float>(4096));
  std::vector<float*> ptrs(f.channels);
  for (;;) {
    for (uint32_t c = 0; c < f.channels; ++c) ptrs[c] = buf[c].data();
    sq_audio_block blk{ptrs.data(), f.channels, 4096, 0};
    uint32_t got = 0;
    sq_result rr = r->read(&blk, &got);
    for (uint32_t c = 0; c < f.channels; ++c) out.ch[c].insert(out.ch[c].end(), buf[c].begin(), buf[c].begin() + got);
    if (SQ_FAILED(rr) || got == 0) break;
  }
  return true;
}

std::string clipSource(IClip* c) {
  char b[1024];
  uint32_t need = 0;
  return c->getSourcePath(b, sizeof b, &need) == SQ_OK ? std::string(b) : std::string();
}

float deviceInput(int64_t n, uint32_t c) {
  uint64_t x = static_cast<uint64_t>(n) * 0x9E3779B97F4A7C15ull + (static_cast<uint64_t>(c) + 7) * 0xBF58476D1CE4E5B9ull;
  x ^= x >> 31;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 29;
  return static_cast<float>(x & 0xFFFFFFull) / 16777216.0f - 0.5f;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
SQ_TEST("M1: create, save, close, reopen -> identical document (+ commands through the shell)") {
  TestShell t("m1");
  REQUIRE_OK(t.run("File.New"));
  auto res = makeAttributes();
  REQUIRE_OK(t.run("Track.AddAudio", [](IAttributes* a) { a->setString("name", "Drums"); }, res.get()));
  REQUIRE_OK(t.run("Track.AddInstrument", [](IAttributes* a) { a->setString("name", "Keys"); }));
  sq_guid drums{};
  res->getGuid("node", &drums);
  t.exec(doc::kSetProperty, [&](IAttributes* p) {
    p->setGuid("node", &drums);
    p->setString("key", "volumeDb");
    p->setDouble("value", -3.5);
  });
  t.exec(doc::kAddMarker, [](IAttributes* p) {
    p->setDouble("position", 16);
    p->setString("name", "Verse");
  });
  t.exec(doc::kSetTempo, [](IAttributes* p) {
    p->setDouble("position", 0);
    p->setDouble("bpm", 132);
  });
  std::string before = docHash(t.doc().get());
  std::string path = joinPath(t.dir, "song.song");
  REQUIRE_OK(t.run("File.Save", [&](IAttributes* a) { a->setString("path", path.c_str()); }));
  REQUIRE(sq_fs_exists(path.c_str()));
  REQUIRE_OK(t.run("File.Close"));
  CHECK(!t.doc());
  REQUIRE_OK(t.run("File.Open", [&](IAttributes* a) { a->setString("path", path.c_str()); }));
  REQUIRE(t.doc());
  CHECK_EQ(docHash(t.doc().get()), before);
  CHECK(before.find("Drums") != std::string::npos);
  CHECK(before.find("v-3.5") != std::string::npos);
  CHECK(t.run("File.Open", [&](IAttributes* a) { a->setString("path", joinPath(t.dir, "missing.song").c_str()); }) < 0);
  CHECK_EQ(t.sh.execute("No.Such"), SQ_E_NOTFOUND);
}

// ------------------------------------------------------------------------------------------------
SQ_TEST("M2: record 2 tracks @64 frames through the NullDevice into WAV takes") {
  TestShell t("m2", [](app::Settings& s) {
    s.setString("Media.RecordFormat", "wav");
    s.setInt("Devices.BlockSize", 64);
  });
  REQUIRE_OK(t.run("File.New"));
  for (int k = 0; k < 2; ++k) {
    REQUIRE_OK(t.run("Track.AddAudio", [&](IAttributes* a) {
      a->setString("name", k == 0 ? "Vox" : "Gtr");
      a->setInt("channels", 1);
    }));
    REQUIRE_OK(t.run("Track.SetInput", [&](IAttributes* a) { a->setInt("channel", k); }));
    REQUIRE_OK(t.run("Track.ToggleArm"));
  }
  engine::audio::NullDeviceOptions o;
  o.blockFrames = 64;
  o.sampleRate = 48000;
  o.inputChannels = 2;
  o.outputChannels = 2;
  // 16x realtime: proves sample-exact takes (no lost/repeated frame, no ring overflow). Device
  // late callbacks are not asserted at this pace (83 us periods measure OS scheduling); the
  // dropout assertion lives in engine_audio_record's realtime-paced case.
  o.speed = 16.0;
  o.inputGenerator = [](int64_t pos, float* const* ch, uint32_t nch, uint32_t n) {
    for (uint32_t c = 0; c < nch; ++c)
      for (uint32_t i = 0; i < n; ++i) ch[c][i] = deviceInput(pos + i, c);
  };
  auto dev = Ptr<engine::audio::NullDevice>::adopt(new engine::audio::NullDevice(o));
  REQUIRE_OK(t.sh.audio()->openDeviceObject(dev.get(), nullptr));
  sq_engine_status st{};
  st.struct_size = sizeof st;
  REQUIRE_OK(t.sh.engine()->getStatus(&st));
  CHECK_EQ(st.block_frames, 64u);

  REQUIRE_OK(t.run("Transport.Record"));
  const int64_t target = 48000 * 5;  // 5 s of audio
  const int64_t start = dev->position();
  uint64_t t0 = sq_time_monotonic_ns();
  while (dev->position() < start + target && sq_time_monotonic_ns() - t0 < 60ull * 1000000000ull) {
    sq_thread_sleep_ms(5);
    t.sh.idle();
  }
  REQUIRE_OK(t.run("Transport.Stop"));
  t.sh.audio()->closeDevice();
  t.sh.idle();

  auto d = t.doc();
  sq_document_info di{};
  di.struct_size = sizeof di;
  d->getDocumentInfo(&di);
  int found = 0;
  for (uint32_t i = 0; i < di.track_count; ++i) {
    Ptr<ITrack> tr;
    d->getTrack(i, tr.put());
    sq_track_info ti{};
    ti.struct_size = sizeof ti;
    tr->getTrackInfo(&ti);
    if (ti.kind != SQ_TRACK_AUDIO) continue;
    REQUIRE_EQ(ti.clip_count, 1u);
    Ptr<IClip> c;
    REQUIRE_OK(tr->getClip(0, c.put()));
    std::string src = clipSource(c.get());
    CHECK(src.find(".wav") != std::string::npos);
    Audio a;
    REQUIRE(readAudio(t.sh, src, a));
    REQUIRE(a.frames() >= static_cast<size_t>(target));
    uint32_t inCh = name(tr.get()) == "Vox" ? 0u : 1u;
    // Locate the first recorded frame in the device input stream, then compare every sample.
    int64_t p0 = -1;
    for (int64_t p = 0; p < dev->position(); ++p)
      if (deviceInput(p, inCh) == a.ch[0][0] && deviceInput(p + 1, inCh) == a.ch[0][1] && deviceInput(p + 2, inCh) == a.ch[0][2]) {
        p0 = p;
        break;
      }
    REQUIRE(p0 >= 0);
    CHECK_EQ(p0 % 64, int64_t(0));
    int64_t bad = 0;
    for (size_t k = 0; k < a.frames(); ++k)
      if (a.ch[0][k] != deviceInput(p0 + static_cast<int64_t>(k), inCh)) ++bad;
    CHECK_EQ(bad, int64_t(0));
    ++found;
  }
  CHECK_EQ(found, 2);
  for (const auto& take : t.sh.audio()->lastTakes()) CHECK_EQ(take.overflowFrames, uint64_t(0));
  // The take commit is one undo step.
  REQUIRE_OK(t.run("Edit.Undo"));
  Ptr<ITrack> tr;
  d->getTrack(di.track_count - 1, tr.put());
  sq_track_info ti{};
  ti.struct_size = sizeof ti;
  tr->getTrackInfo(&ti);
  CHECK_EQ(ti.clip_count, 0u);
}

// ------------------------------------------------------------------------------------------------
SQ_TEST("M3: keyboard-only editing workflow with exact undo/redo, and browser search over 1000 items") {
  TestShell t("m3");
  std::string wav = joinPath(t.dir, "loop.wav");
  writeWav(t.sh, wav, 48000, 48000 * 8, [](uint32_t i) { return 0.25f * std::sin(static_cast<float>(i) * 0.01f); });
  REQUIRE_OK(t.run("File.New"));
  auto res = makeAttributes();
  REQUIRE_OK(t.run("Track.AddAudio", [](IAttributes* a) { a->setString("name", "Loop"); }, res.get()));
  sq_guid track{};
  res->getGuid("node", &track);
  std::vector<sq_guid> clips;
  for (double s : {0.0, 8.0})
    clips.push_back(t.exec(doc::kAddClip, [&](IAttributes* p) {
      p->setGuid("track", &track);
      p->setInt("kind", SQ_CLIP_AUDIO);
      p->setDouble("start", s);
      p->setDouble("length", 8);
      p->setString("source", wav.c_str());
    }));
  auto ed = t.editor();
  const std::string h0 = docHash(t.doc().get());
  const uint32_t u0 = ed->getUndoCount();

  // Key-map driven session: only commands, no direct document access.
  REQUIRE_OK(t.run("Navigate.SetEditState", [&](IAttributes* a) {
    a->setDouble("cursor", 2);
    a->setGuid("track", &track);
    a->setString("nodes", toString(clips[0]).c_str());
  }));
  const char* editing[] = {"Edit.Split", "Edit.NudgeRight", "Edit.GainUp", "Edit.GainUp", "Edit.FadeOutFromCursor", "Edit.Duplicate", "Edit.Delete"};
  uint32_t steps = 0;
  for (const char* cmd : editing) {
    uint32_t before = ed->getUndoCount();
    if (std::strcmp(cmd, "Edit.FadeOutFromCursor") == 0) REQUIRE_OK(t.run("Navigate.SetEditState", [](IAttributes* a) { a->setDouble("cursor", 5); }));
    sq_result r = t.run(cmd);
    if (r == SQ_COMMAND_DISABLED) {
      std::printf("    %s disabled in this state (skipped)\n", cmd);
      continue;
    }
    REQUIRE_OK(r);
    CHECK_EQ(ed->getUndoCount(), before + 1);  // every editing command = exactly one undo step
    ++steps;
    CHECK(t.run("Navigate.SelectAll") >= 0);  // journal only: never an undo step
    CHECK_EQ(ed->getUndoCount(), before + 1);
  }
  CHECK(steps >= 5);
  const std::string h1 = docHash(t.doc().get());
  CHECK(h1 != h0);
  while (ed->getUndoCount() > u0) REQUIRE_OK(t.run("Edit.Undo"));
  CHECK_EQ(docHash(t.doc().get()), h0);
  while (ed->canRedo()) REQUIRE_OK(t.run("Edit.Redo"));
  CHECK_EQ(docHash(t.doc().get()), h1);

  // Browser: 1000 files in a small folder tree, indexed and searched through the shell.
  std::string lib = joinPath(t.dir, "Library");
  const char* folders[] = {"Drums/Kicks", "Drums/Snares", "Bass", "Keys", "Vocals"};
  int perFolder = 200;
  for (int f = 0; f < 5; ++f)
    for (int i = 0; i < perFolder; ++i) {
      std::string base = std::string(f < 2 ? (f == 0 ? "kick" : "snare") : f == 2 ? "bass" : f == 3 ? "piano" : "vox") + "_" + std::to_string(i);
      std::string ext = (i % 4 == 0) ? ".mid" : ".wav";
      REQUIRE_OK(writeFile(joinPath(joinPath(lib, folders[f]), base + ext), "x"));
    }
  auto r = makeAttributes();
  uint64_t ti0 = sq_time_monotonic_ns();
  REQUIRE_OK(t.run("Browser.IndexFolder", [&](IAttributes* a) { a->setString("folder", lib.c_str()); }, r.get()));
  CHECK_EQ(r->getInt("count", 0), 1000);
  std::printf("    indexed 1000 items in %.1f ms\n", static_cast<double>(sq_time_monotonic_ns() - ti0) / 1e6);
  auto search = [&](const char* text, const char* tags, const char* kind) {
    auto out = makeAttributes();
    uint64_t q0 = sq_time_monotonic_ns();
    REQUIRE_OK(t.run("Browser.Search", [&](IAttributes* a) {
      if (text) a->setString("text", text);
      if (tags) a->setString("tags", tags);
      if (kind) a->setString("kind", kind);
    }, out.get()));
    double ms = static_cast<double>(sq_time_monotonic_ns() - q0) / 1e6;
    CHECK(ms < 200.0);
    return out->getInt("count", -1);
  };
  CHECK_EQ(search("kick", nullptr, nullptr), 200);
  CHECK_EQ(search(nullptr, "drums", nullptr), 400);
  CHECK_EQ(search(nullptr, "drums;snares", "midi"), 50);
  CHECK_EQ(search("piano", nullptr, "audio"), 150);
  CHECK_EQ(search("nothing-matches-this", nullptr, nullptr), 0);
}

// ------------------------------------------------------------------------------------------------
SQ_TEST("M4: out-of-process scan survives crash/hang plugins; gain plugin on a track, compensated") {
  std::string plugins = joinPath(specialDir(SQ_DIR_EXE), "testplugins");
  TestShell t("m4", [&](app::Settings& s) {
    s.setString("Services.pluginscanner.Paths", plugins.c_str());
    s.setInt("Services.pluginscanner.TimeoutMs", 3000);
  });
  auto reg = t.sh.plugins();
  REQUIRE(reg);
  REQUIRE_OK(t.run("Plugins.Rescan", [](IAttributes* a) { a->setBool("full", SQ_TRUE); }));
  for (int i = 0; i < 600 && reg->isScanning(); ++i) sq_thread_sleep_ms(50);
  REQUIRE(!reg->isScanning());
  int ok = 0, crashed = 0, timeout = 0;
  for (uint32_t i = 0; i < reg->getPluginCount(); ++i) {
    sq_plugin_desc d{};
    d.struct_size = sizeof d;
    reg->getPlugin(i, &d);
    ok += d.status == SQ_PLUGIN_STATUS_OK;
    crashed += d.status == SQ_PLUGIN_STATUS_CRASHED;
    timeout += d.status == SQ_PLUGIN_STATUS_TIMEOUT;
  }
  CHECK_EQ(ok, 1);
  CHECK_EQ(crashed, 1);
  CHECK_EQ(timeout, 1);

  std::string wav = joinPath(t.dir, "src.wav");
  // A non-periodic ramp-ish signal so any misalignment between the two renders shows.
  writeWav(t.sh, wav, 48000, 48000 * 2, [](uint32_t i) { return 0.4f * std::sin(static_cast<float>(i) * 0.0031f + 0.00001f * static_cast<float>(i) * static_cast<float>(i) * 0.001f); });
  REQUIRE_OK(t.run("File.New"));
  auto res = makeAttributes();
  REQUIRE_OK(t.run("Track.AddAudio", [](IAttributes* a) {
    a->setString("name", "Gtr");
    a->setInt("channels", 1);
  }, res.get()));
  sq_guid track{};
  res->getGuid("node", &track);
  t.exec(doc::kAddClip, [&](IAttributes* p) {
    p->setGuid("track", &track);
    p->setInt("kind", SQ_CLIP_AUDIO);
    p->setDouble("start", 0);
    p->setDouble("length", 4);  // 2 s at 120 bpm
    p->setString("source", wav.c_str());
  });
  std::string dry = joinPath(t.dir, "dry.wav"), wet = joinPath(t.dir, "wet.wav");
  REQUIRE_OK(t.run("File.ExportAudio", [&](IAttributes* a) { a->setString("path", dry.c_str()); }));
  // The quarantined crasher can never be inserted.
  CHECK(t.run("Track.InsertPlugin", [](IAttributes* a) { a->setString("plugin", "sq-test-crash.clap"); }) < 0);
  REQUIRE_OK(t.run("Track.InsertPlugin", [](IAttributes* a) {
    a->setString("plugin", "clap:studio.squizzy.test.gain");
    a->setString("params", "1=-6.0205999");
  }));
  CHECK(t.sh.audio()->buildWarnings().empty());
  REQUIRE_OK(t.run("File.ExportAudio", [&](IAttributes* a) { a->setString("path", wet.c_str()); }));
  Audio A, B;
  REQUIRE(readAudio(t.sh, dry, A));
  REQUIRE(readAudio(t.sh, wet, B));
  REQUIRE_EQ(A.frames(), B.frames());
  REQUIRE(A.frames() > 48000);
  double maxErr = 0, peak = 0;
  for (size_t i = 0; i < A.frames(); ++i) {
    maxErr = std::fmax(maxErr, std::fabs(B.ch[0][i] - 0.5 * A.ch[0][i]));
    peak = std::fmax(peak, std::fabs(A.ch[0][i]));
  }
  std::printf("    plugin render: peak %.3f, max |wet - dry/2| = %.2e\n", peak, maxErr);
  CHECK(peak > 0.1);
  CHECK(maxErr < 1e-4);  // exact half everywhere = the 64-frame plugin latency was compensated
  // Insert is part of the document: undo removes it, and the render is dry again.
  REQUIRE_OK(t.run("Edit.Undo"));
  REQUIRE_OK(t.run("File.ExportAudio", [&](IAttributes* a) { a->setString("path", wet.c_str()); }));
  REQUIRE(readAudio(t.sh, wet, B));
  double d2 = 0;
  for (size_t i = 0; i < A.frames(); ++i) d2 = std::fmax(d2, std::fabs(B.ch[0][i] - A.ch[0][i]));
  CHECK(d2 < 1e-6);
}

SQ_TEST("M4 (VST3): scanned out of process, inserted on a track through the shell, compensated") {
  // The VST3 test plugin lives in its own folder (bin/testplugins holds the CLAP set that M4 and
  // the scanner tests count exactly).
  std::string plugins = joinPath(specialDir(SQ_DIR_EXE), "testplugins-vst3");
  if (!sq_fs_exists(joinPath(plugins, "sq-test-gain.vst3").c_str())) SQ_SKIP("pluginhost-vst3 test plugin not built (filtered build)");
  TestShell t("m4v", [&](app::Settings& s) {
    s.setString("Services.pluginscanner.Paths", plugins.c_str());
    s.setInt("Services.pluginscanner.TimeoutMs", 10000);
  });
  auto reg = t.sh.plugins();
  REQUIRE(reg);
  REQUIRE_OK(t.run("Plugins.Rescan", [](IAttributes* a) { a->setBool("full", SQ_TRUE); }));
  for (int i = 0; i < 600 && reg->isScanning(); ++i) sq_thread_sleep_ms(50);
  REQUIRE(!reg->isScanning());
  int vst3 = 0;
  for (uint32_t i = 0; i < reg->getPluginCount(); ++i) {
    sq_plugin_desc d{};
    d.struct_size = sizeof d;
    reg->getPlugin(i, &d);
    vst3 += d.status == SQ_PLUGIN_STATUS_OK && std::strcmp(d.format, "vst3") == 0;
  }
  REQUIRE_EQ(vst3, 1);

  std::string wav = joinPath(t.dir, "src.wav");
  writeWav(t.sh, wav, 48000, 48000 * 2, [](uint32_t i) { return 0.4f * std::sin(static_cast<float>(i) * 0.0047f + 0.000000013f * static_cast<float>(i) * static_cast<float>(i)); });
  REQUIRE_OK(t.run("File.New"));
  auto res = makeAttributes();
  REQUIRE_OK(t.run("Track.AddAudio", [](IAttributes* a) {
    a->setString("name", "Keys");
    a->setInt("channels", 2);
  }, res.get()));
  sq_guid track{};
  res->getGuid("node", &track);
  t.exec(doc::kAddClip, [&](IAttributes* p) {
    p->setGuid("track", &track);
    p->setInt("kind", SQ_CLIP_AUDIO);
    p->setDouble("start", 0);
    p->setDouble("length", 4);
    p->setString("source", wav.c_str());
  });
  std::string dry = joinPath(t.dir, "dry.wav"), wet = joinPath(t.dir, "wet.wav");
  REQUIRE_OK(t.run("File.ExportAudio", [&](IAttributes* a) { a->setString("path", dry.c_str()); }));
  // Gain (param 1) is in plain dB; -6.0206 dB = x0.5. The plugin adds 64 frames of latency.
  REQUIRE_OK(t.run("Track.InsertPlugin", [](IAttributes* a) {
    a->setString("plugin", "vst3:6D2F880BA9134121A830C04A1A7D830E");
    a->setString("params", "1=-6.0205999");
  }));
  CHECK(t.sh.audio()->buildWarnings().empty());
  REQUIRE_OK(t.run("File.ExportAudio", [&](IAttributes* a) { a->setString("path", wet.c_str()); }));
  Audio A, B;
  REQUIRE(readAudio(t.sh, dry, A));
  REQUIRE(readAudio(t.sh, wet, B));
  REQUIRE_EQ(A.frames(), B.frames());
  REQUIRE(A.frames() > 48000);
  double maxErr = 0, peak = 0;
  for (size_t c = 0; c < A.ch.size() && c < B.ch.size(); ++c)
    for (size_t i = 0; i < A.frames(); ++i) {
      maxErr = std::fmax(maxErr, std::fabs(B.ch[c][i] - 0.5 * A.ch[c][i]));
      peak = std::fmax(peak, std::fabs(A.ch[c][i]));
    }
  std::printf("    vst3 render: peak %.3f, max |wet - dry/2| = %.2e\n", peak, maxErr);
  CHECK(peak > 0.1);
  CHECK(maxErr < 1e-4);
}

SQ_TEST("M5/M8: themes, remote pages, control surfaces, OSC -> commands, devices, optional services") {
  TestShell t("m58");
  // Themes: the stock pack (<exe>/themes/sqthemes.sqtheme beside the test; the EXE embeds it).
  auto* themes = t.sh.themes();
  REQUIRE(themes);
  if (!themes->hasTheme("dark")) SQ_SKIP("stock theme pack not built (tools/skin-compiler filtered out)");
  CHECK(themes->hasTheme("light"));
  CHECK_EQ(themes->activeTheme(), std::string("dark"));
  Ptr<IThemeResolver> svc;
  REQUIRE_OK(t.sh.host.queryService(&IThemeResolver::iid, svc.putVoid()));
  uint32_t dark = 0, light = 0;
  REQUIRE_OK(svc->getColor("Window.Background", &dark));
  REQUIRE_OK(t.run("UI.SetTheme", [](IAttributes* a) { a->setString("name", "light"); }));
  CHECK_EQ(t.sh.settings.get("UI.Theme"), std::string("light"));
  CHECK_EQ(themes->activeTheme(), std::string("light"));
  REQUIRE_OK(svc->getColor("Window.Background", &light));
  CHECK(dark != light);
  CHECK(t.run("UI.SetTheme", [](IAttributes* a) { a->setString("name", "nope"); }) == SQ_E_NOTFOUND);
  // Remote pages from the theme's skin (same skin language as the GUI).
  Ptr<IRemotePageProvider> pages;
  REQUIRE_OK(t.sh.host.queryService(&IRemotePageProvider::iid, pages.putVoid()));
  Ptr<IRemotePageSet> set;
  REQUIRE_OK(pages->loadRemotePages("theme://current/remote/mixer.skin.xml", "object://doc/active", set.put()));
  CHECK(set->getPageCount() >= 1);

  // Control surfaces: hardware-surface owns IControlSurfaceService; no hardware is needed to list.
  auto res = makeAttributes();
  REQUIRE_OK(t.run("Surface.List", {}, res.get()));
  CHECK(res->getInt("count", -1) >= 0);
  CHECK(t.run("Surface.LoadMap", [&](IAttributes* a) { a->setString("path", joinPath(t.dir, "missing.surface.xml").c_str()); }) < 0);

  // Audio devices: every AudioDriver class answers; ASIO says why it has no devices.
  res = makeAttributes();
  REQUIRE_OK(t.run("Audio.ListDevices", {}, res.get()));
  std::printf("    %s\n", t.sh.lastMessage().c_str());
  CHECK(!getAttrString(res.get(), "notes").empty());  // the ASIO status reason
  CHECK(t.run("Audio.SelectDevice", [](IAttributes* a) { a->setString("driver", "bogus"); }) == SQ_E_INVALIDARG);
  REQUIRE_OK(t.run("Audio.SelectDevice", [](IAttributes* a) { a->setString("driver", "null"); }));
  CHECK_EQ(t.sh.settings.get("Devices.Audio.Driver"), std::string("null"));
  CHECK(t.sh.deviceDescription().find("null") != std::string::npos);

  // OSC -> commands on the main thread: an allowed command runs, a file command is refused.
  REQUIRE_OK(t.run("File.New"));
  auto trackCount = [&] {
    sq_document_info di{};
    di.struct_size = sizeof di;
    t.doc()->getDocumentInfo(&di);
    return di.track_count;
  };
  const uint32_t before = trackCount();
  REQUIRE_OK(t.run("Track.AddAudio"));
  REQUIRE_EQ(trackCount(), before + 1);
  res = makeAttributes();
  REQUIRE_OK(t.run("Osc.Listen", [](IAttributes* a) { a->setInt("port", 0); }, res.get()));
  int64_t port = res->getInt("port", 0);
  REQUIRE(port > 0);
  REQUIRE_OK(t.run("Osc.Send", [&](IAttributes* a) {
    a->setInt("port", port);
    a->setString("address", "/sq/File/Close");
  }));
  REQUIRE_OK(t.run("Osc.Send", [&](IAttributes* a) {
    a->setInt("port", port);
    a->setString("address", "/sq/Edit/Undo");
  }));
  for (int i = 0; i < 300 && trackCount() != before; ++i) {
    sq_thread_sleep_ms(10);
    t.sh.idle();
  }
  CHECK_EQ(trackCount(), before);  // undo arrived over UDP and ran through the command system
  REQUIRE(t.doc());            // File.Close was not allowed from the network
  REQUIRE_OK(t.run("Osc.Stop"));

  // Optional services answer when loaded (all are in an unfiltered build).
  CHECK_OK(t.run("Account.Status"));
  res = makeAttributes();
  CHECK_OK(t.run("Sync.Link", [](IAttributes* a) { a->setBool("enabled", SQ_TRUE); }, res.get()));
  CHECK(res->getBool("enabled", SQ_FALSE) == SQ_TRUE);
  CHECK_OK(t.run("Sync.Link", [](IAttributes* a) { a->setBool("enabled", SQ_FALSE); }));
  CHECK(t.run("Video.Probe", [&](IAttributes* a) { a->setString("path", joinPath(t.dir, "none.mp4").c_str()); }) < 0);
}

// ------------------------------------------------------------------------------------------------
SQ_TEST("M6: a user script adds a menu command that edits the document with full undo") {
  TestShell t("m6");
  std::string pkg = joinPath(t.dir, "userpkg");
  REQUIRE_OK(writeFile(joinPath(pkg, "package.xml"), R"(<?xml version="1.0" encoding="UTF-8"?>
<ScriptPackage id="user.e2e" name="E2E User Commands" version="1.0.0" abi="1.0">
  <ClassFactory>
    <ScriptClass classID="{187FABB7-E2B0-4B3E-8DD9-9E0EE9DABBEE}" category="Command" name="E2E: Intro"
                 sourceFile="intro.js" functionName="createIntroCommand">
      <Command category="User" name="Add Intro" title="Add Intro Section"/>
    </ScriptClass>
  </ClassFactory>
</ScriptPackage>
)"));
  REQUIRE_OK(writeFile(joinPath(pkg, "intro.js"), R"(
function createIntroCommand() {
  return Host.Commands.handler({
    category: 'User', name: 'Add Intro', title: 'Add Intro Section',
    run(ctx) {
      ctx.doc.execute(Host.Actions.AddMarker, { position: Host.Values.double(0), name: Host.Values.string('Intro') });
      ctx.doc.execute(Host.Actions.AddTrack, { kind: 1, name: Host.Values.string('Intro Pad') });
    },
  });
}
)"));
  REQUIRE_OK(t.run("Script.LoadPackage", [&](IAttributes* a) { a->setString("path", pkg.c_str()); }));
  bool listed = false;
  for (const auto& c : t.sh.commands())
    if (c.category == "User" && c.name == "Add Intro" && c.source == "script") listed = true;
  CHECK(listed);
  REQUIRE_OK(t.run("File.New"));
  auto ed = t.editor();
  const std::string h0 = docHash(t.doc().get());
  const uint32_t u0 = ed->getUndoCount();
  REQUIRE_OK(t.run("User.Add Intro"));
  CHECK_EQ(t.doc()->getMarkerCount(), 1u);
  CHECK_EQ(ed->getUndoCount(), u0 + 1);
  char desc[128];
  uint32_t need = 0;
  REQUIRE_OK(ed->getUndoDescription(0, desc, sizeof desc, &need));
  CHECK_EQ(std::string(desc), std::string("Add Intro Section"));
  const std::string h1 = docHash(t.doc().get());
  CHECK(h1.find("Intro Pad") != std::string::npos);
  REQUIRE_OK(t.run("Edit.Undo"));
  CHECK_EQ(docHash(t.doc().get()), h0);
  REQUIRE_OK(t.run("Edit.Redo"));
  CHECK_EQ(docHash(t.doc().get()), h1);

  // A bundled package command against the real engine (property names must match engine/doc).
  auto res = makeAttributes();
  REQUIRE_OK(t.run("Track.AddAudio", {}, res.get()));
  sq_guid tr{};
  res->getGuid("node", &tr);
  sq_guid clip = t.exec(doc::kAddClip, [&](IAttributes* p) {
    p->setGuid("track", &tr);
    p->setInt("kind", SQ_CLIP_AUDIO);
    p->setDouble("start", 0);
    p->setDouble("length", 4);
    p->setString("source", joinPath(t.dir, "none.wav").c_str());
  });
  std::string sel = toString(clip);
  REQUIRE_OK(t.run("Edit.Toggle Clip Mute", [&](IAttributes* a) { a->setString("selection", sel.c_str()); }));
  Ptr<INode> n;
  REQUIRE_OK(t.doc()->findNode(&clip, n.put()));
  sq_clip_info ci{};
  ci.struct_size = sizeof ci;
  REQUIRE_OK(n.as<IClip>()->getClipInfo(&ci));
  CHECK((ci.flags & SQ_CLIP_MUTED) != 0);
  // Script evaluation through the command system sees the same document.
  auto out = makeAttributes();
  REQUIRE_OK(t.run("Script.Evaluate", [](IAttributes* a) { a->setString("source", "Host.Documents.active().markers.length"); }, out.get()));
  CHECK_EQ(out->getDouble("value", -1), 1.0);
}

// ------------------------------------------------------------------------------------------------
SQ_TEST("M7: export WAV + FLAC + MIDI file + DAWproject, read everything back") {
  TestShell t("m7");
  std::string wav = joinPath(t.dir, "src.wav");
  writeWav(t.sh, wav, 48000, 48000 * 4, [](uint32_t i) { return 0.3f * std::sin(static_cast<float>(i) * 0.02f); });
  REQUIRE_OK(t.run("File.New"));
  auto res = makeAttributes();
  REQUIRE_OK(t.run("Track.AddAudio", [](IAttributes* a) { a->setString("name", "Audio"); }, res.get()));
  sq_guid at{};
  res->getGuid("node", &at);
  t.exec(doc::kAddClip, [&](IAttributes* p) {
    p->setGuid("track", &at);
    p->setInt("kind", SQ_CLIP_AUDIO);
    p->setDouble("start", 0);
    p->setDouble("length", 8);
    p->setString("source", wav.c_str());
  });
  REQUIRE_OK(t.run("Track.AddInstrument", [](IAttributes* a) { a->setString("name", "Lead"); }, res.get()));
  sq_guid it{};
  res->getGuid("node", &it);
  sq_guid mc = t.exec(doc::kAddClip, [&](IAttributes* p) {
    p->setGuid("track", &it);
    p->setInt("kind", SQ_CLIP_MIDI);
    p->setDouble("start", 2);
    p->setDouble("length", 4);
  });
  std::vector<sq_clip_event> ev;
  for (int i = 0; i < 8; ++i) {
    sq_clip_event e{};
    e.struct_size = sizeof e;
    e.position = i * 0.5;
    e.length = 0.25;
    e.bytes[0] = 0x90;
    e.bytes[1] = static_cast<uint8_t>(60 + (i % 5));
    e.bytes[2] = 90;
    e.size = 3;
    ev.push_back(e);
  }
  t.exec(doc::kSetClipEvents, [&](IAttributes* p) {
    p->setGuid("clip", &mc);
    p->setBlob("events", ev.data(), static_cast<uint32_t>(ev.size() * sizeof ev[0]));
  });
  t.exec(doc::kAddMarker, [](IAttributes* p) {
    p->setDouble("position", 4);
    p->setString("name", "B");
  });
  const std::string h = docHash(t.doc().get());

  std::string outWav = joinPath(t.dir, "mix.wav"), outFlac = joinPath(t.dir, "mix.flac");
  REQUIRE_OK(t.run("File.ExportAudio", [&](IAttributes* a) { a->setString("path", outWav.c_str()); }));
  REQUIRE_OK(t.run("File.ExportAudio", [&](IAttributes* a) { a->setString("path", outFlac.c_str()); }));
  Audio W, F;
  REQUIRE(readAudio(t.sh, outWav, W));
  REQUIRE(readAudio(t.sh, outFlac, F));
  REQUIRE_EQ(W.frames(), F.frames());
  CHECK_EQ(W.frames(), size_t(48000 * 4));  // 8 beats at 120 bpm
  CHECK_EQ(W.ch.size(), size_t(2));
  double diff = 0, peak = 0;
  for (size_t c = 0; c < W.ch.size(); ++c)
    for (size_t i = 0; i < W.frames(); ++i) {
      diff = std::fmax(diff, std::fabs(W.ch[c][i] - F.ch[c][i]));
      peak = std::fmax(peak, std::fabs(W.ch[c][i]));
    }
  std::printf("    mixdown peak %.3f, |wav - flac| max %.2e\n", peak, diff);
  CHECK(peak > 0.1);
  CHECK(diff < 1e-5);  // FLAC stores 24-bit: lossless up to the quantisation step

  std::string mid = joinPath(t.dir, "song.mid");
  auto mres = makeAttributes();
  REQUIRE_OK(t.run("File.ExportMidi", [&](IAttributes* a) { a->setString("path", mid.c_str()); }, mres.get()));
  CHECK_EQ(mres->getInt("notes", 0), 8);
  auto rd = createInstance<IMidiFileReader>(&t.sh.registry, midi::kMidiFileReaderClassId);
  REQUIRE(rd);
  REQUIRE_OK(rd->open(mid.c_str()));
  sq_midi_file_info mi{};
  mi.struct_size = sizeof mi;
  REQUIRE_OK(rd->getInfo(&mi));
  CHECK_EQ(mi.format, 1u);
  CHECK_EQ(mi.track_count, 2u);
  int noteOns = 0;
  uint64_t firstTick = UINT64_MAX;
  for (uint32_t i = 0; i < rd->getEventCount(1); ++i) {
    sq_midi_file_event e{};
    e.struct_size = sizeof e;
    rd->getEvent(1, i, &e);
    if ((e.status & 0xF0) == 0x90 && e.data2 > 0) {
      ++noteOns;
      firstTick = std::min<uint64_t>(firstTick, e.tick);
    }
  }
  CHECK_EQ(noteOns, 8);
  CHECK_EQ(firstTick, uint64_t(2 * mi.ticks_per_quarter));
  char tn[64];
  uint32_t need = 0;
  REQUIRE_OK(rd->getTrackName(1, tn, sizeof tn, &need));
  CHECK_EQ(std::string(tn), std::string("Lead"));

  std::string dp = joinPath(t.dir, "song.dawproject");
  REQUIRE_OK(t.run("File.ExportDawProject", [&](IAttributes* a) { a->setString("path", dp.c_str()); }));
  auto ires = makeAttributes();
  REQUIRE_OK(t.run("File.ImportDawProject", [&](IAttributes* a) { a->setString("path", dp.c_str()); }, ires.get()));
  CHECK_EQ(ires->getInt("tracks", 0), 2);
  auto back = t.doc();
  // Same content except the media path (extracted next to the project); compare without sources.
  std::string hb = docHash(back.get());
  CHECK_EQ(hb, h);
  // The reimported song renders the same audio.
  std::string outWav2 = joinPath(t.dir, "mix2.wav");
  REQUIRE_OK(t.run("File.ExportAudio", [&](IAttributes* a) { a->setString("path", outWav2.c_str()); }));
  Audio W2;
  REQUIRE(readAudio(t.sh, outWav2, W2));
  REQUIRE_EQ(W2.frames(), W.frames());
  double d2 = 0;
  for (size_t c = 0; c < W.ch.size(); ++c)
    for (size_t i = 0; i < W.frames(); ++i) d2 = std::fmax(d2, std::fabs(W.ch[c][i] - W2.ch[c][i]));
  CHECK(d2 < 1e-6);
}

// ------------------------------------------------------------------------------------------------
SQ_TEST("M7: import audio (44.1 kHz into a 48 kHz song) + MIDI, mixer commands, save/reopen") {
  TestShell t("imports");
  REQUIRE_OK(t.run("File.New"));
  auto tr = makeAttributes();
  REQUIRE_OK(t.run("Track.AddAudio", [](IAttributes* a) { a->setString("name", "Imported"); }, tr.get()));
  sq_guid track{};
  tr->getGuid("node", &track);

  const std::string wav = joinPath(t.dir, "tone-44k.wav");
  writeWav(t.sh, wav, 44100, 44100 * 2, [](uint32_t i) {
    return 0.5f * std::sin(2.0f * 3.14159265358979323846f * 220.0f * static_cast<float>(i) / 44100.0f);
  });

  auto imported = makeAttributes();
  REQUIRE_OK(t.run("File.ImportAudio", [&](IAttributes* a) {
    a->setString("path", wav.c_str());
    a->setDouble("start", 0);
  }, imported.get()));
  sq_guid clip{};
  imported->getGuid("node", &clip);
  CHECK(!sq_guid_is_null(&clip));
  CHECK_EQ(imported->getInt("sample_rate", 0), int64_t(44100));
  auto ed = t.editor();
  const uint32_t undoAfterImport = ed->getUndoCount();

  auto d = t.doc();
  Ptr<ITrack> importedTrack;
  REQUIRE_OK(d->findNode(&track, reinterpret_cast<INode**>(importedTrack.put())));
  sq_track_info ti{};
  ti.struct_size = sizeof ti;
  REQUIRE_OK(importedTrack->getTrackInfo(&ti));
  REQUIRE_EQ(ti.clip_count, 1u);
  Ptr<IClip> c;
  REQUIRE_OK(importedTrack->getClip(0, c.put()));
  sq_clip_info ci{};
  ci.struct_size = sizeof ci;
  REQUIRE_OK(c->getClipInfo(&ci));
  CHECK_EQ(clipSource(c.get()), wav);
  Ptr<ITempoMap> tm;
  REQUIRE_OK(d->getTempoMap(tm.put()));
  CHECK(std::fabs(ci.length - tm->secondsToPpq(2.0)) < 1e-6);
  CHECK_EQ(ci.kind, int32_t(SQ_CLIP_AUDIO));

  // One undo step for the import; undo/redo are exact.
  REQUIRE_OK(t.run("Edit.Undo"));
  CHECK_EQ(ed->getUndoCount(), undoAfterImport - 1);
  REQUIRE_OK(importedTrack->getTrackInfo(&ti));
  CHECK_EQ(ti.clip_count, 0u);
  REQUIRE_OK(t.run("Edit.Redo"));
  REQUIRE_OK(importedTrack->getTrackInfo(&ti));
  CHECK_EQ(ti.clip_count, 1u);

  // Render the imported 44.1 kHz material through the 48 kHz graph.
  const std::string mix = joinPath(t.dir, "mix.wav");
  REQUIRE_OK(t.run("File.ExportAudio", [&](IAttributes* a) { a->setString("path", mix.c_str()); }));
  Audio rendered;
  REQUIRE(readAudio(t.sh, mix, rendered));
  CHECK_EQ(rendered.sr, 48000.0);
  CHECK(std::llabs(static_cast<long long>(rendered.frames()) - 96000) <= 64);
  double rms = 0;
  for (float v : rendered.ch[0]) rms += static_cast<double>(v) * v;
  rms = std::sqrt(rms / static_cast<double>(rendered.frames()));
  CHECK(rms > 0.2);  // a full-scale 0.5 sine resampled without losing energy

  // Save/reopen keeps the source and the musical length.
  const std::string song = joinPath(t.dir, "imports.song");
  REQUIRE_OK(t.run("File.Save", [&](IAttributes* a) { a->setString("path", song.c_str()); }));
  REQUIRE_OK(t.run("File.Close"));
  REQUIRE_OK(t.run("File.Open", [&](IAttributes* a) { a->setString("path", song.c_str()); }));
  d = t.doc();
  REQUIRE(d);
  REQUIRE_OK(d->findNode(&track, reinterpret_cast<INode**>(importedTrack.put())));
  REQUIRE_OK(importedTrack->getTrackInfo(&ti));
  REQUIRE_EQ(ti.clip_count, 1u);
  REQUIRE_OK(importedTrack->getClip(0, c.put()));
  REQUIRE_OK(c->getClipInfo(&ci));
  CHECK_EQ(clipSource(c.get()), wav);
  CHECK(std::fabs(ci.length - tm->secondsToPpq(2.0)) < 1e-6);

  // Mixer commands: volume, pan, mute, solo, rename (each one undo step).
  auto mixTr = makeAttributes();
  REQUIRE_OK(t.run("Track.AddAudio", [](IAttributes* a) { a->setString("name", "Mix"); }, mixTr.get()));
  sq_guid mixTrack{};
  mixTr->getGuid("node", &mixTrack);
  auto runOn = [&](const char* cmd, std::function<void(IAttributes*)> args) {
    return t.run(cmd, [&](IAttributes* a) {
      a->setGuid("track", &mixTrack);
      if (args) args(a);
    });
  };
  CHECK_EQ(runOn("Track.SetVolume", [](IAttributes* a) { a->setDouble("value", 100.0); }), SQ_E_INVALIDARG);
  REQUIRE_OK(runOn("Track.SetVolume", [](IAttributes* a) { a->setDouble("value", -6.0); }));
  REQUIRE_OK(runOn("Track.SetPan", [](IAttributes* a) { a->setDouble("value", 0.5); }));
  REQUIRE_OK(runOn("Track.ToggleMute", {}));
  REQUIRE_OK(runOn("Track.ToggleSolo", {}));
  REQUIRE_OK(runOn("Track.Rename", [](IAttributes* a) { a->setString("name", "MIX"); }));
  Ptr<ITrack> mt;
  REQUIRE_OK(d->findNode(&mixTrack, reinterpret_cast<INode**>(mt.put())));
  sq_track_info mi{};
  mi.struct_size = sizeof mi;
  REQUIRE_OK(mt->getTrackInfo(&mi));
  CHECK_EQ(name(mt.get()), std::string("MIX"));
  CHECK(std::fabs(mi.volume_db + 6.0) < 1e-9);
  CHECK(std::fabs(mi.pan - 0.5) < 1e-9);
  CHECK(mi.flags & SQ_TRACK_MUTE);
  CHECK(mi.flags & SQ_TRACK_SOLO);
  REQUIRE_OK(runOn("Track.ToggleMute", {}));
  REQUIRE_OK(mt->getTrackInfo(&mi));
  CHECK(!(mi.flags & SQ_TRACK_MUTE));

  // MIDI file import: one instrument track with the file's notes.
  const std::string mid = joinPath(t.dir, "notes.mid");
  auto writer = createInstance<IMidiFileWriter>(&t.sh.registry, midi::kMidiFileWriterClassId);
  REQUIRE(writer);
  REQUIRE_OK(writer->create(mid.c_str(), 1, 480));
  REQUIRE_OK(writer->beginTrack());
  sq_midi_file_event ev{};
  ev.struct_size = sizeof ev;
  ev.tick = 0;
  ev.status = 0x90;
  ev.data1 = 60;
  ev.data2 = 100;
  REQUIRE_OK(writer->addEvent(&ev));
  ev.tick = 480;
  ev.status = 0x80;
  ev.data2 = 0;
  REQUIRE_OK(writer->addEvent(&ev));
  ev.tick = 480;
  ev.status = 0x90;
  ev.data1 = 67;
  ev.data2 = 90;
  REQUIRE_OK(writer->addEvent(&ev));
  ev.tick = 960;
  ev.status = 0x80;
  ev.data2 = 0;
  REQUIRE_OK(writer->addEvent(&ev));
  REQUIRE_OK(writer->endTrack());
  REQUIRE_OK(writer->finish());
  writer = nullptr;

  sq_document_info before{};
  before.struct_size = sizeof before;
  REQUIRE_OK(d->getDocumentInfo(&before));
  auto midiRes = makeAttributes();
  REQUIRE_OK(t.run("File.ImportMidi", [&](IAttributes* a) { a->setString("path", mid.c_str()); }, midiRes.get()));
  CHECK(midiRes->getInt("tracks", 0) >= 1);
  sq_document_info after{};
  after.struct_size = sizeof after;
  REQUIRE_OK(d->getDocumentInfo(&after));
  CHECK_EQ(after.track_count, before.track_count + 1);
  Ptr<ITrack> last;
  REQUIRE_OK(d->getTrack(after.track_count - 1, last.put()));
  sq_track_info li{};
  li.struct_size = sizeof li;
  REQUIRE_OK(last->getTrackInfo(&li));
  CHECK_EQ(li.kind, int32_t(SQ_TRACK_INSTRUMENT));
  REQUIRE_EQ(li.clip_count, 1u);
  Ptr<IClip> mc;
  REQUIRE_OK(last->getClip(0, mc.put()));
  sq_clip_info mci{};
  mci.struct_size = sizeof mci;
  REQUIRE_OK(mc->getClipInfo(&mci));
  CHECK_EQ(mci.kind, int32_t(SQ_CLIP_MIDI));
  CHECK_EQ(mci.event_count, 2u);
  sq_clip_event ce{};
  ce.struct_size = sizeof ce;
  REQUIRE_OK(mc->getEvent(0, &ce));
  CHECK_EQ(ce.position, 0.0);
  CHECK_EQ(ce.length, 1.0);
  CHECK_EQ(int(ce.bytes[0]), 0x90);
  CHECK_EQ(int(ce.bytes[1]), 60);
}
