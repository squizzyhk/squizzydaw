// engine/audio - hosted plugin instances of the live graph (AudioEngine::pluginInstance /
// locatePlugin / pluginSlots) and plugin state carried across graph recompiles
// (AudioEngine::carryPluginStates), on an engine without a device (graphs are installed directly).
#include <cstdint>
#include <memory>
#include <string>

#include "fakes.hpp"
#include "sq/app/class_registry.hpp"
#include "sq/engine/audio/engine.hpp"
#include "sq/engine/audio/plugin_fx.hpp"
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

  // The state is keyed by (node, line): the same line keeps it across a recompile ...
  fake(inst)->setValue(77);
  REQUIRE_OK(eng->setGraphDesc(d));
  sq::Ptr<sq::IPluginInstance> again = eng->pluginInstance(t.id, 1);
  REQUIRE(again);
  CHECK(again.get() != inst.get());
  CHECK_EQ(fake(again)->value(), 77);
  // ... and a plugin that moves to another line starts fresh (a different insert as far as the
  // document is concerned).
  GraphDesc moved = d;
  moved.tracks[0].slotToChain = {0};
  REQUIRE_OK(eng->setGraphDesc(moved));
  CHECK(!eng->pluginInstance(t.id, 1));
  sq::Ptr<sq::IPluginInstance> fresh = eng->pluginInstance(t.id, 0);
  REQUIRE(fresh);
  CHECK_EQ(fake(fresh)->value(), 1);
  CHECK_EQ(fake(fresh)->loads(), 0);
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
