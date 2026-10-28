// Squizzy Studio GUI - built-in device helpers shared by the browser (Devices tab), the mixer
// ("+ FX") and the device rack: catalog rows, insert / instrument menus, and adding devices to a
// track (commands from the shell, with a document-property fallback).
#include <algorithm>
#include <cstring>

#include "gui_panels.hpp"
#include "sq/engine/audio/builtin.hpp"
#include "sq/interfaces/pluginhost.h"

namespace sq::app::gui {

namespace ea = sq::engine::audio;

struct DeviceEntry {
  std::string category, id, name;
  bool instrument = false;
};

std::vector<DeviceEntry> browserDeviceEntries() {
  std::vector<DeviceEntry> out;
  // instruments first, then effects grouped by category (catalog order within a category)
  for (int pass = 0; pass < 2; ++pass)
    for (const auto& d : ea::builtinDevices()) {
      if (d.instrument != (pass == 0)) continue;
      out.push_back(DeviceEntry{d.instrument ? std::string("Instruments") : d.category, d.id, d.name, d.instrument});
    }
  std::stable_sort(out.begin(), out.end(), [](const DeviceEntry& a, const DeviceEntry& b) {
    if (a.instrument != b.instrument) return a.instrument;
    return a.category < b.category;
  });
  return out;
}

namespace {

void appendLine(GuiContext& ctx, const sq_guid& track, const char* key, const std::string& line) {
  std::string cur = ctx.trackString(track, key);
  if (!cur.empty() && cur.back() != '\n') cur += '\n';
  cur += line + "\n";
  ctx.setPropertyString(track, key, cur);
}

void addInsertSpec(GuiContext& ctx, const sq_guid& track, const std::string& spec, const std::string& name) {
  auto a = sq::makeAttributes();
  a->setGuid("track", &track);
  a->setString("spec", spec.c_str());
  if (SQ_FAILED(ctx.shell().execute("Track.AddInsert", a.get()))) appendLine(ctx, track, "user.inserts", spec);
  ctx.setStatus("Added " + name);
  ctx.invalidate();
}

void setInstrumentSpec(GuiContext& ctx, const sq_guid& track, const std::string& spec, const std::string& name) {
  auto a = sq::makeAttributes();
  a->setGuid("track", &track);
  a->setString("spec", spec.c_str());
  if (SQ_FAILED(ctx.shell().execute("Track.SetInstrument", a.get()))) ctx.setPropertyString(track, "user.instrument", spec);
  ctx.setStatus("Instrument: " + name);
  ctx.invalidate();
}

void newInstrumentTrack(GuiContext& ctx, const std::string& spec, const std::string& name) {
  auto a = sq::makeAttributes();
  a->setString("name", name.c_str());
  a->setString("instrument", spec.c_str());
  if (SQ_FAILED(ctx.run("Track.AddInstrument", a.get()))) return;
  // make sure the instrument is set even if the command ignored "instrument"
  auto ts = ctx.tracks();
  if (!ts.empty()) {
    const sq_guid id = ctx.editState().track;
    TrackView t;
    if (ctx.findTrack(id, &t) && t.info.kind == SQ_TRACK_INSTRUMENT && ctx.trackString(id, "user.instrument") != spec)
      setInstrumentSpec(ctx, id, spec, name);
  }
}

}  // namespace

void browserUseDevice(GuiContext& ctx, const DeviceEntry& d) {
  const std::string spec = "builtin|" + d.id + "|";
  const sq_guid sel = ctx.selectedTrack();
  TrackView t;
  const bool have = ctx.findTrack(sel, &t);
  if (d.instrument) {
    if (have && t.info.kind == SQ_TRACK_INSTRUMENT) setInstrumentSpec(ctx, sel, spec, d.name);
    else newInstrumentTrack(ctx, spec, d.name);
    return;
  }
  if (!have) {
    ctx.setStatus("Select a track first");
    return;
  }
  addInsertSpec(ctx, sel, spec, d.name);
}

std::vector<MenuItem> buildInsertMenu(GuiContext& ctx, const sq_guid& track) {
  std::vector<MenuItem> m;
  std::string lastCat;
  MenuItem* cat = nullptr;
  for (const auto& d : ea::builtinDevices()) {
    if (d.instrument) continue;
    if (d.category != lastCat || !cat) {
      lastCat = d.category;
      MenuItem c;
      c.label = d.category;
      m.push_back(c);
      cat = &m.back();
    }
    MenuItem it;
    it.label = d.name;
    const std::string spec = "builtin|" + d.id + "|", name = d.name;
    GuiContext* cp = &ctx;
    it.action = [cp, track, spec, name] { addInsertSpec(*cp, track, spec, name); };
    cat->submenu.push_back(it);
  }
  // flatten single-entry categories
  for (auto& c : m)
    if (c.submenu.size() == 1) c = c.submenu.front();
  if (auto reg = ctx.shell().plugins()) {
    MenuItem plugs;
    plugs.label = "Plugins";
    for (uint32_t i = 0; i < reg->getPluginCount(); ++i) {
      sq_plugin_desc d{};
      d.struct_size = sizeof d;
      if (reg->getPlugin(i, &d) != SQ_OK || d.status != SQ_PLUGIN_STATUS_OK) continue;
      if (std::strstr(d.category, "Instrument")) continue;
      MenuItem it;
      it.label = d.name;
      const std::string want = std::string(d.format) + ":" + d.id;
      GuiContext* cp = &ctx;
      it.action = [cp, track, want] {
        auto a = sq::makeAttributes();
        a->setGuid("track", &track);
        a->setString("plugin", want.c_str());
        cp->run("Track.InsertPlugin", a.get());
      };
      plugs.submenu.push_back(it);
    }
    if (!plugs.submenu.empty()) {
      m.push_back(MenuItem::sep());
      m.push_back(plugs);
    }
  }
  return m;
}

std::vector<MenuItem> buildInstrumentMenu(GuiContext& ctx, const sq_guid& track) {
  std::vector<MenuItem> m;
  for (const auto& d : ea::builtinDevices()) {
    if (!d.instrument) continue;
    MenuItem it;
    it.label = d.name;
    const std::string spec = "builtin|" + d.id + "|", name = d.name;
    GuiContext* cp = &ctx;
    it.action = [cp, track, spec, name] { setInstrumentSpec(*cp, track, spec, name); };
    m.push_back(it);
  }
  if (auto reg = ctx.shell().plugins()) {
    MenuItem plugs;
    plugs.label = "Plugins";
    for (uint32_t i = 0; i < reg->getPluginCount(); ++i) {
      sq_plugin_desc d{};
      d.struct_size = sizeof d;
      if (reg->getPlugin(i, &d) != SQ_OK || d.status != SQ_PLUGIN_STATUS_OK || !std::strstr(d.category, "Instrument")) continue;
      MenuItem it;
      it.label = d.name;
      const std::string spec = std::string(d.format) + "|" + d.id + "|" + d.path, name = d.name;
      GuiContext* cp = &ctx;
      it.action = [cp, track, spec, name] { setInstrumentSpec(*cp, track, spec, name); };
      plugs.submenu.push_back(it);
    }
    if (!plugs.submenu.empty()) {
      m.push_back(MenuItem::sep());
      m.push_back(plugs);
    }
  }
  return m;
}

}  // namespace sq::app::gui
