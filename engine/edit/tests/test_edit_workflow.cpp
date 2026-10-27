// engine/edit - M3 exit criterion: a full editing workflow driven only by commands (a key map
// resolved through the class registry's command dispatcher), no mouse paths. Every editing
// command is one undo step; undoing all of them restores the model exactly, redoing all of them
// reproduces it exactly, and the result survives an archive round trip.
#include <map>

#include "edit_test_util.hpp"
#include "sq/engine/doc/archive.hpp"
#include "sq/platform.hpp"

using namespace edittest;
namespace ed = sq::edit;

namespace {

// The model without UI journals (navigation only changes journals and is not undoable).
std::string modelHash(const Fx& fx) {
  std::string all;
  for (const auto& [name, xml] : sq::edoc::serializeParts(fx.st())) {
    if (name == "uistate.xml") continue;
    all += name + "\n" + xml;
  }
  return sq::sha256Hex(all);
}

struct Key {
  const char* category;
  const char* name;
};
// A key map, as a UI would load it from settings.
const std::map<std::string, Key>& keymap() {
  static const std::map<std::string, Key> k = {
    {"Down", {"Navigate", "NextTrack"}},
    {"Up", {"Navigate", "PreviousTrack"}},
    {"Tab", {"Navigate", "SelectNextClip"}},
    {"Shift+Tab", {"Navigate", "SelectPreviousClip"}},
    {"Ctrl+A", {"Navigate", "SelectAll"}},
    {"Esc", {"Navigate", "SelectNone"}},
    {"Right", {"Navigate", "CursorNextGrid"}},
    {"Left", {"Navigate", "CursorPreviousGrid"}},
    {"R", {"Navigate", "RangeFromSelection"}},
    {"Ctrl+Shift+E", {"Navigate", "SetEditState"}},
    {"S", {"Edit", "Split"}},
    {"Alt+Right", {"Edit", "NudgeRight"}},
    {"Alt+Left", {"Edit", "NudgeLeft"}},
    {"X", {"Edit", "Crossfade"}},
    {"Q", {"Edit", "Quantize"}},
    {"Ctrl+D", {"Edit", "Duplicate"}},
    {"Ctrl+Del", {"Edit", "DeleteTime"}},
    {"Ctrl+I", {"Edit", "InsertTime"}},
    {"Ctrl+Shift+S", {"Edit", "StripSilence"}},
    {"Ctrl+N", {"Edit", "Normalize"}},
    {"Ctrl+T", {"Edit", "FitTempoToRange"}},
    {"-", {"Edit", "GainDown"}},
  };
  return k;
}

struct Session {
  Fx& fx;
  Ptr<sq::ICommandHandler> dispatcher;
  int edits = 0;
  void press(const std::string& key, std::initializer_list<std::pair<const char*, double>> nums = {}) {
    auto it = keymap().find(key);
    REQUIRE(it != keymap().end());
    sq_command c{};
    c.struct_size = sizeof c;
    c.category = it->second.category;
    c.name = it->second.name;
    auto args = sq::makeAttributes();
    for (const auto& [k, v] : nums) args->setDouble(k, v);
    const uint32_t before = fx.ed->getUndoCount();
    const sq_result r = dispatcher->interpretCommand(&c, args.get(), SQ_FALSE);
    if (r != SQ_OK) sqtest::reportFailure(__FILE__, __LINE__, "key " + key + " (" + c.name + ") -> " + std::to_string(r));
    const bool editing = std::string(c.category) == "Edit";
    const uint32_t expect = before + (editing ? 1u : 0u);
    if (fx.ed->getUndoCount() != expect) sqtest::reportFailure(__FILE__, __LINE__, "undo steps after " + key);
    if (editing) ++edits;
  }
};

double durSeconds(const Fx& fx, const ClipRec& c) {
  return fx.st().tempo.ppqToSeconds(c.end()) - fx.st().tempo.ppqToSeconds(c.start);
}

}  // namespace

SQ_TEST("workflow: strip, normalize, split, nudge, crossfade, quantize, duplicate, time edits, tempo fit - keys only") {
  // media
  FakeMedia vox{1000.0, std::vector<float>(8000, 0.001f)};  // -60 dB floor
  for (int i = 500; i < 2000; ++i) vox.samples[i] = 0.6f;
  for (int i = 3000; i < 4500; ++i) vox.samples[i] = 0.3f;
  for (int i = 6000; i < 7500; ++i) vox.samples[i] = -0.8f;
  media()["vox.fake"] = vox;
  media()["gtr.fake"] = FakeMedia{1000.0, std::vector<float>(10000, 0.2f)};
  sq::app::ClassRegistry reg;
  registerFakeCodec(reg);
  ed::setMediaRegistry(&reg);

  // the session as recorded / imported
  Fx fx;
  const sq_guid V = addTrack(fx, SQ_TRACK_AUDIO, "Vox");
  const sq_guid G = addTrack(fx, SQ_TRACK_AUDIO, "Gtr");
  const sq_guid K = addTrack(fx, SQ_TRACK_INSTRUMENT, "Keys");
  addClip(fx, V, SQ_CLIP_AUDIO, 0, 16, "vox.fake");
  const sq_guid gtr = addClip(fx, G, SQ_CLIP_AUDIO, 0, 8, "gtr.fake");
  const sq_guid keys = addClip(fx, K, SQ_CLIP_MIDI, 0, 8);
  CHECK_OK(setEvents(fx, keys, {note(0.1, 0.5, 60), note(1.1, 0.5, 62), note(2.05, 0.5, 64), note(2.9, 0.5, 65)}));
  REQUIRE_OK(ed::registerEditClasses(&reg, fx.mgr.get()));
  Session s{fx, {}, 0};
  REQUIRE_OK(reg.createInstance(&ed::cmd::kDispatcher, &sq::ICommandHandler::iid, s.dispatcher.putVoid()));
  const uint32_t undo0 = fx.ed->getUndoCount();
  const std::string model0 = modelHash(fx);

  // vocals: strip the silence between phrases, normalize every phrase
  s.press("Down");  // focus Vox
  s.press("Tab");   // select the recording
  s.press("Ctrl+Shift+S", {{"threshold_db", -40.0}});
  REQUIRE_EQ(fx.st().clipsOf(V).size(), size_t(3));
  s.press("Ctrl+A");
  s.press("Ctrl+N", {{"target_db", -1.0}});
  for (const sq_guid& id : fx.st().clipsOf(V)) CHECK(fx.clip(id).gainDb > 0.0);
  CHECK(near(fx.clip(fx.st().clipsOf(V)[2]).gainDb, -1.0 - 20.0 * std::log10(0.8), 1e-4));

  // guitar: cut at beat 4, nudge the tail away and back, crossfade the edit
  s.press("Esc");
  s.press("Down");  // Gtr
  s.press("Tab");   // the guitar clip, cursor at 0
  for (int i = 0; i < 4; ++i) s.press("Right");
  s.press("S");
  REQUIRE_EQ(fx.st().clipsOf(G).size(), size_t(2));
  s.press("Alt+Right");
  CHECK(near(fx.clip(fx.st().clipsOf(G)[1]).start, 5.0));
  s.press("Alt+Left");
  s.press("Ctrl+A");
  s.press("X");
  {
    const ClipRec& l = fx.clip(fx.st().clipsOf(G)[0]);
    const ClipRec& r = fx.clip(fx.st().clipsOf(G)[1]);
    CHECK(l.id == gtr);
    CHECK(near(l.end(), 4.05));
    CHECK(near(r.start, 3.95));
    CHECK(near(l.fadeOut, 0.05));
    CHECK(near(r.fadeIn, 0.05));
  }

  // keys: quantize, duplicate, then delete the duplicated bars again as a time edit
  s.press("Down");
  s.press("Shift+Tab");  // the keys clip (before the cursor)
  s.press("Q");
  for (size_t i = 0; i < 4; ++i) CHECK(near(fx.clip(keys).events[i].pos, static_cast<double>(i)));
  s.press("Ctrl+D");
  CHECK_EQ(fx.st().clipsOf(K).size(), size_t(2));
  s.press("R");  // range = the copy, 8..16
  s.press("Ctrl+Del");
  CHECK_EQ(fx.st().clipsOf(K).size(), size_t(1));
  CHECK_EQ(fx.st().clipsOf(V).size(), size_t(2));  // the last phrase lived in 8..16 too

  // open two beats at beat 2 on every track
  for (int i = 0; i < 6; ++i) s.press("Left");  // cursor 8 -> 2
  s.press("Ctrl+I", {{"length", 2.0}});
  CHECK_EQ(fx.st().clipsOf(K).size(), size_t(2));
  CHECK(near(fx.clip(fx.st().clipsOf(K)[1]).start, 4.0));

  // fit the first two bars into 3.2 s: 150 bpm; unstretched audio keeps its length in seconds
  std::vector<double> before;
  for (const sq_guid& id : fx.st().clipsOf(G)) before.push_back(durSeconds(fx, fx.clip(id)));
  s.press("Ctrl+Shift+E", {{"range_from", 0.0}, {"range_to", 8.0}});
  s.press("Ctrl+T", {{"seconds", 3.2}});
  CHECK(near(fx.st().tempo.tempos[0].bpm, 150.0));
  const auto after = fx.st().clipsOf(G);
  REQUIRE_EQ(after.size(), before.size());
  for (size_t i = 0; i < after.size(); ++i) CHECK(near(durSeconds(fx, fx.clip(after[i])), before[i], 1e-9));

  s.press("Ctrl+A");
  s.press("-");
  for (const sq_guid& id : fx.st().clipsOf(K)) CHECK(near(fx.clip(id).gainDb, -1.0));

  // history: every editing key was exactly one step
  CHECK_EQ(s.edits, 12);
  CHECK_EQ(fx.ed->getUndoCount(), undo0 + static_cast<uint32_t>(s.edits));
  const std::string model1 = modelHash(fx);
  for (int i = 0; i < s.edits; ++i) CHECK_OK(fx.ed->undo());
  CHECK_EQ(modelHash(fx), model0);
  for (int i = 0; i < s.edits; ++i) CHECK_OK(fx.ed->redo());
  CHECK_EQ(modelHash(fx), model1);

  // persistence: the archive carries fade curves, gain, envelopes, tempo
  sq::edoc::Store reread;
  REQUIRE_OK(sq::edoc::readArchive(sq::edoc::writeArchive(fx.st()), reread));
  CHECK(sq::edoc::serializeParts(reread) == sq::edoc::serializeParts(fx.st()));

  ed::unregisterEditClasses(&reg);
  ed::setMediaRegistry(nullptr);
}
