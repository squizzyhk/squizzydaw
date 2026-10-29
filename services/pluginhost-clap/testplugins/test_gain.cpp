// Squizzy Studio - CLAP test plugin "Squizzy Test Gain" (original code, test-only).
//
// A stereo gain with:
//   - a fixed algorithmic latency of kLatency frames (a real delay line, so the reported latency
//     is observable in the output),
//   - a stereo sidechain input bus (mixed into the output by the "Sidechain Mix" parameter),
//   - a note input port (counts note-ons into the read-only "Notes Received" parameter and
//     reports the change as an output parameter event) and a note output port (echoes notes),
//   - state save/load of the parameter values,
//   - a hidden "Edit Probe" parameter (test-only): every value the host sends it makes the plugin
//     act as if its editor had dragged "Sidechain Mix" to that value (GESTURE_BEGIN, PARAM_VALUE,
//     GESTURE_END on the output events), so hosts can test editor edits of an active plugin,
//   - two exported test-only functions (not CLAP; host tests reach them through their own handle
//     on this library): sq_test_gain_set_process_hook (a callback at the start of every process
//     call, before the plugin reads its input events) and sq_test_gain_set_edit_steps (how many
//     PARAM_VALUE steps an Edit Probe drag reports, default 1).
// Used by the pluginhost-clap, pluginscanner and app e2e tests; lives in bin/testplugins/.
#include <cstdlib>
#include <clap/clap.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

constexpr uint32_t kLatency = 64;
constexpr uint32_t kChannels = 2;

enum ParamId : clap_id { kGain = 1, kScMix = 2, kBypass = 3, kNotes = 4, kEditProbe = 5 };
constexpr clap_id kParamIds[] = {kGain, kScMix, kBypass, kNotes, kEditProbe};
constexpr uint32_t kParamCount = 5;

// Test-only hooks (see the exported functions at the end).
std::atomic<void (*)(void*)> gProcessHook{nullptr};
std::atomic<void*> gProcessHookCtx{nullptr};
std::atomic<uint32_t> gEditSteps{1};

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor_t kDesc = {
    CLAP_VERSION_INIT,
    "studio.squizzy.test.gain",
    "Squizzy Test Gain",
    "Squizzy Studio (test)",
    "",
    "",
    "",
    "1.0.0",
    "Gain with fixed latency, sidechain and note counter (test plugin)",
    kFeatures,
};

struct Gain {
  clap_plugin_t plugin{};
  const clap_host_t* host = nullptr;
  double gainDb = 0.0;
  double scMix = 0.0;
  double bypass = 0.0;
  double notes = 0.0;
  double editProbe = 0.0;
  float* delay[kChannels] = {nullptr, nullptr};
  uint32_t delayPos = 0;
  bool active = false;
};

Gain* self(const clap_plugin_t* p) { return static_cast<Gain*>(p->plugin_data); }

double* paramSlot(Gain* g, clap_id id) {
  switch (id) {
    case kGain: return &g->gainDb;
    case kScMix: return &g->scMix;
    case kBypass: return &g->bypass;
    case kNotes: return &g->notes;
    case kEditProbe: return &g->editProbe;
    default: return nullptr;
  }
}

double clampParam(clap_id id, double v) {
  double lo = 0, hi = 1;
  if (id == kGain) lo = -60, hi = 12;
  if (id == kNotes) hi = 1e6;
  return v < lo ? lo : v > hi ? hi : v;
}

// ---- plugin -------------------------------------------------------------------------------------
bool gInit(const clap_plugin_t*) { return true; }
void gDestroy(const clap_plugin_t* p) {
  Gain* g = self(p);
  for (float*& d : g->delay) delete[] d;
  delete g;
}
bool gActivate(const clap_plugin_t* p, double, uint32_t, uint32_t) {
  Gain* g = self(p);
  for (float*& d : g->delay) {
    delete[] d;
    d = new (std::nothrow) float[kLatency]();
    if (!d) return false;
  }
  g->delayPos = 0;
  g->active = true;
  return true;
}
void gDeactivate(const clap_plugin_t* p) { self(p)->active = false; }
bool gStartProcessing(const clap_plugin_t*) { return true; }
void gStopProcessing(const clap_plugin_t*) {}
void gReset(const clap_plugin_t* p) {
  Gain* g = self(p);
  for (float* d : g->delay)
    if (d) std::memset(d, 0, sizeof(float) * kLatency);
  g->delayPos = 0;
}

void pushNotesParam(Gain* g, const clap_output_events_t* out, uint32_t time) {
  if (!out) return;
  clap_event_param_value_t ev{};
  ev.header.size = sizeof ev;
  ev.header.time = time;
  ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
  ev.header.type = CLAP_EVENT_PARAM_VALUE;
  ev.param_id = kNotes;
  ev.note_id = -1;
  ev.port_index = -1;
  ev.channel = -1;
  ev.key = -1;
  ev.value = g->notes;
  out->try_push(out, &ev.header);
}

// Test-only "Edit Probe": what an editor drag of "Sidechain Mix" to `value` reports to the host
// (gEditSteps PARAM_VALUE events, evenly spaced from the current value, the last one exact).
void simulateEditorEdit(Gain* g, const clap_output_events_t* out, uint32_t time, double value) {
  const double from = g->scMix;
  g->scMix = clampParam(kScMix, value);
  if (!out) return;
  clap_event_param_gesture_t gesture{};
  gesture.header.size = sizeof gesture;
  gesture.header.time = time;
  gesture.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
  gesture.header.type = CLAP_EVENT_PARAM_GESTURE_BEGIN;
  gesture.param_id = kScMix;
  out->try_push(out, &gesture.header);
  clap_event_param_value_t ev{};
  ev.header.size = sizeof ev;
  ev.header.time = time;
  ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
  ev.header.type = CLAP_EVENT_PARAM_VALUE;
  ev.param_id = kScMix;
  ev.note_id = -1;
  ev.port_index = -1;
  ev.channel = -1;
  ev.key = -1;
  const uint32_t steps = gEditSteps.load(std::memory_order_relaxed);
  for (uint32_t i = 1; i <= steps; ++i) {
    ev.value = i == steps ? g->scMix : from + (g->scMix - from) * i / steps;
    out->try_push(out, &ev.header);
  }
  gesture.header.type = CLAP_EVENT_PARAM_GESTURE_END;
  out->try_push(out, &gesture.header);
}

void handleEvent(Gain* g, const clap_event_header_t* h, const clap_output_events_t* out) {
  if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
  switch (h->type) {
    case CLAP_EVENT_PARAM_VALUE: {
      auto* ev = reinterpret_cast<const clap_event_param_value_t*>(h);
      if (ev->param_id == kNotes) break;  // read-only
      if (double* slot = paramSlot(g, ev->param_id)) *slot = clampParam(ev->param_id, ev->value);
      if (ev->param_id == kEditProbe) simulateEditorEdit(g, out, h->time, g->editProbe);
      break;
    }
    case CLAP_EVENT_NOTE_ON:
    case CLAP_EVENT_NOTE_OFF: {
      if (h->type == CLAP_EVENT_NOTE_ON) {
        g->notes += 1;
        pushNotesParam(g, out, h->time);
      }
      if (out) out->try_push(out, h);  // echo on the note output port
      break;
    }
    case CLAP_EVENT_MIDI: {
      auto* ev = reinterpret_cast<const clap_event_midi_t*>(h);
      if ((ev->data[0] & 0xF0) == 0x90 && ev->data[2] > 0) {
        g->notes += 1;
        pushNotesParam(g, out, h->time);
      }
      if (out) out->try_push(out, h);
      break;
    }
    default:
      break;
  }
}

clap_process_status gProcess(const clap_plugin_t* p, const clap_process_t* proc) {
  Gain* g = self(p);
  if (!g->active || proc->audio_outputs_count < 1 || proc->audio_inputs_count < 1) return CLAP_PROCESS_ERROR;
  if (auto hook = gProcessHook.load(std::memory_order_acquire)) hook(gProcessHookCtx.load(std::memory_order_acquire));
  const clap_audio_buffer_t& in = proc->audio_inputs[0];
  const clap_audio_buffer_t* sc = proc->audio_inputs_count > 1 ? &proc->audio_inputs[1] : nullptr;
  const clap_audio_buffer_t& outb = proc->audio_outputs[0];
  const uint32_t nev = proc->in_events ? proc->in_events->size(proc->in_events) : 0;
  uint32_t ei = 0;
  for (uint32_t i = 0; i < proc->frames_count; ++i) {
    while (ei < nev) {
      const clap_event_header_t* h = proc->in_events->get(proc->in_events, ei);
      if (h->time > i) break;
      handleEvent(g, h, proc->out_events);
      ++ei;
    }
    const float gain = g->bypass >= 0.5 ? 1.0f : static_cast<float>(std::pow(10.0, g->gainDb / 20.0));
    const float mix = g->bypass >= 0.5 ? 0.0f : static_cast<float>(g->scMix);
    for (uint32_t c = 0; c < kChannels && c < outb.channel_count; ++c) {
      float x = c < in.channel_count ? in.data32[c][i] : 0.0f;
      float s = (sc && c < sc->channel_count && sc->data32) ? sc->data32[c][i] : 0.0f;
      float y = x * gain + s * mix;
      float delayed = g->delay[c][g->delayPos];
      g->delay[c][g->delayPos] = y;
      outb.data32[c][i] = delayed;
    }
    g->delayPos = (g->delayPos + 1) % kLatency;
  }
  for (; ei < nev; ++ei) handleEvent(g, proc->in_events->get(proc->in_events, ei), proc->out_events);
  return CLAP_PROCESS_CONTINUE;
}

// ---- extensions ---------------------------------------------------------------------------------
uint32_t apCount(const clap_plugin_t*, bool isInput) { return isInput ? 2 : 1; }
bool apGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info) {
  if (index >= (isInput ? 2u : 1u)) return false;
  std::memset(info, 0, sizeof *info);
  info->id = isInput ? index : 100 + index;
  info->channel_count = kChannels;
  info->port_type = CLAP_PORT_STEREO;
  if (index == 0) {
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->in_place_pair = isInput ? 100 : 0;
    std::snprintf(info->name, sizeof info->name, "%s", isInput ? "Main In" : "Main Out");
  } else {
    info->in_place_pair = CLAP_INVALID_ID;
    std::snprintf(info->name, sizeof info->name, "Sidechain");
  }
  return true;
}
const clap_plugin_audio_ports_t kAudioPorts = {apCount, apGet};

uint32_t npCount(const clap_plugin_t*, bool) { return 1; }
bool npGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_note_port_info_t* info) {
  if (index != 0) return false;
  std::memset(info, 0, sizeof *info);
  info->id = isInput ? 0 : 1;
  info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
  info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
  std::snprintf(info->name, sizeof info->name, "%s", isInput ? "Notes In" : "Notes Out");
  return true;
}
const clap_plugin_note_ports_t kNotePorts = {npCount, npGet};

uint32_t latGet(const clap_plugin_t*) { return kLatency; }
const clap_plugin_latency_t kLatencyExt = {latGet};

uint32_t tailGet(const clap_plugin_t*) { return kLatency; }
const clap_plugin_tail_t kTailExt = {tailGet};

uint32_t prCount(const clap_plugin_t*) { return kParamCount; }
bool prInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info) {
  if (index >= kParamCount) return false;
  std::memset(info, 0, sizeof *info);
  info->id = kParamIds[index];
  switch (info->id) {
    case kGain:
      info->flags = CLAP_PARAM_IS_AUTOMATABLE;
      std::snprintf(info->name, sizeof info->name, "Gain");
      std::snprintf(info->module, sizeof info->module, "Main");
      info->min_value = -60;
      info->max_value = 12;
      info->default_value = 0;
      break;
    case kScMix:
      info->flags = CLAP_PARAM_IS_AUTOMATABLE;
      std::snprintf(info->name, sizeof info->name, "Sidechain Mix");
      std::snprintf(info->module, sizeof info->module, "Main/Sidechain");
      info->max_value = 1;
      break;
    case kBypass:
      info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_BYPASS;
      std::snprintf(info->name, sizeof info->name, "Bypass");
      info->max_value = 1;
      break;
    case kNotes:
      info->flags = CLAP_PARAM_IS_READONLY;
      std::snprintf(info->name, sizeof info->name, "Notes Received");
      info->max_value = 1e6;
      break;
    case kEditProbe:
      info->flags = CLAP_PARAM_IS_HIDDEN;
      std::snprintf(info->name, sizeof info->name, "Edit Probe (test)");
      info->max_value = 1;
      break;
  }
  return true;
}
bool prValue(const clap_plugin_t* p, clap_id id, double* out) {
  double* slot = paramSlot(self(p), id);
  if (!slot) return false;
  *out = *slot;
  return true;
}
bool prToText(const clap_plugin_t*, clap_id id, double v, char* buf, uint32_t size) {
  if (id == kGain) return std::snprintf(buf, size, "%.1f dB", v) > 0;
  if (id == kBypass) return std::snprintf(buf, size, "%s", v >= 0.5 ? "On" : "Off") > 0;
  if (id == kScMix) return std::snprintf(buf, size, "%.0f %%", v * 100.0) > 0;
  if (id == kNotes) return std::snprintf(buf, size, "%.0f", v) > 0;
  if (id == kEditProbe) return std::snprintf(buf, size, "%.2f", v) > 0;
  return false;
}
bool prFromText(const clap_plugin_t*, clap_id, const char* text, double* out) {
  char* end = nullptr;
  double v = std::strtod(text, &end);
  if (end == text) return false;
  *out = v;
  return true;
}
void prFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out) {
  Gain* g = self(p);
  if (!in) return;
  for (uint32_t i = 0, n = in->size(in); i < n; ++i) handleEvent(g, in->get(in, i), out);
}
const clap_plugin_params_t kParams = {prCount, prInfo, prValue, prToText, prFromText, prFlush};

constexpr uint32_t kStateMagic = 0x53514731;  // "SQG1"
bool stSave(const clap_plugin_t* p, const clap_ostream_t* s) {
  Gain* g = self(p);
  unsigned char buf[4 + 3 * sizeof(double)];
  std::memcpy(buf, &kStateMagic, 4);
  std::memcpy(buf + 4, &g->gainDb, 8);
  std::memcpy(buf + 12, &g->scMix, 8);
  std::memcpy(buf + 20, &g->bypass, 8);
  size_t done = 0;
  while (done < sizeof buf) {
    int64_t w = s->write(s, buf + done, sizeof buf - done);
    if (w <= 0) return false;
    done += static_cast<size_t>(w);
  }
  return true;
}
bool stLoad(const clap_plugin_t* p, const clap_istream_t* s) {
  Gain* g = self(p);
  unsigned char buf[4 + 3 * sizeof(double)];
  size_t done = 0;
  while (done < sizeof buf) {
    int64_t r = s->read(s, buf + done, sizeof buf - done);
    if (r <= 0) return false;
    done += static_cast<size_t>(r);
  }
  uint32_t magic = 0;
  std::memcpy(&magic, buf, 4);
  if (magic != kStateMagic) return false;
  std::memcpy(&g->gainDb, buf + 4, 8);
  std::memcpy(&g->scMix, buf + 12, 8);
  std::memcpy(&g->bypass, buf + 20, 8);
  return true;
}
const clap_plugin_state_t kState = {stSave, stLoad};

const void* gGetExtension(const clap_plugin_t*, const char* id) {
  if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
  if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
  if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatencyExt;
  if (!std::strcmp(id, CLAP_EXT_TAIL)) return &kTailExt;
  if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
  if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
  return nullptr;
}
void gOnMainThread(const clap_plugin_t*) {}

// ---- factory / entry ----------------------------------------------------------------------------
uint32_t fCount(const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* fDesc(const clap_plugin_factory_t*, uint32_t index) { return index == 0 ? &kDesc : nullptr; }
const clap_plugin_t* fCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
  if (std::strcmp(id, kDesc.id) != 0) return nullptr;
  Gain* g = new (std::nothrow) Gain();
  if (!g) return nullptr;
  g->host = host;
  g->plugin.desc = &kDesc;
  g->plugin.plugin_data = g;
  g->plugin.init = gInit;
  g->plugin.destroy = gDestroy;
  g->plugin.activate = gActivate;
  g->plugin.deactivate = gDeactivate;
  g->plugin.start_processing = gStartProcessing;
  g->plugin.stop_processing = gStopProcessing;
  g->plugin.reset = gReset;
  g->plugin.process = gProcess;
  g->plugin.get_extension = gGetExtension;
  g->plugin.on_main_thread = gOnMainThread;
  return &g->plugin;
}
const clap_plugin_factory_t kFactory = {fCount, fDesc, fCreate};

bool eInit(const char*) { return true; }
void eDeinit() {}
const void* eGetFactory(const char* id) { return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr; }

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, eInit, eDeinit, eGetFactory};

// ---- test-only exports (not CLAP) ----------------------------------------------------------------
// `fn(ctx)` runs at the start of every process call, before the plugin reads its input events
// (null: none). Set it while no process call runs.
extern "C" CLAP_EXPORT void sq_test_gain_set_process_hook(void (*fn)(void*), void* ctx) {
  gProcessHook.store(nullptr, std::memory_order_release);
  gProcessHookCtx.store(ctx, std::memory_order_release);
  gProcessHook.store(fn, std::memory_order_release);
}
// Every later Edit Probe drag reports `steps` PARAM_VALUE events (at least 1) between its gesture events.
extern "C" CLAP_EXPORT void sq_test_gain_set_edit_steps(uint32_t steps) {
  gEditSteps.store(steps ? steps : 1u, std::memory_order_relaxed);
}
