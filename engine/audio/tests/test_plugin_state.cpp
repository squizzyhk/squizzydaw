// engine/audio - hosted plugin instances of the live graph (AudioEngine::pluginInstance /
// locatePlugin / pluginSlots) and plugin state carried across graph recompiles
// (AudioEngine::captureStatesInto: lines paired by spec identity, duplicates by position, document
// values win, never across a document switch, kept with a bypassed line, kept for the undo / redo
// that reverses a removal, bounded with the bypassed lines' states kept longest, listed for File.Save
// (keptPluginStates), offline renders too) and values a plugin reported not echoed back to it (also
// through the automation swap), on an engine without a device (graphs are installed directly).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "fakes.hpp"
#include "sq/app/class_registry.hpp"
#include "sq/engine/audio/builtin.hpp"
#include "sq/engine/audio/engine.hpp"
#include "sq/engine/audio/plugin_fx.hpp"
#include "sq/engine/audio/render.hpp"
#include "sqtest.hpp"

using namespace sq::engine::audio;

namespace {

PluginFactory stateFactory(std::string pluginId, int32_t value) {
  return [pluginId, value](std::string*) {
    return sq::Ptr<sq::IPluginInstance>::adopt(new fakes::StatePlugin(pluginId, value));
  };
}
fakes::StatePlugin* fake(const sq::Ptr<sq::IPluginInstance>& p) { return static_cast<fakes::StatePlugin*>(p.get()); }
ProcessorProto fxProto(const std::string& pluginId, int32_t value) {
  return std::make_shared<PluginFx>(stateFactory(pluginId, value), pluginId);
}
ProcessorProto synthProto(const std::string& pluginId, int32_t value) {
  return std::make_shared<PluginSynth>(stateFactory(pluginId, value), pluginId);
}
sq::Ptr<AudioEngine> makeEngine(sq::app::ClassRegistry* reg) {
  return sq::Ptr<AudioEngine>::adopt(new AudioEngine(reg, nullptr));
}

// Every instance the document resolvers created, in creation order.
using InstanceLog = std::shared_ptr<std::vector<sq::Ptr<sq::IPluginInstance>>>;

// "fake|<plugin id>|[|i=v]..." -> PluginFx (or PluginSynth) whose factory creates a StatePlugin
// and applies the spec's values, like the app's resolver does.
InsertResolver fakeResolver(bool synth, InstanceLog log) {
  return [synth, log](const std::string& spec, std::string* err) -> ProcessorProto {
    SpecParts sp;
    if (!parseSpec(spec, &sp) || sp.format != "fake") {
      if (err) *err = "not a fake spec";
      return nullptr;
    }
    PluginFactory factory = [sp, log](std::string*) {
      sq::Ptr<sq::IPluginInstance> inst = sq::Ptr<sq::IPluginInstance>::adopt(new fakes::StatePlugin(sp.id, 0));
      for (const auto& [id, v] : sp.params) inst->setParamValue(id, v);
      if (log) log->push_back(inst);
      return inst;
    };
    if (synth) return std::make_shared<PluginSynth>(factory, sp.id);
    return std::make_shared<PluginFx>(factory, sp.id);
  };
}
sq::Ptr<AudioEngine> makeDocEngine(sq::app::ClassRegistry* reg, InstanceLog log = nullptr) {
  sq::Ptr<AudioEngine> eng = makeEngine(reg);
  eng->setInsertResolver(fakeResolver(false, log));
  eng->setInstrumentResolver(fakeResolver(true, log));
  return eng;
}
sq::Ptr<fakes::MTrack> addTrack(fakes::MDoc& doc, int32_t kind, const char* name, const std::string& inserts) {
  auto t = sq::Ptr<fakes::MTrack>::adopt(new fakes::MTrack());
  t->name = name;
  t->info.kind = kind;
  t->info.channels = 2;
  if (!inserts.empty()) t->strProps["user.inserts"] = inserts;
  doc.tracks.push_back(t);
  return t;
}
// A document edit of one track property (what Track.AddInsert / RemoveInsert / MoveInsert / a
// bypass toggle write), then the change notification; reason "undo" / "redo" for what an undo /
// redo of such an edit writes.
void edit(fakes::MDoc& doc, fakes::MTrack& t, const char* prop, const std::string& value,
          const char* reason = "commit") {
  t.strProps[prop] = value;
  doc.fireChanged(reason);
}
sq::Ptr<fakes::MDoc> newDoc() { return sq::Ptr<fakes::MDoc>::adopt(new fakes::MDoc()); }

}  // namespace

SQ_TEST("plugin_state: pluginInstance addresses an insert by its user.inserts line; a recompile carries its state") {
  sq::app::ClassRegistry reg;
  sq::Ptr<AudioEngine> eng = makeEngine(&reg);
  // No graph yet: every lookup is empty.
  CHECK(!eng->pluginInstance(sq_guid{}, 0));
  CHECK(eng->pluginSlots().empty());
  CHECK(!eng->locatePlugin(nullptr, nullptr, nullptr));

  GraphDesc d;
  TrackDesc t;
  t.id = fakes::newGuid();
  t.name = "fx";
  t.channels = 2;
  t.inserts.push_back(fxProto("fake.fx", 1));
  t.slotToChain = {0};  // line 0 -> chain 0
  d.tracks.push_back(t);
  REQUIRE_OK(eng->setGraphDesc(d));
  const uint64_t compiles = eng->compileCount();

  sq::Ptr<sq::IPluginInstance> first = eng->pluginInstance(t.id, 0);
  REQUIRE(first);
  CHECK_EQ(fake(first)->value(), 1);
  CHECK(fake(first)->active());  // the live graph prepared (activated) it
  CHECK_EQ(fake(first)->loads(), 0);  // nothing to carry into the first graph
  CHECK(!eng->pluginInstance(t.id, 1));                 // no such line
  CHECK(!eng->pluginInstance(t.id, -1));                // audio track: no instrument
  CHECK(!eng->pluginInstance(fakes::newGuid(), 0));     // unknown node
  CHECK(!eng->pluginInstance(sq_guid{}, 0));            // master has no inserts here
  sq_guid node{};
  int32_t slot = INT32_MIN;
  REQUIRE(eng->locatePlugin(first.get(), &node, &slot));
  CHECK(node == t.id);
  CHECK_EQ(slot, 0);

  // Change the instance's state through the hosted instance, then recompile the same description.
  fake(first)->setValue(123);
  REQUIRE_OK(eng->setGraphDesc(d));
  CHECK_EQ(eng->compileCount(), compiles + 1);
  sq::Ptr<sq::IPluginInstance> second = eng->pluginInstance(t.id, 0);
  REQUIRE(second);
  CHECK(second.get() != first.get());        // a new instance (the graph was recompiled)
  CHECK_EQ(fake(second)->value(), 123);      // ... that starts where the old one left off
  CHECK_EQ(fake(second)->loads(), 1);
  CHECK(fake(second)->active());
  CHECK_EQ(fake(first)->saves(), 1);         // captured exactly once, by the recompile
  CHECK_EQ(fake(first)->value(), 123);       // our reference kept the retired instance alive, untouched
  CHECK(!fake(first)->active());             // ... but its graph released (deactivated) it

  // The newest graph is what the lookups see: the retired instance is not located any more.
  node = sq_guid{};
  slot = INT32_MIN;
  REQUIRE(eng->locatePlugin(second.get(), &node, &slot));
  CHECK(node == t.id);
  CHECK_EQ(slot, 0);
  CHECK(!eng->locatePlugin(first.get(), &node, &slot));
  std::vector<AudioEngine::PluginSlot> slots = eng->pluginSlots();
  REQUIRE_EQ(slots.size(), size_t(1));
  CHECK(slots[0].node == t.id);
  CHECK_EQ(slots[0].slot, 0);
  CHECK(slots[0].instance.get() == second.get());

  // A third compile carries the (unchanged) state again: the chain of hand-overs never breaks.
  REQUIRE_OK(eng->setGraphDesc(d));
  sq::Ptr<sq::IPluginInstance> third = eng->pluginInstance(t.id, 0);
  REQUIRE(third);
  CHECK(third.get() != second.get());
  CHECK_EQ(fake(third)->value(), 123);
}

SQ_TEST("plugin_state: slots are user.inserts LINES, not chain positions (bypassed line in front)") {
  sq::app::ClassRegistry reg;
  sq::Ptr<AudioEngine> eng = makeEngine(&reg);
  GraphDesc d;
  TrackDesc t;
  t.id = fakes::newGuid();
  t.channels = 2;
  t.inserts.push_back(fxProto("fake.fx", 1));
  t.slotToChain = {-1, 0};  // line 0 bypassed / unavailable, line 1 -> chain 0
  d.tracks.push_back(t);
  REQUIRE_OK(eng->setGraphDesc(d));
  CHECK(!eng->pluginInstance(t.id, 0));
  sq::Ptr<sq::IPluginInstance> inst = eng->pluginInstance(t.id, 1);
  REQUIRE(inst);
  sq_guid node{};
  int32_t slot = INT32_MIN;
  REQUIRE(eng->locatePlugin(inst.get(), &node, &slot));
  CHECK(node == t.id);
  CHECK_EQ(slot, 1);
  std::vector<AudioEngine::PluginSlot> slots = eng->pluginSlots();
  REQUIRE_EQ(slots.size(), size_t(1));
  CHECK_EQ(slots[0].slot, 1);

  // The same line keeps its state across a recompile ...
  fake(inst)->setValue(77);
  REQUIRE_OK(eng->setGraphDesc(d));
  sq::Ptr<sq::IPluginInstance> again = eng->pluginInstance(t.id, 1);
  REQUIRE(again);
  CHECK(again.get() != inst.get());
  CHECK_EQ(fake(again)->value(), 77);
  // ... and so does an insert that moves to another line: the state follows the insert (its spec
  // identity), never the line number.
  GraphDesc moved = d;
  moved.tracks[0].slotToChain = {0};
  REQUIRE_OK(eng->setGraphDesc(moved));
  CHECK(!eng->pluginInstance(t.id, 1));
  sq::Ptr<sq::IPluginInstance> kept = eng->pluginInstance(t.id, 0);
  REQUIRE(kept);
  CHECK(kept.get() != again.get());
  CHECK_EQ(fake(kept)->value(), 77);
  CHECK_EQ(fake(kept)->loads(), 1);
}

SQ_TEST("plugin_state: instrument (-1), bus and master plugins are listed, located and carried too") {
  sq::app::ClassRegistry reg;
  sq::Ptr<AudioEngine> eng = makeEngine(&reg);
  GraphDesc d;
  TrackDesc synth;
  synth.id = fakes::newGuid();
  synth.name = "keys";
  synth.channels = 2;
  synth.instrument = synthProto("fake.synth", 5);
  TrackDesc fx;
  fx.id = fakes::newGuid();
  fx.name = "fx";
  fx.channels = 2;
  fx.inserts.push_back(fxProto("fake.fx", 6));
  fx.slotToChain = {0};
  fx.output = 0;  // -> bus 0
  BusDesc bus;
  bus.id = fakes::newGuid();
  bus.name = "bus";
  bus.channels = 2;
  bus.inserts.push_back(fxProto("fake.bus", 7));
  bus.slotToChain = {0};
  d.tracks = {synth, fx};
  d.buses = {bus};
  d.masterInserts.push_back(fxProto("fake.master", 8));
  d.masterSlotToChain = {0};
  REQUIRE_OK(eng->setGraphDesc(d));

  sq::Ptr<sq::IPluginInstance> si = eng->pluginInstance(synth.id, -1);
  sq::Ptr<sq::IPluginInstance> fi = eng->pluginInstance(fx.id, 0);
  sq::Ptr<sq::IPluginInstance> bi = eng->pluginInstance(bus.id, 0);
  sq::Ptr<sq::IPluginInstance> mi = eng->pluginInstance(sq_guid{}, 0);  // null guid = master
  REQUIRE(si);
  REQUIRE(fi);
  REQUIRE(bi);
  REQUIRE(mi);
  CHECK_EQ(fake(si)->value(), 5);
  CHECK_EQ(fake(fi)->value(), 6);
  CHECK_EQ(fake(bi)->value(), 7);
  CHECK_EQ(fake(mi)->value(), 8);
  CHECK(fake(si)->active());
  CHECK(!eng->pluginInstance(synth.id, 0));  // the instrument track has no inserts
  CHECK(!eng->pluginInstance(fx.id, -1));    // the audio track has no instrument
  CHECK(!eng->pluginInstance(bus.id, 1));
  CHECK(!eng->pluginInstance(sq_guid{}, 1));
  CHECK_EQ(eng->pluginSlots().size(), size_t(4));

  auto locate = [&](const sq::Ptr<sq::IPluginInstance>& inst, const sq_guid& wantNode, int32_t wantSlot) {
    sq_guid node = fakes::newGuid();
    int32_t slot = INT32_MIN;
    REQUIRE(eng->locatePlugin(inst.get(), &node, &slot));
    CHECK(node == wantNode);
    CHECK_EQ(slot, wantSlot);
  };
  locate(si, synth.id, -1);
  locate(fi, fx.id, 0);
  locate(bi, bus.id, 0);
  locate(mi, sq_guid{}, 0);

  // Every kind of node carries its plugin state into the recompiled graph.
  fake(si)->setValue(50);
  fake(fi)->setValue(60);
  fake(bi)->setValue(70);
  fake(mi)->setValue(80);
  REQUIRE_OK(eng->setGraphDesc(d));
  sq::Ptr<sq::IPluginInstance> si2 = eng->pluginInstance(synth.id, -1);
  sq::Ptr<sq::IPluginInstance> fi2 = eng->pluginInstance(fx.id, 0);
  sq::Ptr<sq::IPluginInstance> bi2 = eng->pluginInstance(bus.id, 0);
  sq::Ptr<sq::IPluginInstance> mi2 = eng->pluginInstance(sq_guid{}, 0);
  REQUIRE(si2);
  REQUIRE(fi2);
  REQUIRE(bi2);
  REQUIRE(mi2);
  CHECK(si2.get() != si.get());
  CHECK(fi2.get() != fi.get());
  CHECK(bi2.get() != bi.get());
  CHECK(mi2.get() != mi.get());
  CHECK_EQ(fake(si2)->value(), 50);
  CHECK_EQ(fake(fi2)->value(), 60);
  CHECK_EQ(fake(bi2)->value(), 70);
  CHECK_EQ(fake(mi2)->value(), 80);
  locate(si2, synth.id, -1);
  locate(fi2, fx.id, 0);
  locate(bi2, bus.id, 0);
  locate(mi2, sq_guid{}, 0);
  std::vector<AudioEngine::PluginSlot> slots = eng->pluginSlots();
  REQUIRE_EQ(slots.size(), size_t(4));
  for (const AudioEngine::PluginSlot& s : slots) CHECK(eng->pluginInstance(s.node, s.slot).get() == s.instance.get());

  // A state is never handed to a different plugin: swap the master insert for another plugin id
  // in the same line and the new instance starts from its own default.
  GraphDesc swapped = d;
  swapped.masterInserts = {fxProto("fake.other", 9)};
  fake(mi2)->setValue(81);
  REQUIRE_OK(eng->setGraphDesc(swapped));
  sq::Ptr<sq::IPluginInstance> mi3 = eng->pluginInstance(sq_guid{}, 0);
  REQUIRE(mi3);
  CHECK_EQ(fake(mi3)->pluginId(), std::string("fake.other"));
  CHECK_EQ(fake(mi3)->value(), 9);
  CHECK_EQ(fake(mi3)->loads(), 0);
  CHECK_EQ(fake(eng->pluginInstance(synth.id, -1))->value(), 50);  // the others still carry
}

SQ_TEST("plugin_state: a document edit that removes, restores, moves or adds lines leaves every insert its OWN state") {
  sq::app::ClassRegistry reg;
  sq::Ptr<fakes::MDoc> doc = newDoc();
  // Two instances of the same plugin (they differ in a parameter value, as edited inserts do) and
  // another plugin.
  auto t = addTrack(*doc, SQ_TRACK_AUDIO, "fx", "fake|a||0=1\nfake|a||0=2\nfake|b|");
  sq::Ptr<AudioEngine> eng = makeDocEngine(&reg);
  REQUIRE_OK(eng->setDocument(doc.get()));
  auto at = [&](int32_t slot) {
    sq::Ptr<sq::IPluginInstance> p = eng->pluginInstance(t->id, slot);
    REQUIRE(p);
    return fake(p);  // the graph keeps the instance alive
  };
  CHECK_EQ(at(0)->param(0), 1.0);  // the factory applied the spec values
  CHECK_EQ(at(1)->param(0), 2.0);
  CHECK_EQ(at(2)->pluginId(), std::string("b"));
  // Live state the document does not hold (think: the sample loaded in the plugin's editor).
  at(0)->setValue(100);
  at(1)->setValue(200);
  at(2)->setValue(300);

  // Track.RemoveInsert slot 0: the survivor (old line 1) keeps ITS state, not line 0's.
  uint64_t compiles = eng->compileCount();
  edit(*doc, *t, "user.inserts", "fake|a||0=2\nfake|b|");
  CHECK_EQ(eng->compileCount(), compiles + 1);
  CHECK_EQ(at(0)->value(), 200);
  CHECK_EQ(at(0)->param(0), 2.0);
  CHECK_EQ(at(1)->value(), 300);
  CHECK(!eng->pluginInstance(t->id, 2));
  CHECK_EQ(eng->parkedPluginStates(), size_t(1));  // the removed instance's live state

  // Undo (the line comes back): it gets its kept live state back, the kept entry is used up.
  edit(*doc, *t, "user.inserts", "fake|a||0=1\nfake|a||0=2\nfake|b|", "undo");
  CHECK_EQ(at(0)->value(), 100);
  CHECK_EQ(at(0)->loads(), 1);
  CHECK_EQ(at(1)->value(), 200);
  CHECK_EQ(at(2)->value(), 300);
  CHECK_EQ(eng->parkedPluginStates(), size_t(0));

  // Track.MoveInsert b to the front: every state moves with its line.
  edit(*doc, *t, "user.inserts", "fake|b|\nfake|a||0=1\nfake|a||0=2");
  CHECK_EQ(at(0)->value(), 300);
  CHECK_EQ(at(1)->value(), 100);
  CHECK_EQ(at(2)->value(), 200);

  // Swapping the two "a" lines changes no structure (only values differ): the engine recompiles
  // instead of sending the values to the instances in place, so the states move with the lines.
  compiles = eng->compileCount();
  edit(*doc, *t, "user.inserts", "fake|b|\nfake|a||0=2\nfake|a||0=1");
  CHECK_EQ(eng->compileCount(), compiles + 1);
  CHECK_EQ(at(1)->value(), 200);
  CHECK_EQ(at(1)->param(0), 2.0);
  CHECK_EQ(at(2)->value(), 100);
  CHECK_EQ(at(2)->param(0), 1.0);

  // Track.AddInsert index=0 of another "a": the new one starts from its spec, the others keep theirs.
  edit(*doc, *t, "user.inserts", "fake|a|\nfake|b|\nfake|a||0=2\nfake|a||0=1");
  CHECK_EQ(at(0)->value(), 0);
  CHECK_EQ(at(0)->loads(), 0);
  CHECK_EQ(at(1)->value(), 300);
  CHECK_EQ(at(2)->value(), 200);
  CHECK_EQ(at(3)->value(), 100);

  // A parameter edit of one line is still applied live (no recompile, same instances).
  compiles = eng->compileCount();
  fakes::StatePlugin* before = at(3);
  edit(*doc, *t, "user.inserts", "fake|a|\nfake|b|\nfake|a||0=2\nfake|a||0=0.5");
  CHECK_EQ(eng->compileCount(), compiles);
  CHECK(at(3) == before);
}

SQ_TEST("plugin_state: the document's parameter values are applied over the carried state") {
  sq::app::ClassRegistry reg;
  sq::Ptr<AudioEngine> eng = makeEngine(&reg);
  GraphDesc d;
  TrackDesc t;
  t.id = fakes::newGuid();
  t.channels = 2;
  t.inserts.push_back(fxProto("fake.fx", 1));
  t.insertParams = {ParamList{{3, 0.25}}};
  t.slotToChain = {0};
  TrackDesc s;
  s.id = fakes::newGuid();
  s.channels = 2;
  s.instrument = synthProto("fake.synth", 2);
  s.instrumentParams = {{7, 0.3}};
  d.tracks = {t, s};
  REQUIRE_OK(eng->setGraphDesc(d));
  sq::Ptr<sq::IPluginInstance> fx1 = eng->pluginInstance(t.id, 0);
  sq::Ptr<sq::IPluginInstance> sy1 = eng->pluginInstance(s.id, -1);
  REQUIRE(fx1);
  REQUIRE(sy1);
  // Edited in the plugins' own editors (not in the document): parameters and opaque state.
  fake(fx1)->setParamValue(3, 0.9);
  fake(fx1)->setParamValue(4, 0.1);
  fake(fx1)->setValue(42);
  fake(sy1)->setParamValue(7, 0.8);
  fake(sy1)->setValue(43);

  REQUIRE_OK(eng->setGraphDesc(d));
  sq::Ptr<sq::IPluginInstance> fx2 = eng->pluginInstance(t.id, 0);
  sq::Ptr<sq::IPluginInstance> sy2 = eng->pluginInstance(s.id, -1);
  REQUIRE(fx2);
  REQUIRE(sy2);
  CHECK_EQ(fake(fx2)->loads(), 1);
  CHECK_EQ(fake(fx2)->value(), 42);      // the carried state ...
  CHECK_EQ(fake(fx2)->param(4), 0.1);    // ... with the values the document does not name
  CHECK_EQ(fake(fx2)->param(3), 0.25);   // the document's value wins over the carried 0.9
  CHECK_EQ(fake(sy2)->value(), 43);
  CHECK_EQ(fake(sy2)->param(7), 0.3);
  Graph* g = eng->core().newestGraph();
  REQUIRE(g);
  REQUIRE(g->track(0).insert(0) != nullptr);
  CHECK_EQ(g->track(0).insert(0)->parameter(3), 0.25f);  // what the engine reports for the slot

  // A document value edit that lands with a recompile (e.g. undo of a parameter change together
  // with a structural edit) is not reverted by the carried bytes either.
  d.tracks[0].insertParams = {ParamList{{3, 0.5}}};
  d.tracks[1].instrumentParams = {{7, 0.6}};
  REQUIRE_OK(eng->setGraphDesc(d));
  CHECK_EQ(fake(eng->pluginInstance(t.id, 0))->param(3), 0.5);
  CHECK_EQ(fake(eng->pluginInstance(t.id, 0))->value(), 42);
  CHECK_EQ(fake(eng->pluginInstance(s.id, -1))->param(7), 0.6);
}

SQ_TEST("plugin_state: no state crosses setDocument (another song, the same file reopened, File.Close)") {
  sq::app::ClassRegistry reg;
  sq::Ptr<fakes::MDoc> doc1 = newDoc();
  auto m1 = addTrack(*doc1, SQ_TRACK_MASTER, "Master", "fake|m|");
  auto t1 = addTrack(*doc1, SQ_TRACK_AUDIO, "fx", "fake|a|");
  sq::Ptr<AudioEngine> eng = makeDocEngine(&reg);
  REQUIRE_OK(eng->setDocument(doc1.get()));
  sq::Ptr<sq::IPluginInstance> a1 = eng->pluginInstance(t1->id, 0);
  sq::Ptr<sq::IPluginInstance> mm1 = eng->pluginInstance(sq_guid{}, 0);
  REQUIRE(a1);
  REQUIRE(mm1);
  fake(a1)->setValue(11);
  fake(mm1)->setValue(12);
  edit(*doc1, *t1, "user.inserts", "!fake|x|\nfake|a|");  // parks nothing (x never ran), carries a
  sq::Ptr<sq::IPluginInstance> a1b = eng->pluginInstance(t1->id, 1);
  REQUIRE(a1b);
  CHECK_EQ(fake(a1b)->value(), 11);
  edit(*doc1, *t1, "user.inserts", "!fake|a|");  // bypass: a's live state is parked
  CHECK_EQ(eng->parkedPluginStates(), size_t(1));

  // The same song reopened (same node ids; the master is always the null node) or another one
  // with the same lines: everything starts from its spec, nothing is even captured, nothing parked.
  sq::Ptr<fakes::MDoc> doc2 = newDoc();
  auto m2 = addTrack(*doc2, SQ_TRACK_MASTER, "Master", "fake|m|");
  auto t2 = addTrack(*doc2, SQ_TRACK_AUDIO, "fx", "fake|a|");
  m2->id = m1->id;
  t2->id = t1->id;
  sq::Ptr<sq::IPluginInstance> mLive = eng->pluginInstance(sq_guid{}, 0);
  REQUIRE(mLive);
  CHECK_EQ(fake(mLive)->value(), 12);  // carried through doc1's own recompiles
  const int savesM = fake(mLive)->saves();
  REQUIRE_OK(eng->setDocument(doc2.get()));
  CHECK_EQ(eng->parkedPluginStates(), size_t(0));
  sq::Ptr<sq::IPluginInstance> a2 = eng->pluginInstance(t2->id, 0);
  sq::Ptr<sq::IPluginInstance> mm2 = eng->pluginInstance(sq_guid{}, 0);
  REQUIRE(a2);
  REQUIRE(mm2);
  CHECK_EQ(fake(a2)->value(), 0);
  CHECK_EQ(fake(a2)->loads(), 0);
  CHECK_EQ(fake(mm2)->value(), 0);
  CHECK_EQ(fake(mm2)->loads(), 0);
  CHECK_EQ(fake(mLive)->saves(), savesM);

  // Within the new song the carry works again.
  fake(a2)->setValue(21);
  edit(*doc2, *t2, "user.inserts", "fake|a|\nfake|b|");
  CHECK_EQ(fake(eng->pluginInstance(t2->id, 0))->value(), 21);

  // File.Close: setDocument(nullptr) captures nothing and leaves no plugin behind.
  sq::Ptr<sq::IPluginInstance> a3 = eng->pluginInstance(t2->id, 0);
  sq::Ptr<sq::IPluginInstance> b3 = eng->pluginInstance(t2->id, 1);
  REQUIRE(a3);
  REQUIRE(b3);
  const int savesA = fake(a3)->saves(), savesB = fake(b3)->saves();
  REQUIRE_OK(eng->setDocument(nullptr));
  CHECK_EQ(fake(a3)->saves(), savesA);
  CHECK_EQ(fake(b3)->saves(), savesB);
  CHECK(eng->pluginSlots().empty());
  CHECK_EQ(eng->parkedPluginStates(), size_t(0));
  // ... and reopening after the close starts from the spec as well.
  REQUIRE_OK(eng->setDocument(doc2.get()));
  CHECK_EQ(fake(eng->pluginInstance(t2->id, 0))->loads(), 0);
}

SQ_TEST("plugin_state: a bypassed, removed or swapped-out plugin gets its live state back with its line (parked)") {
  sq::app::ClassRegistry reg;
  sq::Ptr<fakes::MDoc> doc = newDoc();
  auto t = addTrack(*doc, SQ_TRACK_AUDIO, "fx", "fake|a||0=1\nfake|b|");
  auto k = addTrack(*doc, SQ_TRACK_INSTRUMENT, "keys", "");
  k->strProps["user.instrument"] = "fake|s1|";
  sq::Ptr<AudioEngine> eng = makeDocEngine(&reg);
  REQUIRE_OK(eng->setDocument(doc.get()));
  auto at = [&](const sq_guid& node, int32_t slot) {
    sq::Ptr<sq::IPluginInstance> p = eng->pluginInstance(node, slot);
    REQUIRE(p);
    return fake(p);
  };
  at(t->id, 0)->setValue(10);
  at(t->id, 1)->setValue(20);
  at(k->id, -1)->setValue(77);

  // Bypass: the line stays (slot 0 hosts nothing), the instance goes, its live state is parked.
  edit(*doc, *t, "user.inserts", "!fake|a||0=1\nfake|b|");
  CHECK(!eng->pluginInstance(t->id, 0));
  CHECK_EQ(at(t->id, 1)->value(), 20);
  CHECK_EQ(eng->parkedPluginStates(), size_t(1));
  // Unbypass: the kept state comes back, once.
  edit(*doc, *t, "user.inserts", "fake|a||0=1\nfake|b|");
  CHECK_EQ(at(t->id, 0)->value(), 10);
  CHECK_EQ(at(t->id, 0)->loads(), 1);
  CHECK_EQ(eng->parkedPluginStates(), size_t(0));
  // ... a second, identical line added later starts from its spec (the kept state was used up).
  edit(*doc, *t, "user.inserts", "fake|a||0=1\nfake|a||0=1\nfake|b|");
  CHECK_EQ(at(t->id, 0)->value(), 10);
  CHECK_EQ(at(t->id, 1)->value(), 0);
  CHECK_EQ(at(t->id, 1)->loads(), 0);

  // A bypassed insert whose values are edited meanwhile (Track.SetInsertParam keeps the '!'; no
  // recompile) still gets its own state back when it is enabled again.
  edit(*doc, *t, "user.inserts", "fake|a||0=1\nfake|a||0=1\n!fake|b|");
  CHECK_EQ(eng->parkedPluginStates(), size_t(1));
  uint64_t compiles = eng->compileCount();
  edit(*doc, *t, "user.inserts", "fake|a||0=1\nfake|a||0=1\n!fake|b||3=0.5");
  CHECK_EQ(eng->compileCount(), compiles);
  edit(*doc, *t, "user.inserts", "fake|a||0=1\nfake|a||0=1\nfake|b||3=0.5");
  CHECK_EQ(at(t->id, 2)->value(), 20);
  CHECK_EQ(at(t->id, 2)->param(3), 0.5);  // ... with the edited value
  CHECK_EQ(eng->parkedPluginStates(), size_t(0));

  // Remove b, then undo: b comes back with its live state.
  edit(*doc, *t, "user.inserts", "fake|a||0=1\nfake|a||0=1");
  CHECK(!eng->pluginInstance(t->id, 2));
  CHECK_EQ(eng->parkedPluginStates(), size_t(1));
  edit(*doc, *t, "user.inserts", "fake|a||0=1\nfake|a||0=1\nfake|b||3=0.5", "undo");
  CHECK_EQ(at(t->id, 2)->value(), 20);
  CHECK_EQ(at(t->id, 0)->value(), 10);  // the others are untouched by the keeping
  CHECK_EQ(eng->parkedPluginStates(), size_t(0));
  // Redo the removal, then add the same line again as a NEW insert (not an undo): it starts from
  // its spec; the removed instance's state stays kept for an undo.
  edit(*doc, *t, "user.inserts", "fake|a||0=1\nfake|a||0=1", "redo");
  CHECK_EQ(eng->parkedPluginStates(), size_t(1));
  edit(*doc, *t, "user.inserts", "fake|a||0=1\nfake|a||0=1\nfake|b||3=0.5");
  CHECK_EQ(at(t->id, 2)->value(), 0);
  CHECK_EQ(at(t->id, 2)->loads(), 0);
  CHECK_EQ(at(t->id, 2)->param(3), 0.5);  // the spec value (applied by the factory)
  CHECK_EQ(eng->parkedPluginStates(), size_t(1));
  // An undo / redo puts a line back only where it was: the removed b does not come back at slot 0.
  edit(*doc, *t, "user.inserts", "fake|a||0=1\nfake|a||0=1");
  CHECK_EQ(eng->parkedPluginStates(), size_t(2));
  edit(*doc, *t, "user.inserts", "fake|b||3=0.5\nfake|a||0=1\nfake|a||0=1", "undo");
  CHECK_EQ(at(t->id, 0)->value(), 0);
  CHECK_EQ(at(t->id, 1)->value(), 10);

  // Instrument: another plugin, then undo: s1 returns with its live state; redo: s2 with its own.
  edit(*doc, *k, "user.instrument", "fake|s2|");
  CHECK_EQ(at(k->id, -1)->pluginId(), std::string("s2"));
  CHECK_EQ(at(k->id, -1)->loads(), 0);
  at(k->id, -1)->setValue(88);
  edit(*doc, *k, "user.instrument", "fake|s1|", "undo");
  CHECK_EQ(at(k->id, -1)->pluginId(), std::string("s1"));
  CHECK_EQ(at(k->id, -1)->value(), 77);
  edit(*doc, *k, "user.instrument", "fake|s2|", "redo");
  CHECK_EQ(at(k->id, -1)->value(), 88);
  // Picking s1 again (a new choice, not an undo) starts it from its spec.
  edit(*doc, *k, "user.instrument", "fake|s1|");
  CHECK_EQ(at(k->id, -1)->pluginId(), std::string("s1"));
  CHECK_EQ(at(k->id, -1)->value(), 0);
  CHECK_EQ(at(k->id, -1)->loads(), 0);

  // Bounded: bypassing a long chain parks only the newest kMaxParked (64) states.
  std::string many, bypassed;
  for (int i = 0; i < 70; ++i) {
    many += "fake|p||0=" + std::to_string(i) + "\n";
    bypassed += "!fake|p||0=" + std::to_string(i) + "\n";
  }
  edit(*doc, *t, "user.inserts", many);
  edit(*doc, *t, "user.inserts", bypassed);
  CHECK_EQ(eng->parkedPluginStates(), size_t(64));
}

SQ_TEST("plugin_state: identical duplicate lines keep their own states (bypass, enable, edits, moves)") {
  sq::app::ClassRegistry reg;
  sq::Ptr<fakes::MDoc> doc = newDoc();
  // Two byte-identical lines (the device rack's Duplicate, or two unedited plugins) and another one.
  auto t = addTrack(*doc, SQ_TRACK_AUDIO, "fx", "fake|d|\nfake|d|\nfake|c|");
  sq::Ptr<AudioEngine> eng = makeDocEngine(&reg);
  REQUIRE_OK(eng->setDocument(doc.get()));
  auto at = [&](int32_t slot) {
    sq::Ptr<sq::IPluginInstance> p = eng->pluginInstance(t->id, slot);
    REQUIRE(p);
    return fake(p);
  };
  at(0)->setValue(1);  // e.g. another preset loaded in each copy's editor: the lines stay identical
  at(1)->setValue(2);
  at(2)->setValue(3);

  // Bypass the first copy: the second keeps ITS state, the first's is kept with its line.
  edit(*doc, *t, "user.inserts", "!fake|d|\nfake|d|\nfake|c|");
  CHECK(!eng->pluginInstance(t->id, 0));
  CHECK_EQ(at(1)->value(), 2);
  CHECK_EQ(at(2)->value(), 3);
  CHECK_EQ(eng->parkedPluginStates(), size_t(1));
  // A value edit of the bypassed copy (applied without a recompile), then enable it: each copy
  // has its own state, the edited copy with its new value.
  const uint64_t compiles = eng->compileCount();
  edit(*doc, *t, "user.inserts", "!fake|d||0=5\nfake|d|\nfake|c|");
  CHECK_EQ(eng->compileCount(), compiles);
  edit(*doc, *t, "user.inserts", "fake|d||0=5\nfake|d|\nfake|c|");
  CHECK_EQ(at(0)->value(), 1);
  CHECK_EQ(at(0)->param(0), 5.0);
  CHECK_EQ(at(1)->value(), 2);
  CHECK_EQ(eng->parkedPluginStates(), size_t(0));

  // Bypass the second copy and enable it again.
  edit(*doc, *t, "user.inserts", "fake|d||0=5\n!fake|d|\nfake|c|");
  CHECK_EQ(at(0)->value(), 1);
  CHECK(!eng->pluginInstance(t->id, 1));
  edit(*doc, *t, "user.inserts", "fake|d||0=5\nfake|d|\nfake|c|");
  CHECK_EQ(at(0)->value(), 1);
  CHECK_EQ(at(1)->value(), 2);
  // Back to identical lines (a value edit back to the default: recompiled, states carried).
  edit(*doc, *t, "user.inserts", "fake|d|\nfake|d|\nfake|c|");
  CHECK_EQ(at(0)->value(), 1);

  // Track.MoveInsert of the other insert around the copies: they keep their order and states.
  edit(*doc, *t, "user.inserts", "fake|c|\nfake|d|\nfake|d|");
  CHECK_EQ(at(0)->value(), 3);
  CHECK_EQ(at(1)->value(), 1);
  CHECK_EQ(at(2)->value(), 2);
  edit(*doc, *t, "user.inserts", "fake|d|\nfake|c|\nfake|d|");
  CHECK_EQ(at(0)->value(), 1);
  CHECK_EQ(at(1)->value(), 3);
  CHECK_EQ(at(2)->value(), 2);

  // Duplicate of the last copy (the rack inserts the same line right after it): the copies keep
  // their states, the new one starts from its spec.
  edit(*doc, *t, "user.inserts", "fake|d|\nfake|c|\nfake|d|\nfake|d|");
  CHECK_EQ(at(0)->value(), 1);
  CHECK_EQ(at(2)->value(), 2);
  CHECK_EQ(at(3)->value(), 0);
  CHECK_EQ(at(3)->loads(), 0);
}

SQ_TEST("plugin_state: re-binding the document already bound keeps every live and kept state") {
  sq::app::ClassRegistry reg;
  sq::Ptr<fakes::MDoc> doc = newDoc();
  addTrack(*doc, SQ_TRACK_MASTER, "Master", "fake|m|");
  auto t = addTrack(*doc, SQ_TRACK_AUDIO, "fx", "fake|a|\nfake|b|");
  sq::Ptr<AudioEngine> eng = makeDocEngine(&reg);
  REQUIRE_OK(eng->setDocument(doc.get()));
  fake(eng->pluginInstance(t->id, 0))->setValue(5);
  fake(eng->pluginInstance(t->id, 1))->setValue(6);
  fake(eng->pluginInstance(sq_guid{}, 0))->setValue(7);
  edit(*doc, *t, "user.inserts", "fake|a|\n!fake|b|");  // b's live state is kept with its line
  REQUIRE_EQ(eng->parkedPluginStates(), size_t(1));

  // What the shell does after File.Open of the song already open, a failed import or a background
  // document closing: setDocument(the active song) again. The rebuild carries every live state.
  const uint64_t compiles = eng->compileCount();
  REQUIRE_OK(eng->setDocument(doc.get()));
  CHECK_EQ(eng->compileCount(), compiles + 1);
  CHECK_EQ(fake(eng->pluginInstance(t->id, 0))->value(), 5);
  CHECK_EQ(fake(eng->pluginInstance(t->id, 0))->loads(), 1);
  CHECK_EQ(fake(eng->pluginInstance(sq_guid{}, 0))->value(), 7);
  CHECK_EQ(eng->parkedPluginStates(), size_t(1));
  // ... the engine still follows the document, and the kept state is still there for b.
  edit(*doc, *t, "user.inserts", "fake|a|\nfake|b|");
  CHECK_EQ(fake(eng->pluginInstance(t->id, 1))->value(), 6);
  CHECK_EQ(eng->parkedPluginStates(), size_t(0));

  // Another document, then this one again: two switches, nothing carries.
  sq::Ptr<fakes::MDoc> other = newDoc();
  auto o = addTrack(*other, SQ_TRACK_AUDIO, "fx", "fake|a|");
  o->id = t->id;
  REQUIRE_OK(eng->setDocument(other.get()));
  REQUIRE_OK(eng->setDocument(doc.get()));
  CHECK_EQ(fake(eng->pluginInstance(t->id, 0))->value(), 0);
  CHECK_EQ(fake(eng->pluginInstance(t->id, 0))->loads(), 0);
  CHECK_EQ(fake(eng->pluginInstance(sq_guid{}, 0))->value(), 0);
}

SQ_TEST("plugin_state: an offline render starts from the live plugin states and changes nothing live") {
  sq::app::ClassRegistry reg;
  fakes::registerFakeCodec(&reg);
  InstanceLog log = std::make_shared<std::vector<sq::Ptr<sq::IPluginInstance>>>();
  sq::Ptr<fakes::MDoc> doc = newDoc();
  auto t = addTrack(*doc, SQ_TRACK_AUDIO, "fx", "fake|a||0=1\nfake|b|");
  sq::Ptr<AudioEngine> eng = makeDocEngine(&reg, log);
  REQUIRE_OK(eng->setDocument(doc.get()));
  fake(eng->pluginInstance(t->id, 0))->setValue(5150);
  fake(eng->pluginInstance(t->id, 1))->setValue(42);
  edit(*doc, *t, "user.inserts", "fake|a||0=1\n!fake|b|");  // b's live state is kept with its line
  REQUIRE_EQ(eng->parkedPluginStates(), size_t(1));
  sq::Ptr<sq::IPluginInstance> live = eng->pluginInstance(t->id, 0);
  REQUIRE(live);
  REQUIRE_EQ(fake(live)->value(), 5150);
  const uint64_t compiles = eng->compileCount();
  const size_t created = log->size();

  sq_render_request rq{};
  rq.struct_size = sizeof rq;
  rq.kind = SQ_RENDER_MIXDOWN;
  rq.start = 0;
  rq.end = 1;
  rq.output = "mem://plugin_state_mix.tst";
  rq.file_extension = "tst";
  sq::Ptr<sq::IRenderTask> task;
  REQUIRE_OK(eng->startRender(&rq, nullptr, task.put()));
  REQUIRE_OK(task->wait(60000));
  REQUIRE_OK(task->getResult());
  // The render graph's own instances: the insert and its fast-lane (monitoring) copy.
  REQUIRE_EQ(log->size(), created + 2);
  for (size_t i = created; i < log->size(); ++i) {
    fakes::StatePlugin* offline = fake((*log)[i]);
    CHECK(offline != fake(live));
    CHECK_EQ(offline->value(), 5150);  // started from the live state
    CHECK_EQ(offline->loads(), 1);
    CHECK_EQ(offline->param(0), 1.0);  // with the document's values
  }
  // The live graph is untouched: no recompile, same instance, the kept state still kept.
  CHECK_EQ(eng->compileCount(), compiles);
  CHECK(eng->pluginInstance(t->id, 0).get() == live.get());
  CHECK_EQ(fake(live)->value(), 5150);
  CHECK_EQ(eng->parkedPluginStates(), size_t(1));

  // The render seam with a desc that enables b (as a render of another variant would): it READS
  // b's kept state and consumes nothing.
  GraphDesc d = eng->graphDesc();
  REQUIRE_EQ(d.tracks.size(), size_t(1));
  TrackDesc& td = d.tracks[0];
  REQUIRE_EQ(td.slotToChain.size(), size_t(2));
  REQUIRE_EQ(td.slotToChain[1], -1);
  td.slotToChain[1] = static_cast<int32_t>(td.inserts.size());
  td.inserts.push_back(fakeResolver(false, nullptr)("fake|b|", nullptr));
  td.insertParams.emplace_back();
  REQUIRE(td.inserts.back());
  eng->captureRenderStates(d);
  REQUIRE_EQ(td.insertStates.size(), size_t(2));
  int32_t v = 0;
  CHECK(fakes::StatePlugin::decode(td.insertStates[0].bytes, &v, nullptr));
  CHECK_EQ(v, 5150);
  CHECK(fakes::StatePlugin::decode(td.insertStates[1].bytes, &v, nullptr));
  CHECK_EQ(v, 42);
  eng->captureRenderStates(d);  // again: still there
  CHECK(fakes::StatePlugin::decode(td.insertStates[1].bytes, &v, nullptr));
  CHECK_EQ(v, 42);
  CHECK_EQ(eng->parkedPluginStates(), size_t(1));
  CHECK_EQ(eng->compileCount(), compiles);

  // The click render hosts no plugins: it captures nothing.
  const int saves = fake(live)->saves();
  rq.kind = kRenderClick;
  rq.output = "mem://plugin_state_click.tst";
  REQUIRE_OK(eng->startRender(&rq, nullptr, task.put()));
  REQUIRE_OK(task->wait(60000));
  REQUIRE_OK(task->getResult());
  CHECK_EQ(fake(live)->saves(), saves);

  // A recompile after the renders still carries the live state, and enabling b gives it its kept
  // state (no render used it up) ...
  const size_t before = log->size();
  edit(*doc, *t, "user.inserts", "fake|a||0=1\nfake|b|");
  CHECK_EQ(fake(eng->pluginInstance(t->id, 0))->value(), 5150);
  CHECK_EQ(fake(eng->pluginInstance(t->id, 1))->value(), 42);
  CHECK_EQ(eng->parkedPluginStates(), size_t(0));
  // ... into the fast-lane (monitoring) copies of the inserts too: the chain's a and b come first.
  REQUIRE_EQ(log->size(), before + 4);
  CHECK_EQ(fake((*log)[before + 2])->pluginId(), std::string("a"));
  CHECK_EQ(fake((*log)[before + 2])->value(), 5150);
  CHECK_EQ(fake((*log)[before + 3])->pluginId(), std::string("b"));
  CHECK_EQ(fake((*log)[before + 3])->value(), 42);
  // After a song switch a render starts from the NEW song's live states.
  sq::Ptr<fakes::MDoc> other = newDoc();
  auto t2 = addTrack(*other, SQ_TRACK_AUDIO, "fx", "fake|a||0=1");
  t2->id = t->id;
  REQUIRE_OK(eng->setDocument(other.get()));
  fake(eng->pluginInstance(t2->id, 0))->setValue(6);
  rq.kind = SQ_RENDER_MIXDOWN;
  rq.output = "mem://plugin_state_mix2.tst";
  REQUIRE_OK(eng->startRender(&rq, nullptr, task.put()));
  REQUIRE_OK(task->wait(60000));
  REQUIRE_OK(task->getResult());
  REQUIRE(!log->empty());
  CHECK_EQ(fake(log->back())->value(), 6);
}

SQ_TEST("plugin_state: values a plugin reported itself are not echoed back to it (notePluginValues)") {
  sq::app::ClassRegistry reg;
  sq::Ptr<fakes::MDoc> doc = newDoc();
  auto m = addTrack(*doc, SQ_TRACK_MASTER, "Master", "fake|m||1=0.5");
  auto t = addTrack(*doc, SQ_TRACK_AUDIO, "fx", "fake|a||3=0.25\nfake|b|");
  auto k = addTrack(*doc, SQ_TRACK_INSTRUMENT, "keys", "");
  k->strProps["user.instrument"] = "fake|s||7=0.25";
  sq::Ptr<AudioEngine> eng = makeDocEngine(&reg);
  REQUIRE_OK(eng->setDocument(doc.get()));
  const uint64_t compiles = eng->compileCount();
  // The processors of the newest graph (parameter(): the last value a command gave them).
  auto graph = [&]() -> Graph& {
    Graph* g = eng->core().newestGraph();
    REQUIRE(g);
    return *g;
  };
  auto trackIndex = [&](const sq_guid& id) {
    const int32_t i = graph().findTrack(id);
    REQUIRE(i >= 0);
    return static_cast<size_t>(i);
  };
  auto mainA = [&]() { return graph().track(trackIndex(t->id)).insert(0); };
  auto fastA = [&]() { return graph().fastInsert(trackIndex(t->id), 0); };
  auto synth = [&]() { return graph().track(trackIndex(k->id)).instrument(); };
  auto masterM = [&]() { return graph().master().insert(0); };
  REQUIRE(mainA());
  REQUIRE(fastA());
  REQUIRE(synth());
  REQUIRE(masterM());
  const float mainBefore = mainA()->parameter(3);

  // The plugin's editor moved 3 to 0.75 and the shell writes that into the spec line: the value
  // goes to the fast-lane copy only (the main instance holds it; an echo would land after, and
  // undo, a newer edit of the plugin). No recompile.
  eng->notePluginValues(eng->pluginInstance(t->id, 0).get(), ParamList{{3, 0.75}});
  edit(*doc, *t, "user.inserts", "fake|a||3=0.75\nfake|b|");
  CHECK_EQ(eng->compileCount(), compiles);
  CHECK_EQ(mainA()->parameter(3), mainBefore);
  CHECK_EQ(fastA()->parameter(3), 0.75f);
  // A host-side edit (no note) reaches both.
  edit(*doc, *t, "user.inserts", "fake|a||3=0.5\nfake|b|");
  CHECK_EQ(mainA()->parameter(3), 0.5f);
  CHECK_EQ(fastA()->parameter(3), 0.5f);
  // A noted value only covers that very value ...
  eng->notePluginValues(eng->pluginInstance(t->id, 0).get(), ParamList{{3, 0.6}});
  edit(*doc, *t, "user.inserts", "fake|a||3=0.7\nfake|b|");
  CHECK_EQ(mainA()->parameter(3), 0.7f);
  // ... and only the next document update: one without it uses it up.
  eng->notePluginValues(eng->pluginInstance(t->id, 0).get(), ParamList{{3, 0.9}});
  edit(*doc, *t, "user.inserts", "fake|a||3=0.7\nfake|b||1=0.1");
  edit(*doc, *t, "user.inserts", "fake|a||3=0.9\nfake|b||1=0.1");
  CHECK_EQ(mainA()->parameter(3), 0.9f);
  eng->notePluginValues(eng->pluginInstance(t->id, 0).get(), ParamList{{3, 0.2}});
  eng->forgetPluginValues();
  edit(*doc, *t, "user.inserts", "fake|a||3=0.2\nfake|b||1=0.1");
  CHECK_EQ(mainA()->parameter(3), 0.2f);

  // Instruments and master inserts (no fast-lane copy): the command is dropped.
  const float synthBefore = synth()->parameter(7);
  eng->notePluginValues(eng->pluginInstance(k->id, -1).get(), ParamList{{7, 0.8}});
  edit(*doc, *k, "user.instrument", "fake|s||7=0.8");
  CHECK_EQ(synth()->parameter(7), synthBefore);
  edit(*doc, *k, "user.instrument", "fake|s||7=0.4");
  CHECK_EQ(synth()->parameter(7), 0.4f);
  const float masterBefore = masterM()->parameter(1);
  eng->notePluginValues(eng->pluginInstance(sq_guid{}, 0).get(), ParamList{{1, 0.9}});
  edit(*doc, *m, "user.inserts", "fake|m||1=0.9");
  CHECK_EQ(masterM()->parameter(1), masterBefore);
  edit(*doc, *m, "user.inserts", "fake|m||1=0.3");
  CHECK_EQ(masterM()->parameter(1), 0.3f);
  CHECK_EQ(eng->compileCount(), compiles);  // every edit above was a mix-only update

  // Unknown or retired instances are ignored.
  eng->notePluginValues(nullptr, ParamList{{3, 0.1}});
  sq::Ptr<sq::IPluginInstance> stray = sq::Ptr<sq::IPluginInstance>::adopt(new fakes::StatePlugin("a", 0));
  eng->notePluginValues(stray.get(), ParamList{{3, 0.1}});
  edit(*doc, *t, "user.inserts", "fake|a||3=0.1\nfake|b||1=0.1");
  CHECK_EQ(mainA()->parameter(3), 0.1f);
}

SQ_TEST("plugin_state: a reported value of an AUTOMATED parameter is not echoed back by the automation swap either") {
  sq::app::ClassRegistry reg;
  sq::Ptr<fakes::MDoc> doc = newDoc();
  // Every kind of automatable plugin parameter: a track insert (which has a fast-lane copy), an
  // instrument and a bus insert, each with a lane that has points (its static value = the spec's).
  auto t = addTrack(*doc, SQ_TRACK_AUDIO, "fx", "fake|a||3=0.25\nfake|b|");
  t->strProps["automation"] = "insert.0.3 0:0.125 100:0.125\n";
  auto k = addTrack(*doc, SQ_TRACK_INSTRUMENT, "keys", "");
  k->strProps["user.instrument"] = "fake|s||7=0.25";
  k->strProps["automation"] = "instrument.7 0:0.5 100:0.5\n";
  auto bus = addTrack(*doc, SQ_TRACK_BUS, "bus", "fake|c||2=0.25");
  bus->strProps["automation"] = "insert.0.2 0:0.5 100:0.5\n";
  sq::Ptr<AudioEngine> eng = makeDocEngine(&reg);
  REQUIRE_OK(eng->setDocument(doc.get()));
  const uint64_t compiles = eng->compileCount();
  auto graph = [&]() -> Graph& {
    Graph* g = eng->core().graphForControlThread();
    REQUIRE(g);
    return *g;
  };
  auto trackIndex = [&](const sq_guid& id) {
    const int32_t i = graph().findTrack(id);
    REQUIRE(i >= 0);
    return static_cast<size_t>(i);
  };
  auto mainA = [&]() { return graph().track(trackIndex(t->id)).insert(0); };
  auto fastA = [&]() { return graph().fastInsert(trackIndex(t->id), 0); };
  auto synth = [&]() { return graph().track(trackIndex(k->id)).instrument(); };
  auto busC = [&]() {
    const int32_t b = graph().findBus(bus->id);
    REQUIRE(b >= 0);
    return graph().bus(static_cast<size_t>(b)).insert(0);
  };
  REQUIRE(mainA());
  REQUIRE(fastA());
  REQUIRE(synth());
  REQUIRE(busC());

  // The editor moved 3 to 0.75 and the shell writes it into the spec line: the lane's static value
  // changes, so the automation is swapped (no recompile). The swap's restores leave the main
  // instance alone (it holds 0.75 and may hold a newer value by the time the swap is applied); the
  // fast-lane copy gets the value; the installed lane has the new static value.
  const float mainBefore = mainA()->parameter(3);
  REQUIRE(mainBefore != 0.75f);
  eng->notePluginValues(eng->pluginInstance(t->id, 0).get(), ParamList{{3, 0.75}});
  edit(*doc, *t, "user.inserts", "fake|a||3=0.75\nfake|b|");
  CHECK_EQ(eng->compileCount(), compiles);
  CHECK_EQ(mainA()->parameter(3), mainBefore);
  CHECK_EQ(fastA()->parameter(3), 0.75f);
  const AutomationData* installed = graph().automation();
  REQUIRE(installed);
  REQUIRE(trackIndex(t->id) < installed->tracks.size());
  REQUIRE_EQ(installed->tracks[trackIndex(t->id)].size(), size_t(1));
  CHECK_EQ(installed->tracks[trackIndex(t->id)][0].staticValue, 0.75f);

  // The instrument and the bus insert (no fast-lane copy): not sent at all.
  const float synthBefore = synth()->parameter(7);
  REQUIRE(synthBefore != 0.8f);
  eng->notePluginValues(eng->pluginInstance(k->id, -1).get(), ParamList{{7, 0.8}});
  edit(*doc, *k, "user.instrument", "fake|s||7=0.8");
  CHECK_EQ(synth()->parameter(7), synthBefore);
  const float busBefore = busC()->parameter(2);
  REQUIRE(busBefore != 0.9f);
  eng->notePluginValues(eng->pluginInstance(bus->id, 0).get(), ParamList{{2, 0.9}});
  edit(*doc, *bus, "user.inserts", "fake|c||2=0.9");
  CHECK_EQ(busC()->parameter(2), busBefore);
  CHECK_EQ(eng->compileCount(), compiles);

  // Play, then stop: the lanes drive every copy, and the stop restores the NEW static values to
  // every copy, the main instances included (the swap's exception does not outlive it).
  std::vector<float> l(64), r(64);
  float* out[2] = {l.data(), r.data()};
  ProcessContext ctx;
  ctx.sampleRate = 48000.0;
  ctx.frames = 64;
  ctx.playing = true;
  graph().process(ctx, out, 2, false, false);
  CHECK_EQ(mainA()->parameter(3), 0.125f);
  CHECK_EQ(fastA()->parameter(3), 0.125f);
  CHECK_EQ(synth()->parameter(7), 0.5f);
  CHECK_EQ(busC()->parameter(2), 0.5f);
  ctx.playing = false;
  ctx.outputFrame = 64;
  graph().process(ctx, out, 2, false, false);
  CHECK_EQ(mainA()->parameter(3), 0.75f);
  CHECK_EQ(fastA()->parameter(3), 0.75f);
  CHECK_EQ(synth()->parameter(7), 0.8f);
  CHECK_EQ(busC()->parameter(2), 0.9f);

  // A host-side edit (no note) still reaches the main instance, also through the swap.
  edit(*doc, *t, "user.inserts", "fake|a||3=0.5\nfake|b|");
  CHECK_EQ(mainA()->parameter(3), 0.5f);
  CHECK_EQ(fastA()->parameter(3), 0.5f);
  CHECK_EQ(eng->compileCount(), compiles);
}

SQ_TEST("plugin_state: an undo / redo hands a line back the state of the step it reverses, not the newest one kept") {
  sq::app::ClassRegistry reg;
  sq::Ptr<fakes::MDoc> doc = newDoc();
  auto t = addTrack(*doc, SQ_TRACK_AUDIO, "fx", "fake|a|\nfake|b|");
  auto k = addTrack(*doc, SQ_TRACK_INSTRUMENT, "keys", "");
  k->strProps["user.instrument"] = "fake|s1|";
  sq::Ptr<AudioEngine> eng = makeDocEngine(&reg);
  REQUIRE_OK(eng->setDocument(doc.get()));
  auto at = [&](const sq_guid& node, int32_t slot) {
    sq::Ptr<sq::IPluginInstance> p = eng->pluginInstance(node, slot);
    REQUIRE(p);
    return fake(p);
  };

  // Remove b (a sample loaded in it, the spec unchanged), add the same plugin again from the
  // browser at the same place, use it, then undo both steps: b gets ITS state back, not the
  // re-added copy's (which the first undo kept, newer, under the same line identity and index).
  at(t->id, 1)->setValue(20);
  edit(*doc, *t, "user.inserts", "fake|a|");
  edit(*doc, *t, "user.inserts", "fake|a|\nfake|b|");
  CHECK_EQ(at(t->id, 1)->loads(), 0);
  at(t->id, 1)->setValue(21);
  edit(*doc, *t, "user.inserts", "fake|a|", "undo");
  edit(*doc, *t, "user.inserts", "fake|a|\nfake|b|", "undo");
  CHECK_EQ(at(t->id, 1)->value(), 20);
  CHECK_EQ(at(t->id, 1)->loads(), 1);
  // The redos walk forward the same way: the removal, then the re-added copy with its state.
  edit(*doc, *t, "user.inserts", "fake|a|", "redo");
  edit(*doc, *t, "user.inserts", "fake|a|\nfake|b|", "redo");
  CHECK_EQ(at(t->id, 1)->value(), 21);

  // Instrument: a patch loaded inside s1, try s2, pick s1 again (a fresh instance of the identical
  // spec), then undo twice to get the patch back; redo twice, undo twice again.
  at(k->id, -1)->setValue(77);
  edit(*doc, *k, "user.instrument", "fake|s2|");
  at(k->id, -1)->setValue(88);
  edit(*doc, *k, "user.instrument", "fake|s1|");
  CHECK_EQ(at(k->id, -1)->value(), 0);  // a new choice starts from its spec
  at(k->id, -1)->setValue(33);
  edit(*doc, *k, "user.instrument", "fake|s2|", "undo");
  CHECK_EQ(at(k->id, -1)->value(), 88);
  edit(*doc, *k, "user.instrument", "fake|s1|", "undo");
  CHECK_EQ(at(k->id, -1)->value(), 77);
  edit(*doc, *k, "user.instrument", "fake|s2|", "redo");
  CHECK_EQ(at(k->id, -1)->value(), 88);
  edit(*doc, *k, "user.instrument", "fake|s1|", "redo");
  CHECK_EQ(at(k->id, -1)->value(), 33);
  edit(*doc, *k, "user.instrument", "fake|s2|", "undo");
  CHECK_EQ(at(k->id, -1)->value(), 88);
  edit(*doc, *k, "user.instrument", "fake|s1|", "undo");
  CHECK_EQ(at(k->id, -1)->value(), 77);

  // Those two undos kept s1' and s2 for a redo (b's state, kept by the first redo above, waits for
  // an undo). A commit clears the document's redo history: s1' and s2 are dropped; s1 is kept by
  // the commit for its undo.
  CHECK_EQ(eng->parkedPluginStates(), size_t(3));
  edit(*doc, *k, "user.instrument", "fake|s3|");
  CHECK_EQ(eng->parkedPluginStates(), size_t(2));
  edit(*doc, *k, "user.instrument", "fake|s1|", "undo");
  CHECK_EQ(at(k->id, -1)->value(), 77);
  CHECK_EQ(eng->parkedPluginStates(), size_t(2));  // b, and s3 kept by the undo for a redo
  // Another kind of change ("relink") moves no history: it drops nothing.
  edit(*doc, *k, "user.instrument", "fake|s1|", "relink");
  CHECK_EQ(eng->parkedPluginStates(), size_t(2));
}

SQ_TEST("plugin_state: over the bound, states kept for an undo / redo go before those of bypassed lines") {
  sq::app::ClassRegistry reg;
  sq::Ptr<fakes::MDoc> doc = newDoc();
  auto m = addTrack(*doc, SQ_TRACK_MASTER, "Master", "fake|r|");
  std::string many;
  for (int i = 0; i < 70; ++i) many += "fake|p||0=" + std::to_string(i) + "\n";
  auto t = addTrack(*doc, SQ_TRACK_AUDIO, "fx", many);
  sq::Ptr<AudioEngine> eng = makeDocEngine(&reg);
  REQUIRE_OK(eng->setDocument(doc.get()));
  for (int32_t i = 0; i < 70; ++i) fake(eng->pluginInstance(t->id, i))->setValue(100 + i);
  // An IR loaded in the master reverb, then the reverb bypassed early in the session.
  fake(eng->pluginInstance(sq_guid{}, 0))->setValue(42);
  edit(*doc, *m, "user.inserts", "!fake|r|");
  REQUIRE_EQ(eng->parkedPluginStates(), size_t(1));

  // Later 70 plugins are removed in one edit: each is kept for an undo, 71 > kMaxParked. The oldest
  // undo-only states go; the reverb's (older still, but its line is in the document) stays.
  edit(*doc, *t, "user.inserts", "");
  CHECK_EQ(eng->parkedPluginStates(), size_t(64));
  std::vector<AudioEngine::KeptPluginState> kept = eng->keptPluginStates();
  REQUIRE_EQ(kept.size(), size_t(1));
  int32_t v = 0;
  CHECK(fakes::StatePlugin::decode(kept[0].state.bytes, &v, nullptr));
  CHECK_EQ(v, 42);
  edit(*doc, *m, "user.inserts", "fake|r|");
  CHECK_EQ(fake(eng->pluginInstance(sq_guid{}, 0))->value(), 42);
  CHECK_EQ(fake(eng->pluginInstance(sq_guid{}, 0))->loads(), 1);
  CHECK_EQ(eng->parkedPluginStates(), size_t(63));

  // Undo of the removal: the 63 newest removed plugins get their states back, the 7 oldest (lines
  // 0..6, dropped) start from their specs.
  edit(*doc, *t, "user.inserts", many, "undo");
  CHECK_EQ(fake(eng->pluginInstance(t->id, 6))->value(), 0);
  CHECK_EQ(fake(eng->pluginInstance(t->id, 6))->loads(), 0);
  CHECK_EQ(fake(eng->pluginInstance(t->id, 7))->value(), 107);
  CHECK_EQ(fake(eng->pluginInstance(t->id, 69))->value(), 169);
  CHECK_EQ(eng->parkedPluginStates(), size_t(0));
}

namespace {
// Stand-in for the base64 text the shell writes into an "s=" token (the engine only digests it).
std::string stateText(const std::vector<uint8_t>& bytes) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string s;
  for (const uint8_t b : bytes) {
    s += kHex[b >> 4];
    s += kHex[b & 0x0F];
  }
  return s;
}
}  // namespace

SQ_TEST("plugin_state: keptPluginStates lists the bypassed lines' states; writing them into the lines keeps them kept") {
  sq::app::ClassRegistry reg;
  sq::Ptr<fakes::MDoc> doc = newDoc();
  auto m = addTrack(*doc, SQ_TRACK_MASTER, "Master", "fake|m|");
  auto t = addTrack(*doc, SQ_TRACK_AUDIO, "fx", "fake|a|\nfake|b||1=0.5");
  auto bus = addTrack(*doc, SQ_TRACK_BUS, "bus", "fake|x|\nfake|c|");
  auto k = addTrack(*doc, SQ_TRACK_INSTRUMENT, "keys", "");
  k->strProps["user.instrument"] = "fake|s|";
  sq::Ptr<AudioEngine> eng = makeDocEngine(&reg);
  REQUIRE_OK(eng->setDocument(doc.get()));
  CHECK(eng->keptPluginStates().empty());
  auto at = [&](const sq_guid& node, int32_t slot) {
    sq::Ptr<sq::IPluginInstance> p = eng->pluginInstance(node, slot);
    REQUIRE(p);
    return fake(p);
  };
  at(sq_guid{}, 0)->setValue(1);
  at(t->id, 0)->setValue(2);
  at(t->id, 1)->setValue(3);
  at(bus->id, 0)->setValue(4);
  at(bus->id, 1)->setValue(5);
  at(k->id, -1)->setValue(6);

  // Bypass b, c and the master's insert; then remove a (b moves to line 0) and replace the
  // instrument: those two are kept detached (for an undo) and are not listed.
  edit(*doc, *t, "user.inserts", "fake|a|\n!fake|b||1=0.5");
  edit(*doc, *bus, "user.inserts", "fake|x|\n!fake|c|");
  edit(*doc, *m, "user.inserts", "!fake|m|");
  edit(*doc, *t, "user.inserts", "!fake|b||1=0.5");
  edit(*doc, *k, "user.instrument", "fake|s2|");
  CHECK_EQ(eng->parkedPluginStates(), size_t(5));
  auto check = [&](const std::vector<AudioEngine::KeptPluginState>& kept) {
    REQUIRE_EQ(kept.size(), size_t(3));
    auto find = [&](const sq_guid& node) -> const AudioEngine::KeptPluginState* {
      for (const AudioEngine::KeptPluginState& s : kept)
        if (s.node == node) return &s;
      return nullptr;
    };
    int32_t v = 0;
    const AudioEngine::KeptPluginState* sb = find(t->id);
    REQUIRE(sb);
    CHECK_EQ(sb->slot, 0);
    CHECK_EQ(sb->state.pluginId, std::string("b"));
    CHECK(fakes::StatePlugin::decode(sb->state.bytes, &v, nullptr));
    CHECK_EQ(v, 3);
    const AudioEngine::KeptPluginState* sc = find(bus->id);
    REQUIRE(sc);
    CHECK_EQ(sc->slot, 1);
    CHECK(fakes::StatePlugin::decode(sc->state.bytes, &v, nullptr));
    CHECK_EQ(v, 5);
    const AudioEngine::KeptPluginState* sm = find(sq_guid{});  // null node = master
    REQUIRE(sm);
    CHECK_EQ(sm->slot, 0);
    CHECK(fakes::StatePlugin::decode(sm->state.bytes, &v, nullptr));
    CHECK_EQ(v, 1);
  };
  const std::vector<AudioEngine::KeptPluginState> kept = eng->keptPluginStates();
  check(kept);

  // File.Save (Shell::capturePluginStates): every bypassed line gets its kept state as its "s="
  // token, the '!' kept, in one document edit. Only states change: a mix-only update (no
  // recompile) after which the states are still kept with their (re-keyed) lines.
  std::string tLine, busLine, mLine;
  for (const AudioEngine::KeptPluginState& s : kept) {
    const std::string text = stateText(s.state.bytes);
    if (s.node == t->id) tLine = specSetState("!fake|b||1=0.5", text);
    else if (s.node == bus->id) busLine = specSetState("!fake|c|", text);
    else mLine = specSetState("!fake|m|", text);
  }
  REQUIRE(specBypassed(tLine));
  REQUIRE(tLine.find("|s=") != std::string::npos);
  const uint64_t compiles = eng->compileCount();
  t->strProps["user.inserts"] = tLine;
  bus->strProps["user.inserts"] = "fake|x|\n" + busLine;
  m->strProps["user.inserts"] = mLine;
  doc->fireChanged("commit");
  CHECK_EQ(eng->compileCount(), compiles);
  CHECK_EQ(eng->parkedPluginStates(), size_t(5));
  check(eng->keptPluginStates());

  // Enabled again (the lines keep their new "s=" tokens): each plugin gets its own kept state.
  edit(*doc, *t, "user.inserts", specWithoutBypass(tLine));
  edit(*doc, *bus, "user.inserts", "fake|x|\n" + specWithoutBypass(busLine));
  edit(*doc, *m, "user.inserts", specWithoutBypass(mLine));
  CHECK_EQ(at(t->id, 0)->value(), 3);
  CHECK_EQ(at(t->id, 0)->loads(), 1);
  CHECK_EQ(at(t->id, 0)->param(1), 0.5);
  CHECK_EQ(at(bus->id, 1)->value(), 5);
  CHECK_EQ(at(sq_guid{}, 0)->value(), 1);
  CHECK(eng->keptPluginStates().empty());
  CHECK_EQ(eng->parkedPluginStates(), size_t(2));  // a and s, kept for an undo
}
