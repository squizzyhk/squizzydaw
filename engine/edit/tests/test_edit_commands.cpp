// engine/edit - every command: enabled/disabled checks, one undo step named after the command,
// undo/redo exact by state hash (selection included), navigation never creates an undo step,
// class registry + dispatcher access.
#include <cstring>
#include <functional>
#include <map>

#include "edit_test_util.hpp"

using namespace edittest;
namespace ed = sq::edit;

namespace {

struct Doc : Fx {
  sq_guid A1{}, A2{}, M{}, a1{}, a2{}, a3{}, m1{}, m2{};
  Doc() {
    FakeMedia w1{1000.0, std::vector<float>(10000, 0.0f)};
    for (int i = 300; i < 900; ++i) w1.samples[i] = 0.5f;
    for (int i = 2500; i < 3200; ++i) w1.samples[i] = -0.25f;
    media()["w1.fake"] = w1;
    media()["w2.fake"] = FakeMedia{1000.0, std::vector<float>(10000, 0.1f)};
    A1 = addTrack(*this, SQ_TRACK_AUDIO, "A1");
    A2 = addTrack(*this, SQ_TRACK_AUDIO, "A2");
    M = addTrack(*this, SQ_TRACK_INSTRUMENT, "M");
    a1 = addClip(*this, A1, SQ_CLIP_AUDIO, 0, 4, "w1.fake");
    a2 = addClip(*this, A1, SQ_CLIP_AUDIO, 4, 4, "w1.fake", 2.0);
    a3 = addClip(*this, A2, SQ_CLIP_AUDIO, 2, 4, "w2.fake");
    m1 = addClip(*this, M, SQ_CLIP_MIDI, 0, 4);
    m2 = addClip(*this, M, SQ_CLIP_MIDI, 4, 4);
    CHECK_OK(setEvents(*this, m1, {note(0.1, 0.4, 60), note(1.2, 0.5, 64), note(2.9, 0.3, 67)}));
    CHECK_OK(setEvents(*this, m2, {note(0.0, 1.0, 48)}));
    auto mp = sq::makeAttributes();
    mp->setDouble("position", 6.0);
    CHECK_OK(exec(*this, sq::doc::kAddMarker, mp.get()));
    std::vector<double> pts{1.0, 0.5, 6.0, 0.8};
    auto ap = sq::makeAttributes();
    ap->setGuid("track", &A1);
    ap->setString("param", "volume");
    ap->setBlob("points", pts.data(), static_cast<uint32_t>(pts.size() * sizeof(double)));
    CHECK_OK(exec(*this, sq::edoc::kSetAutomation, ap.get()));
  }
  void state(std::vector<sq_guid> sel, double cursor, double rf = -1, double rt = -1, const sq_guid* track = nullptr) {
    ed::setSelection(*impl, sel);
    ed::setCursor(*impl, cursor);
    ed::setRange(*impl, rf, rt);
    ed::setFocusTrack(*impl, track ? *track : sq_guid{});
  }
};

struct Setup {
  std::function<void(Doc&)> prepare;
  std::function<void(Doc&, sq::IAttributes*)> args;  // optional
};

std::map<std::string, Setup> setups() {
  std::map<std::string, Setup> s;
  auto sel = [](std::function<std::vector<sq_guid>(Doc&)> f, double cursor, double rf = -1, double rt = -1) {
    return Setup{[=](Doc& d) { d.state(f(d), cursor, rf, rt); }, nullptr};
  };
  s["Split"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a1, d.m1}; }, 2.0);
  s["TrimStartToCursor"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a2}; }, 5.0);
  s["TrimEndToCursor"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a1}; }, 3.0);
  s["SlipLeft"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a2}; }, 0.0);
  s["SlipRight"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a2, d.m2}; }, 0.0);
  s["NudgeLeft"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a3}; }, 0.0);
  s["NudgeRight"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a3}; }, 0.0);
  s["MoveToCursor"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a3}; }, 10.0);
  s["MoveToTrackAbove"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a3}; }, 0.0);
  s["MoveToTrackBelow"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a1}; }, 0.0);
  s["FadeInToCursor"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a1}; }, 1.0);
  s["FadeOutFromCursor"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a1}; }, 3.0);
  s["Crossfade"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a1, d.a2}; }, 0.0);
  s["Quantize"] = sel([](Doc& d) { return std::vector<sq_guid>{d.m1}; }, 0.0);
  s["StripSilence"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a1}; }, 0.0);
  s["SilenceRange"] = sel([](Doc&) { return std::vector<sq_guid>{}; }, 0.0, 1.0, 3.0);
  s["InsertTime"] = sel([](Doc&) { return std::vector<sq_guid>{}; }, 2.0);
  s["DeleteTime"] = sel([](Doc&) { return std::vector<sq_guid>{}; }, 0.0, 1.0, 3.0);
  s["Duplicate"] = sel([](Doc& d) { return std::vector<sq_guid>{d.m1}; }, 0.0);
  s["Delete"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a3}; }, 0.0);
  s["DeleteRipple"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a1}; }, 0.0);
  s["DeleteRippleAll"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a1}; }, 0.0);
  s["Glue"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a1, d.a2}; }, 0.0);
  s["Normalize"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a1, d.a3}; }, 0.0);
  s["GainUp"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a1}; }, 0.0);
  s["GainDown"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a1}; }, 0.0);
  s["FitTempoToRange"] = Setup{[](Doc& d) { d.state({}, 0.0, 0.0, 4.0); }, [](Doc&, sq::IAttributes* a) { a->setDouble("seconds", 1.0); }};
  // navigation
  s["SelectNextClip"] = Setup{[](Doc& d) { d.state({d.a1}, 0.0, -1, -1, &d.A1); }, nullptr};
  s["SelectPreviousClip"] = Setup{[](Doc& d) { d.state({d.a2}, 4.0, -1, -1, &d.A1); }, nullptr};
  s["SelectAll"] = sel([](Doc&) { return std::vector<sq_guid>{}; }, 0.0);
  s["SelectNone"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a1}; }, 0.0);
  s["CursorNextGrid"] = sel([](Doc&) { return std::vector<sq_guid>{}; }, 0.5);
  s["CursorPreviousGrid"] = sel([](Doc&) { return std::vector<sq_guid>{}; }, 2.0);
  s["CursorToSelection"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a2}; }, 0.0);
  s["NextTrack"] = sel([](Doc&) { return std::vector<sq_guid>{}; }, 0.0);
  s["PreviousTrack"] = Setup{[](Doc& d) { d.state({}, 0.0, -1, -1, &d.A2); }, nullptr};
  s["RangeFromSelection"] = sel([](Doc& d) { return std::vector<sq_guid>{d.a1, d.a3}; }, 0.0);
  s["SetEditState"] = Setup{[](Doc& d) { d.state({}, 0.0); }, [](Doc& d, sq::IAttributes* a) {
                              a->setDouble("cursor", 3.0);
                              a->setDouble("grid", 0.5);
                              a->setString("snap", "bar");
                              a->setGuid("track", &d.M);
                            }};
  return s;
}

std::string undoTop(sq::IEditor* e) {
  char buf[256] = {};
  uint32_t need = 0;
  if (e->getUndoDescription(0, buf, sizeof buf, &need) != SQ_OK) return {};
  return buf;
}

}  // namespace

SQ_TEST("commands: every command is covered here and has a unique id and name") {
  const auto s = setups();
  const auto& t = ed::commandTable();
  CHECK_EQ(t.size(), s.size());
  for (size_t i = 0; i < t.size(); ++i) {
    CHECK(s.count(t[i].name) == 1);
    for (size_t j = i + 1; j < t.size(); ++j) {
      CHECK(!(t[i].id == t[j].id));
      CHECK(std::strcmp(t[i].name, t[j].name) != 0);
    }
    CHECK(ed::findCommand(t[i].id) == &t[i]);
    CHECK(ed::findCommand(t[i].category, t[i].name) == &t[i]);
  }
}

SQ_TEST("commands: each editing command is one undo step, undo/redo exact by state hash") {
  sq::app::ClassRegistry reg;
  registerFakeCodec(reg);
  ed::setMediaRegistry(&reg);
  const auto s = setups();
  for (const ed::CommandInfo& c : ed::commandTable()) {
    Doc d;
    s.at(c.name).prepare(d);
    auto args = sq::makeAttributes();
    if (s.at(c.name).args) s.at(c.name).args(d, args.get());
    const std::string before = d.hash();
    const uint32_t undo0 = d.ed->getUndoCount();
    // checkOnly: enabled, and nothing changes
    const sq_result chk = ed::runCommand(d.doc.get(), c.id, args.get(), true);
    if (chk != SQ_OK) sqtest::reportFailure(__FILE__, __LINE__, std::string("not enabled: ") + c.name);
    CHECK_EQ(d.hash(), before);
    const sq_result r = ed::runCommand(d.doc.get(), c.id, args.get(), false);
    if (r != SQ_OK) sqtest::reportFailure(__FILE__, __LINE__, std::string("failed: ") + c.name + " " + std::to_string(r));
    const std::string after = d.hash();
    if (after == before) sqtest::reportFailure(__FILE__, __LINE__, std::string("no effect: ") + c.name);
    if (!c.editsDocument) {
      CHECK_EQ(d.ed->getUndoCount(), undo0);  // journal only
      continue;
    }
    if (d.ed->getUndoCount() != undo0 + 1) sqtest::reportFailure(__FILE__, __LINE__, std::string("undo steps: ") + c.name);
    CHECK_EQ(undoTop(d.ed.get()), std::string(c.title));
    CHECK_OK(d.ed->undo());
    if (d.hash() != before) sqtest::reportFailure(__FILE__, __LINE__, std::string("undo not exact: ") + c.name);
    CHECK_OK(d.ed->redo());
    if (d.hash() != after) sqtest::reportFailure(__FILE__, __LINE__, std::string("redo not exact: ") + c.name);
    // and once more round trip
    CHECK_OK(d.ed->undo());
    CHECK_EQ(d.hash(), before);
  }
  ed::setMediaRegistry(nullptr);
}

SQ_TEST("commands: with nothing selected / no range they are disabled and change nothing") {
  for (const ed::CommandInfo& c : ed::commandTable()) {
    if (std::strcmp(c.name, "Split") == 0 || std::strcmp(c.name, "InsertTime") == 0 || std::strcmp(c.name, "SelectAll") == 0 ||
        std::strcmp(c.name, "SelectNextClip") == 0 || std::strcmp(c.name, "SelectPreviousClip") == 0 ||
        std::strcmp(c.name, "CursorNextGrid") == 0 || std::strcmp(c.name, "NextTrack") == 0 ||
        std::strcmp(c.name, "PreviousTrack") == 0)
      continue;  // these act on the cursor / whole document
    Doc d;
    d.state({}, 0.0);
    const std::string h = d.hash();
    if (ed::runCommand(d.doc.get(), c.id, nullptr, true) != SQ_COMMAND_DISABLED)
      sqtest::reportFailure(__FILE__, __LINE__, std::string("should be disabled: ") + c.name);
    const sq_result r = ed::runCommand(d.doc.get(), c.id, nullptr, false);
    CHECK(r == SQ_COMMAND_DISABLED || r == SQ_E_INVALIDARG);
    CHECK_EQ(d.hash(), h);
  }
}

SQ_TEST("commands: locked clips are skipped; a failing action rolls the whole command back") {
  Doc d;
  CHECK_OK(setPropB(d, d.a1, "locked", true));
  d.state({d.a1}, 2.0);
  CHECK_EQ(ed::runCommand(d.doc.get(), ed::cmd::kSplit, nullptr, true), SQ_COMMAND_DISABLED);
  // FadeIn on two clips where the second one fails mid-command: nothing remains
  d.state({d.a2, d.a3}, 5.0);
  const std::string h = d.hash();
  const uint32_t n = d.ed->getUndoCount();
  auto args = sq::makeAttributes();
  args->setString("in_curve", "not-a-curve");
  CHECK_EQ(ed::runCommand(d.doc.get(), ed::cmd::kFadeInToCursor, args.get(), false), SQ_E_INVALIDARG);
  CHECK_EQ(d.hash(), h);
  CHECK_EQ(d.ed->getUndoCount(), n);
}

SQ_TEST("commands: selection rides along with undo (split then undo restores the selection)") {
  Doc d;
  d.state({d.a1}, 2.0);
  const std::string sel0 = d.impl->journal("edit.selection");
  REQUIRE_OK(ed::runCommand(d.doc.get(), ed::cmd::kSplit, nullptr, false));
  auto st = ed::readEditState(*d.impl);
  REQUIRE_EQ(st.selection.size(), size_t(1));
  CHECK(!(st.selection[0] == d.a1));  // the right part
  CHECK_OK(d.ed->undo());
  CHECK_EQ(d.impl->journal("edit.selection"), sel0);
}

SQ_TEST("commands: class registry, dispatcher by name, explicit document argument") {
  sq::app::ClassRegistry reg;
  Doc d;
  REQUIRE_OK(ed::registerEditClasses(&reg, d.mgr.get()));
  d.state({d.a3}, 0.0);
  // per-command class, active document of the manager
  Ptr<sq::ICommandHandler> h;
  REQUIRE_OK(reg.createInstance(&ed::cmd::kNudgeRight, &sq::ICommandHandler::iid, h.putVoid()));
  CHECK_OK(h->interpretCommand(nullptr, nullptr, SQ_TRUE));
  CHECK_OK(h->interpretCommand(nullptr, nullptr, SQ_FALSE));
  CHECK(near(d.clip(d.a3).start, 3.0));
  // dispatcher: by category/name, document passed explicitly as an object
  Ptr<sq::ICommandHandler> disp;
  REQUIRE_OK(reg.createInstance(&ed::cmd::kDispatcher, &sq::ICommandHandler::iid, disp.putVoid()));
  sq_command c{};
  c.struct_size = sizeof c;
  c.category = "Edit";
  c.name = "NudgeLeft";
  auto args = sq::makeAttributes();
  args->setObject("document", d.doc.get());
  CHECK_OK(disp->interpretCommand(&c, args.get(), SQ_FALSE));
  CHECK(near(d.clip(d.a3).start, 2.0));
  c.name = "NoSuchCommand";
  CHECK_EQ(disp->interpretCommand(&c, args.get(), SQ_TRUE), SQ_E_NOTFOUND);
  // by document id
  sq_guid docId{};
  d.doc->getId(&docId);
  auto byId = sq::makeAttributes();
  byId->setGuid("doc", &docId);
  c.category = "Navigate";
  c.name = "SelectNone";
  CHECK_OK(disp->interpretCommand(&c, byId.get(), SQ_FALSE));
  CHECK(ed::readEditState(*d.impl).selection.empty());
  auto ghost = sq::makeAttributes();
  const sq_guid g = sq::edoc::newGuid();
  ghost->setGuid("doc", &g);
  CHECK_EQ(disp->interpretCommand(&c, ghost.get(), SQ_FALSE), SQ_E_NOTFOUND);
  CHECK_EQ(disp->interpretCommand(&c, ghost.get(), SQ_TRUE), SQ_COMMAND_DISABLED);
  ed::unregisterEditClasses(&reg);
}

SQ_TEST("edit state: journals parse defensively") {
  Doc d;
  d.impl->setJournal("edit.cursor", "nan");
  d.impl->setJournal("edit.grid", "-1");
  d.impl->setJournal("edit.snap", "warp");
  d.impl->setJournal("edit.selection", "{garbage} " + sq::toString(d.a1) + " " + sq::toString(sq::edoc::newGuid()));
  d.impl->setJournal("edit.range", "5 2");
  d.impl->setJournal("edit.track", "nope");
  const ed::EditState st = ed::readEditState(*d.impl);
  CHECK_EQ(st.cursor, 0.0);
  CHECK_EQ(st.grid, 1.0);
  CHECK_EQ(st.snap, std::string("grid"));
  REQUIRE_EQ(st.selection.size(), size_t(1));
  CHECK(st.selection[0] == d.a1);
  CHECK(!st.hasRange);
  CHECK(sq_guid_is_null(&st.track) != 0);
  auto bad = sq::makeAttributes();
  bad->setDouble("grid", -2.0);
  CHECK_EQ(ed::runCommand(d.doc.get(), ed::cmd::kSetEditState, bad.get(), false), SQ_E_INVALIDARG);
}
