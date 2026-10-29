// pluginhost-clap: IPluginInstance over a CLAP plugin.
//
// Threading model (see sq/interfaces/pluginhost.h):
//  - The thread that instantiates the plugin is its main thread. Every IPluginInstance method
//    except process()/reset() runs there.
//  - process()/reset() run on the audio thread. They never allocate or lock: buffers, event
//    storage and the sysex arena are sized in activate(); main-thread parameter sets travel in a
//    fixed SPSC ring.
//  - Plugin -> host requests that may arrive on any thread (request_restart, request_callback,
//    latency/params/state notifications) only set atomic flags. Parameter edits the plugin reports
//    from process() (CLAP_EVENT_PARAM_VALUE / PARAM_GESTURE_* on out_events: its editor while it is
//    active) go to the caller's params_out; when that is absent or full (the engine passes none)
//    they go into a fixed SPSC ring instead (fromAudio_; read-only parameters are outputs, not
//    edits, and are not queued). What the ring has no room for is merged per parameter into
//    preallocated records (newest value, gesture END / BEGIN) that the audio thread pushes into
//    the ring once it has room again (deactivate() delivers the rest): a parameter's final value
//    and its GESTURE_END are never lost. Both are delivered to the IPluginHostCallback (and
//    on_main_thread() is called) from the next main-thread call into this instance (pump()). The
//    interface has no idle hook: the host calls a cheap main-thread method (getLatencyFrames) on
//    every hosted instance from its idle tick (the app shell does it in Shell::idle).
//  - While active, setParamValue() queues the value for the audio thread; getParamValue() reports
//    a queued value until the plugin's process call that received it has returned, so a set is
//    visible right away and never reads back as the old value afterwards.
#include "clap_instance.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

#include "sq/interfaces/audioengine.h"
#include "sq/platform.hpp"

namespace sqclap {

namespace {

thread_local bool tAudioThread = false;

struct AudioScope {
  bool prev;
  AudioScope() : prev(tAudioThread) { tAudioThread = true; }
  ~AudioScope() { tAudioThread = prev; }
};

void copyStr(char* dst, size_t cap, const char* src) {
  if (!cap) return;
  size_t n = src ? std::strlen(src) : 0;
  if (n >= cap) n = cap - 1;
  if (n) std::memcpy(dst, src, n);
  dst[n] = 0;
}

clap_beattime toBeatTime(double ppq) {
  return static_cast<clap_beattime>(std::llround(ppq * static_cast<double>(CLAP_BEATTIME_FACTOR)));
}

// ---- IByteStream <-> clap streams ----
int64_t CLAP_ABI ostreamWrite(const clap_ostream_t* s, const void* buf, uint64_t size) {
  auto* bs = static_cast<sq::IByteStream*>(s->ctx);
  uint64_t done = 0;
  while (done < size) {
    uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(size - done, 1u << 30));
    uint32_t w = 0;
    if (SQ_FAILED(bs->write(static_cast<const uint8_t*>(buf) + done, chunk, &w)) || w == 0) return done ? static_cast<int64_t>(done) : -1;
    done += w;
  }
  return static_cast<int64_t>(done);
}
int64_t CLAP_ABI istreamRead(const clap_istream_t* s, void* buf, uint64_t size) {
  auto* bs = static_cast<sq::IByteStream*>(s->ctx);
  uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(size, 1u << 30));
  uint32_t got = 0;
  if (SQ_FAILED(bs->read(buf, chunk, &got))) return -1;
  return got;
}

}  // namespace

const char* nativeWindowApi() {
#if defined(_WIN32)
  return CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
  return CLAP_WINDOW_API_COCOA;
#else
  return CLAP_WINDOW_API_X11;
#endif
}

void fillHostInfo(clap_host_t& h) {
  h.clap_version = CLAP_VERSION;
  h.name = "Squizzy Studio";
  h.vendor = "Squizzy Studio";
  h.url = "";
  h.version = SQ_MODULE_VERSION_STR;
}

void describePlugin(const clap_plugin_descriptor_t* d, const clap_plugin_t* plugin, sq_plugin_desc* out) {
  copyStr(out->format, sizeof out->format, "clap");
  copyStr(out->id, sizeof out->id, d->id);
  copyStr(out->name, sizeof out->name, d->name);
  copyStr(out->vendor, sizeof out->vendor, d->vendor);
  copyStr(out->version, sizeof out->version, d->version);
  std::string cat;
  uint32_t flags = 0;
  for (const char* const* f = d->features; f && *f; ++f) {
    if (!cat.empty()) cat += '|';
    cat += *f;
    if (!std::strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT)) flags |= SQ_PLUGIN_INSTRUMENT;
    if (!std::strcmp(*f, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT)) flags |= SQ_PLUGIN_EFFECT;
    if (!std::strcmp(*f, CLAP_PLUGIN_FEATURE_NOTE_EFFECT)) flags |= SQ_PLUGIN_MIDI_EFFECT;
    if (!std::strcmp(*f, CLAP_PLUGIN_FEATURE_ANALYZER)) flags |= SQ_PLUGIN_ANALYZER;
  }
  copyStr(out->category, sizeof out->category, cat.c_str());
  out->audio_inputs = out->audio_outputs = out->aux_inputs = out->midi_inputs = out->midi_outputs = 0;
  if (plugin) {
    auto* ap = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    if (ap) {
      for (int dir = 0; dir < 2; ++dir) {
        bool isIn = dir == 0;
        uint32_t n = ap->count(plugin, isIn);
        for (uint32_t i = 0; i < n; ++i) {
          clap_audio_port_info_t info{};
          if (!ap->get(plugin, i, isIn, &info)) continue;
          bool main = (info.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0;
          if (isIn) {
            if (main) out->audio_inputs = info.channel_count;
            else ++out->aux_inputs;
          } else if (main) {
            out->audio_outputs = info.channel_count;
          }
        }
      }
    }
    auto* np = static_cast<const clap_plugin_note_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS));
    if (np) {
      out->midi_inputs = np->count(plugin, true);
      out->midi_outputs = np->count(plugin, false);
    }
    auto* gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
    if (gui && gui->is_api_supported(plugin, nativeWindowApi(), false)) flags |= SQ_PLUGIN_HAS_EDITOR;
  }
  if (out->aux_inputs) flags |= SQ_PLUGIN_SIDECHAIN;
  if (!(flags & (SQ_PLUGIN_INSTRUMENT | SQ_PLUGIN_EFFECT | SQ_PLUGIN_MIDI_EFFECT | SQ_PLUGIN_ANALYZER))) {
    flags |= out->audio_inputs ? SQ_PLUGIN_EFFECT : (out->midi_inputs ? SQ_PLUGIN_INSTRUMENT : 0u);
  }
  out->flags = flags;
}

// ---- host callbacks ---------------------------------------------------------------------------
struct HostThunks {
  static ClapInstance* self(const clap_host_t* h) { return static_cast<ClapInstance*>(h->host_data); }
  static void flag(const clap_host_t* h, uint32_t bit) { self(h)->pending_.fetch_or(bit, std::memory_order_acq_rel); }

  static void CLAP_ABI requestRestart(const clap_host_t* h) { flag(h, ClapInstance::kPendRestart); }
  static void CLAP_ABI requestProcess(const clap_host_t*) {}  // we process continuously while active
  static void CLAP_ABI requestCallback(const clap_host_t* h) { flag(h, ClapInstance::kPendCallback); }

  static void CLAP_ABI log(const clap_host_t*, clap_log_severity sev, const char* msg) {
    if (tAudioThread || !msg) return;  // never log from the audio thread
    int32_t lvl = sev <= CLAP_LOG_DEBUG ? SQ_LOG_DEBUG : sev == CLAP_LOG_INFO ? SQ_LOG_INFO : sev == CLAP_LOG_WARNING ? SQ_LOG_WARN : SQ_LOG_ERROR;
    sq::logf(lvl, "plugin: %s", msg);
  }
  static bool CLAP_ABI isMain(const clap_host_t* h) { return self(h)->isMainThread(); }
  static bool CLAP_ABI isAudio(const clap_host_t*) { return tAudioThread; }
  static void CLAP_ABI latencyChanged(const clap_host_t* h) { flag(h, ClapInstance::kPendLatency); }
  static void CLAP_ABI tailChanged(const clap_host_t* h) { flag(h, ClapInstance::kPendTail); }
  static void CLAP_ABI paramsRescan(const clap_host_t* h, clap_param_rescan_flags) { flag(h, ClapInstance::kPendParamsRescan); }
  static void CLAP_ABI paramsClear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
  static void CLAP_ABI paramsRequestFlush(const clap_host_t* h) { flag(h, ClapInstance::kPendFlush); }
  static void CLAP_ABI stateDirty(const clap_host_t* h) { flag(h, ClapInstance::kPendStateDirty); }
  static bool CLAP_ABI apRescanSupported(const clap_host_t*, uint32_t) { return false; }  // needs restart
  static void CLAP_ABI apRescan(const clap_host_t* h, uint32_t) { flag(h, ClapInstance::kPendRestart); }
  static uint32_t CLAP_ABI npDialects(const clap_host_t*) { return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; }
  static void CLAP_ABI npRescan(const clap_host_t* h, uint32_t) { flag(h, ClapInstance::kPendRestart); }
  static void CLAP_ABI guiHintsChanged(const clap_host_t*) {}
  static bool CLAP_ABI guiRequestResize(const clap_host_t* h, uint32_t w, uint32_t hgt) {
    ClapInstance* s = self(h);
    if (!s->isMainThread() || !s->cb_) return false;
    return s->cb_->onEditorResize(s, w, hgt) != SQ_FALSE;
  }
  static bool CLAP_ABI guiRequestShow(const clap_host_t*) { return false; }
  static bool CLAP_ABI guiRequestHide(const clap_host_t*) { return false; }
  static void CLAP_ABI guiClosed(const clap_host_t* h, bool) { self(h)->editorOpen_ = false; }

  static const void* CLAP_ABI getExtension(const clap_host_t*, const char* id) {
    static const clap_host_log_t kLog = {log};
    static const clap_host_thread_check_t kThreadCheck = {isMain, isAudio};
    static const clap_host_latency_t kLatency = {latencyChanged};
    static const clap_host_tail_t kTail = {tailChanged};
    static const clap_host_params_t kParams = {paramsRescan, paramsClear, paramsRequestFlush};
    static const clap_host_state_t kState = {stateDirty};
    static const clap_host_audio_ports_t kAudioPorts = {apRescanSupported, apRescan};
    static const clap_host_note_ports_t kNotePorts = {npDialects, npRescan};
    static const clap_host_gui_t kGui = {guiHintsChanged, guiRequestResize, guiRequestShow, guiRequestHide, guiClosed};
    if (!id) return nullptr;
    if (!std::strcmp(id, CLAP_EXT_LOG)) return &kLog;
    if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK)) return &kThreadCheck;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kLatency;
    if (!std::strcmp(id, CLAP_EXT_TAIL)) return &kTail;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kParams;
    if (!std::strcmp(id, CLAP_EXT_STATE)) return &kState;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &kAudioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &kNotePorts;
    if (!std::strcmp(id, CLAP_EXT_GUI)) return &kGui;
    return nullptr;
  }

  // Event lists.
  static uint32_t CLAP_ABI inSize(const clap_input_events_t* l) { return static_cast<ClapInstance*>(l->ctx)->evCount_; }
  static const clap_event_header_t* CLAP_ABI inGet(const clap_input_events_t* l, uint32_t i) {
    auto* s = static_cast<ClapInstance*>(l->ctx);
    return i < s->evCount_ ? s->evList_[i] : nullptr;
  }
  static bool CLAP_ABI outPush(const clap_output_events_t* l, const clap_event_header_t* e) {
    return static_cast<ClapInstance*>(l->ctx)->pushOutputEvent(e);
  }
};

ClapInstance::ClapInstance() {
  fillHostInfo(host_);
  host_.host_data = this;
  host_.get_extension = &HostThunks::getExtension;
  host_.request_restart = &HostThunks::requestRestart;
  host_.request_process = &HostThunks::requestProcess;
  host_.request_callback = &HostThunks::requestCallback;
  inEvents_.ctx = this;
  inEvents_.size = &HostThunks::inSize;
  inEvents_.get = &HostThunks::inGet;
  outEvents_.ctx = this;
  outEvents_.try_push = &HostThunks::outPush;
  desc_.struct_size = sizeof desc_;
}

ClapInstance::~ClapInstance() {
  // The refcount is already 0: no host callback may see this object any more (a callback that
  // add-refs and releases its argument would delete it a second time). deactivate() pumps.
  cb_ = nullptr;
  if (plugin_) {
    if (editorOpen_ && gui_) gui_->destroy(plugin_);
    if (active_) deactivate();
    plugin_->destroy(plugin_);
    plugin_ = nullptr;
  }
  lib_.reset();
}

sq_result ClapInstance::create(std::shared_ptr<ClapLibrary> lib, const sq_plugin_desc* desc, sq::IPluginHostCallback* cb,
                               sq::IPluginInstance** out) {
  if (!lib || !desc || !out) return SQ_E_INVALIDARG;
  *out = nullptr;
  sq::Ptr<ClapInstance> inst = sq::Ptr<ClapInstance>::adopt(new (std::nothrow) ClapInstance());
  if (!inst) return SQ_E_OUTOFMEMORY;
  inst->mainThread_ = sq_thread_current_id();
  inst->lib_ = lib;
  inst->cb_ = sq::Ptr<sq::IPluginHostCallback>(cb);
  inst->plugin_ = lib->factory()->create_plugin(lib->factory(), &inst->host_, desc->id);
  if (!inst->plugin_) return SQ_E_NOTFOUND;
  if (!inst->plugin_->init(inst->plugin_)) {
    inst->plugin_->destroy(inst->plugin_);
    inst->plugin_ = nullptr;
    return SQ_E_FAIL;
  }
  inst->queryExtensions();
  inst->desc_ = *desc;
  inst->desc_.struct_size = sizeof inst->desc_;
  describePlugin(inst->plugin_->desc, inst->plugin_, &inst->desc_);
  copyStr(inst->desc_.path, sizeof inst->desc_.path, lib->path().c_str());
  inst->desc_.status = SQ_PLUGIN_STATUS_OK;
  // Default activation: main buses on, aux (sidechain) off.
  for (int dir = 0; dir < 2; ++dir) {
    bool isIn = dir == 0;
    uint32_t n = inst->getBusCount(isIn ? SQ_TRUE : SQ_FALSE);
    auto& act = isIn ? inst->inActive_ : inst->outActive_;
    act.assign(n, 0);
    for (uint32_t i = 0; i < n; ++i) {
      sq_plugin_bus_info bi{};
      bi.struct_size = sizeof bi;
      if (SQ_SUCCEEDED(inst->getBusInfo(isIn ? SQ_TRUE : SQ_FALSE, i, &bi))) act[i] = (bi.flags & SQ_BUS_MAIN) ? 1 : 0;
    }
  }
  *out = inst.detach();
  return SQ_OK;
}

void ClapInstance::queryExtensions() {
  auto ext = [this](const char* id) { return plugin_->get_extension(plugin_, id); };
  audioPorts_ = static_cast<const clap_plugin_audio_ports_t*>(ext(CLAP_EXT_AUDIO_PORTS));
  portsActivation_ = static_cast<const clap_plugin_audio_ports_activation_t*>(ext(CLAP_EXT_AUDIO_PORTS_ACTIVATION));
  if (!portsActivation_)
    portsActivation_ = static_cast<const clap_plugin_audio_ports_activation_t*>(ext(CLAP_EXT_AUDIO_PORTS_ACTIVATION_COMPAT));
  notePorts_ = static_cast<const clap_plugin_note_ports_t*>(ext(CLAP_EXT_NOTE_PORTS));
  params_ = static_cast<const clap_plugin_params_t*>(ext(CLAP_EXT_PARAMS));
  latency_ = static_cast<const clap_plugin_latency_t*>(ext(CLAP_EXT_LATENCY));
  tail_ = static_cast<const clap_plugin_tail_t*>(ext(CLAP_EXT_TAIL));
  state_ = static_cast<const clap_plugin_state_t*>(ext(CLAP_EXT_STATE));
  gui_ = static_cast<const clap_plugin_gui_t*>(ext(CLAP_EXT_GUI));
}

bool ClapInstance::isMainThread() const { return !tAudioThread && sq_thread_current_id() == mainThread_; }

void ClapInstance::pump() {
  if (!plugin_ || !isMainThread()) return;
  // Editor edits reported from process() (pushOutputEvent), oldest first.
  ParamEdit pe{};
  while (fromAudio_.pop(pe)) {
    if (!cb_) continue;
    const sq_param_change c{pe.id, 0, pe.value, pe.flags};
    cb_->onParamEdited(this, &c);
  }
  uint32_t p = pending_.exchange(0, std::memory_order_acq_rel);
  if (!p) return;
  if (p & kPendCallback) plugin_->on_main_thread(plugin_);
  if (p & kPendFlush) {
    if (!active_ && params_) flushParamsMainThread(nullptr, 0, true);
    // while active the next process() call flushes
  }
  if (p & kPendLatency) {
    if (active_ && latency_) latencyFrames_.store(latency_->get(plugin_), std::memory_order_release);
    if (cb_) cb_->onLatencyChanged(this);
  }
  if ((p & kPendParamsRescan) && cb_) cb_->onParamsChanged(this);
  if ((p & kPendStateDirty) && cb_) cb_->onStateDirty(this);
  if ((p & kPendRestart) && cb_) cb_->onRestartRequested(this);
  // kPendTail: getTailFrames() queries live, nothing to cache.
}

// ---- buses ------------------------------------------------------------------------------------
sq_result ClapInstance::getDesc(sq_plugin_desc* out) {
  pump();
  if (!out || out->struct_size < sizeof(sq_plugin_desc)) return SQ_E_INVALIDARG;
  *out = desc_;
  return SQ_OK;
}

uint32_t ClapInstance::getBusCount(sq_bool input) {
  pump();
  return audioPorts_ ? audioPorts_->count(plugin_, input != SQ_FALSE) : 0;
}

sq_result ClapInstance::getBusInfo(sq_bool input, uint32_t index, sq_plugin_bus_info* out) {
  if (!out || out->struct_size < sizeof(sq_plugin_bus_info)) return SQ_E_INVALIDARG;
  if (!audioPorts_) return SQ_E_NOTFOUND;
  bool isIn = input != SQ_FALSE;
  if (index >= audioPorts_->count(plugin_, isIn)) return SQ_E_NOTFOUND;
  clap_audio_port_info_t info{};
  if (!audioPorts_->get(plugin_, index, isIn, &info)) return SQ_E_FAIL;
  copyStr(out->name, sizeof out->name, info.name);
  out->channels = info.channel_count;
  out->flags = (info.flags & CLAP_AUDIO_PORT_IS_MAIN) ? SQ_BUS_MAIN : SQ_BUS_AUX;
  const auto& act = isIn ? inActive_ : outActive_;
  if (index < act.size() ? act[index] != 0 : (out->flags & SQ_BUS_MAIN) != 0) out->flags |= SQ_BUS_ACTIVE;
  return SQ_OK;
}

sq_result ClapInstance::setBusActive(sq_bool input, uint32_t index, sq_bool active) {
  pump();
  if (active_) return SQ_E_BUSY;
  auto& act = input ? inActive_ : outActive_;
  if (index >= act.size()) return SQ_E_NOTFOUND;
  act[index] = active ? 1 : 0;
  return SQ_OK;
}

// ---- activation --------------------------------------------------------------------------------
sq_result ClapInstance::activate(const sq_plugin_setup* setup) {
  pump();
  if (!setup || setup->struct_size < sizeof(sq_plugin_setup) || setup->sample_rate <= 0 || setup->max_block_frames == 0)
    return SQ_E_INVALIDARG;
  if (active_) return SQ_S_FALSE;
  try {
    maxFrames_ = setup->max_block_frames;
    // Port layout snapshot.
    auto layout = [this](bool isIn, std::vector<clap_audio_buffer_t>& bufs, std::vector<uint32_t>& chans,
                         std::vector<std::vector<float*>>& ptrs) {
      uint32_t n = audioPorts_ ? audioPorts_->count(plugin_, isIn) : 0;
      bufs.assign(n, clap_audio_buffer_t{});
      chans.assign(n, 0);
      ptrs.assign(n, {});
      for (uint32_t i = 0; i < n; ++i) {
        clap_audio_port_info_t info{};
        if (audioPorts_->get(plugin_, i, isIn, &info)) chans[i] = info.channel_count;
        ptrs[i].assign(chans[i] ? chans[i] : 1, nullptr);
      }
    };
    layout(true, inBufs_, inChannels_, inPtrs_);
    layout(false, outBufs_, outChannels_, outPtrs_);
    inActive_.resize(inBufs_.size(), 0);
    outActive_.resize(outBufs_.size(), 0);
    zeroIn_.assign(maxFrames_, 0.0f);
    trashOut_.assign(maxFrames_, 0.0f);
    evStore_.assign(4096, EventSlot{});
    evList_.assign(4096, nullptr);
    sysexArena_.assign(64 * 1024, 0);
    // Parameter value table (for gesture values on output), read-only flags, in-flight sets.
    paramIds_.clear();
    std::vector<std::pair<uint32_t, uint8_t>> infos;  // (id, read-only)
    if (params_) {
      uint32_t n = params_->count(plugin_);
      for (uint32_t i = 0; i < n; ++i) {
        clap_param_info_t pi{};
        if (!params_->get_info(plugin_, i, &pi)) continue;
        infos.emplace_back(pi.id, (pi.flags & CLAP_PARAM_IS_READONLY) ? uint8_t{1} : uint8_t{0});
      }
      std::sort(infos.begin(), infos.end());
    }
    paramIds_.reserve(infos.size());
    paramReadOnly_.clear();
    paramReadOnly_.reserve(infos.size());
    for (const auto& [id, ro] : infos) {
      paramIds_.push_back(id);
      paramReadOnly_.push_back(ro);
    }
    const size_t np = paramIds_.size() ? paramIds_.size() : 1;
    paramValues_.reset(new std::atomic<double>[np]);
    for (size_t i = 0; i < paramIds_.size(); ++i) {
      double v = 0;
      if (params_) params_->get_value(plugin_, paramIds_[i], &v);
      paramValues_[i].store(v, std::memory_order_relaxed);
    }
    paramQueuedSeq_.assign(np, 0);
    paramQueuedValue_.assign(np, 0.0);
    paramAppliedSeq_.reset(new std::atomic<uint32_t>[np]);
    for (size_t i = 0; i < np; ++i) paramAppliedSeq_[i].store(0, std::memory_order_relaxed);
    paramTakenSeq_.assign(np, 0);
    paramTakenSlots_.assign(np, 0);
    paramTakenCount_ = 0;
    paramHeld_.assign(np, 0);
    paramHeldValue_.assign(np, 0.0);
    heldSlots_.assign(np, 0);
    heldHead_ = heldCount_ = 0;
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
  noteDialectClap_ = true;
  hasNoteInput_ = false;
  if (notePorts_ && notePorts_->count(plugin_, true) > 0) {
    clap_note_port_info_t ni{};
    if (notePorts_->get(plugin_, 0, true, &ni)) {
      hasNoteInput_ = true;
      noteDialectClap_ = (ni.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0;
    }
  }
  if (portsActivation_) {
    for (uint32_t i = 0; i < inActive_.size(); ++i) portsActivation_->set_active(plugin_, true, i, inActive_[i] != 0, 32);
    for (uint32_t i = 0; i < outActive_.size(); ++i) portsActivation_->set_active(plugin_, false, i, outActive_[i] != 0, 32);
  }
  if (!plugin_->activate(plugin_, setup->sample_rate, setup->min_block_frames ? setup->min_block_frames : 1, maxFrames_))
    return SQ_E_FAIL;
  active_ = true;
  processing_ = false;
  steadyTime_ = 0;
  latencyFrames_.store(latency_ ? latency_->get(plugin_) : 0, std::memory_order_release);
  pending_.fetch_and(~static_cast<uint32_t>(kPendLatency), std::memory_order_acq_rel);  // already read
  pump();
  return SQ_OK;
}

sq_result ClapInstance::deactivate() {
  if (!active_) return SQ_S_FALSE;
  if (processing_) {
    // The audio thread is stopped (contract: never concurrent with process); act as it.
    AudioScope scope;
    plugin_->stop_processing(plugin_);
    processing_ = false;
  }
  plugin_->deactivate(plugin_);
  active_ = false;
  // The edits process() reported come first, oldest first: the ring's, then those held back from a
  // full ring that no process() call pushes any more. Anything the flush in pump() (a pending
  // request_flush) reports is newer. Like pump(), only on the main thread: an
  // instance released elsewhere (a stuck offline render's, on its worker) drops the held ones and
  // leaves the ring.
  const bool deliver = isMainThread();
  if (deliver)
    for (ParamEdit pe{}; fromAudio_.pop(pe);) {
      if (!cb_) continue;
      const sq_param_change c{pe.id, 0, pe.value, pe.flags};
      cb_->onParamEdited(this, &c);
    }
  for (ParamEdit pe{}; nextHeldEdit(pe); popHeldEdit()) {
    if (!cb_ || !deliver) continue;
    const sq_param_change c{pe.id, 0, pe.value, pe.flags};
    cb_->onParamEdited(this, &c);
  }
  // Sets no process() call picked up any more still reach the (now inactive) plugin.
  std::vector<ParamSet> late;
  ParamSet ps{};
  while (toAudio_.pop(ps)) {
    try {
      late.push_back(ps);
    } catch (...) {
    }
  }
  if (!late.empty()) {
    try {
      flushParamsMainThread(late.data(), static_cast<uint32_t>(late.size()), false);
    } catch (...) {  // out of memory: the sets are lost (the destructor calls this too)
    }
  }
  pump();
  return SQ_OK;
}

// ---- audio thread ------------------------------------------------------------------------------
uint32_t ClapInstance::paramSlot(uint32_t id) const {
  auto it = std::lower_bound(paramIds_.begin(), paramIds_.end(), id);
  return (it != paramIds_.end() && *it == id) ? static_cast<uint32_t>(it - paramIds_.begin()) : UINT32_MAX;
}

bool ClapInstance::pushOutputEvent(const clap_event_header_t* h) {
  if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID) return false;
  sq_plugin_process* d = curProcess_;
  auto addParam = [&](uint32_t id, uint32_t time, double value, uint32_t flags, uint32_t slot) -> bool {
    if (flushEdits_) {  // main-thread flush (editor edits)
      try {
        flushEdits_->push_back(sq_param_change{id, 0, value, flags});
      } catch (...) {
        return false;
      }
      return true;
    }
    if (!d) return false;  // not inside process()
    // A held parameter's newer edits queue up behind the held ones (holdEdit).
    const bool held = slot != UINT32_MAX && paramHeld_[slot] != 0;
    if (!held && d->params_out && d->params_out_count < d->params_out_capacity) {
      d->params_out[d->params_out_count++] = sq_param_change{id, time, value, flags};
      return true;
    }
    // No (more) room in params_out: the main thread reports the edit from pump(). A read-only
    // parameter (meter, counter) is an output of the plugin, not an edit, and a gesture of a
    // parameter the table does not know has no value to report: both accepted and dropped.
    if (slot == UINT32_MAX ? flags != 0 : paramReadOnly_[slot] != 0) return true;
    if (!held) {
      if (heldCount_) pushHeldEdits();  // the edits held before this one go first
      if ((!heldCount_ || slot == UINT32_MAX) && fromAudio_.push(ParamEdit{id, flags, value})) return true;
      if (slot == UINT32_MAX) return false;  // no record to hold it in
    }
    holdEdit(slot, flags, value);
    return true;
  };
  auto addMidi = [&](uint32_t time, uint8_t b0, uint8_t b1, uint8_t b2, uint32_t size, const uint8_t* data) -> bool {
    if (!d || !d->midi_out || d->midi_out_count >= d->midi_out_capacity) return false;
    sq_midi_event& m = d->midi_out[d->midi_out_count++];
    m.frame_offset = time;
    m.bytes[0] = b0;
    m.bytes[1] = b1;
    m.bytes[2] = b2;
    m.bytes[3] = 0;
    m.size = size;
    m.data = data;
    return true;
  };
  switch (h->type) {
    case CLAP_EVENT_PARAM_VALUE: {
      auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
      uint32_t slot = paramSlot(e->param_id);
      if (slot != UINT32_MAX) paramValues_[slot].store(e->value, std::memory_order_relaxed);
      return addParam(e->param_id, h->time, e->value, 0, slot);
    }
    case CLAP_EVENT_PARAM_GESTURE_BEGIN:
    case CLAP_EVENT_PARAM_GESTURE_END: {
      auto* e = reinterpret_cast<const clap_event_param_gesture_t*>(h);
      uint32_t slot = paramSlot(e->param_id);
      double v = slot != UINT32_MAX ? paramValues_[slot].load(std::memory_order_relaxed) : 0.0;
      return addParam(e->param_id, h->time, v, h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? SQ_PARAM_GESTURE_BEGIN : SQ_PARAM_GESTURE_END,
                      slot);
    }
    case CLAP_EVENT_NOTE_ON:
    case CLAP_EVENT_NOTE_OFF: {
      auto* e = reinterpret_cast<const clap_event_note_t*>(h);
      int ch = e->channel < 0 ? 0 : (e->channel & 15);
      int key = e->key < 0 ? 0 : (e->key & 127);
      int vel = static_cast<int>(std::lround(std::clamp(e->velocity, 0.0, 1.0) * 127.0));
      if (h->type == CLAP_EVENT_NOTE_ON) return addMidi(h->time, static_cast<uint8_t>(0x90 | ch), static_cast<uint8_t>(key), static_cast<uint8_t>(vel ? vel : 1), 3, nullptr);
      return addMidi(h->time, static_cast<uint8_t>(0x80 | ch), static_cast<uint8_t>(key), static_cast<uint8_t>(vel), 3, nullptr);
    }
    case CLAP_EVENT_MIDI: {
      auto* e = reinterpret_cast<const clap_event_midi_t*>(h);
      uint8_t st = e->data[0];
      uint32_t size = (st >= 0xF8 || st == 0xF6) ? 1 : ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0 || st == 0xF1 || st == 0xF3) ? 2 : 3;
      return addMidi(h->time, e->data[0], e->data[1], e->data[2], size, nullptr);
    }
    case CLAP_EVENT_MIDI_SYSEX: {
      auto* e = reinterpret_cast<const clap_event_midi_sysex_t*>(h);
      if (!e->buffer || !e->size || sysexUsed_ + e->size > sysexArena_.size()) return false;
      uint8_t* dst = sysexArena_.data() + sysexUsed_;
      std::memcpy(dst, e->buffer, e->size);
      sysexUsed_ += e->size;
      return addMidi(h->time, 0xF0, 0, 0, e->size, dst);
    }
    default:
      return false;
  }
}

void ClapInstance::holdEdit(uint32_t slot, uint32_t flags, double value) {
  uint8_t& h = paramHeld_[slot];
  if (!h) {  // newly held: joins the FIFO (each slot at most once, so heldSlots_ never overflows)
    heldSlots_[(heldHead_ + heldCount_) % static_cast<uint32_t>(heldSlots_.size())] = slot;
    ++heldCount_;
  }
  h = static_cast<uint8_t>(h | kHeldEdit);
  if (flags & SQ_PARAM_GESTURE_BEGIN) h = static_cast<uint8_t>(h | kHeldOpen);
  if (flags & SQ_PARAM_GESTURE_END) h = static_cast<uint8_t>((h & ~kHeldOpen) | kHeldEnd);
  paramHeldValue_[slot] = value;
}

bool ClapInstance::nextHeldEdit(ParamEdit& out) const {
  if (!heldCount_) return false;
  // A held parameter reports at most two edits, both with its newest value: GESTURE_END when a
  // gesture ended, then GESTURE_BEGIN when one is still open (a plain value when neither).
  const uint32_t slot = heldSlots_[heldHead_];
  const uint8_t h = paramHeld_[slot];
  const uint32_t flags = (h & kHeldEnd) ? SQ_PARAM_GESTURE_END : (h & kHeldOpen) ? SQ_PARAM_GESTURE_BEGIN : 0u;
  out = ParamEdit{paramIds_[slot], flags, paramHeldValue_[slot]};
  return true;
}

void ClapInstance::popHeldEdit() {
  if (!heldCount_) return;
  const uint32_t slot = heldSlots_[heldHead_];
  uint8_t& h = paramHeld_[slot];
  h = ((h & kHeldEnd) && (h & kHeldOpen)) ? static_cast<uint8_t>(kHeldEdit | kHeldOpen) : uint8_t{0};
  if (h) return;  // its BEGIN is next
  heldHead_ = (heldHead_ + 1) % static_cast<uint32_t>(heldSlots_.size());
  --heldCount_;
}

void ClapInstance::pushHeldEdits() {
  ParamEdit pe{};
  while (nextHeldEdit(pe) && fromAudio_.push(pe)) popHeldEdit();
}

sq_result ClapInstance::process(sq_plugin_process* d) {
  if (!d || d->struct_size < sizeof(sq_plugin_process)) return SQ_E_INVALIDARG;
  if (!active_) return SQ_E_FAIL;
  if (d->frames > maxFrames_) return SQ_E_INVALIDARG;
  AudioScope scope;
  if (!processing_) {
    if (!plugin_->start_processing(plugin_)) return SQ_E_FAIL;
    processing_ = true;
  }
  const uint32_t frames = d->frames;
  d->midi_out_count = 0;
  d->params_out_count = 0;
  sysexUsed_ = 0;

  // ---- audio buffers ----
  std::memset(zeroIn_.data(), 0, sizeof(float) * frames);
  for (uint32_t b = 0; b < inBufs_.size(); ++b) {
    const sq_audio_block* blk = b < d->input_bus_count && d->inputs ? &d->inputs[b] : nullptr;
    auto& ptrs = inPtrs_[b];
    for (uint32_t c = 0; c < inChannels_[b]; ++c)
      ptrs[c] = (blk && blk->channels && c < blk->channel_count && blk->channels[c]) ? blk->channels[c] : zeroIn_.data();
    clap_audio_buffer_t& cb = inBufs_[b];
    cb.data32 = ptrs.data();
    cb.data64 = nullptr;
    cb.channel_count = inChannels_[b];
    cb.latency = 0;
    cb.constant_mask = 0;
  }
  for (uint32_t b = 0; b < outBufs_.size(); ++b) {
    const sq_audio_block* blk = b < d->output_bus_count && d->outputs ? &d->outputs[b] : nullptr;
    auto& ptrs = outPtrs_[b];
    for (uint32_t c = 0; c < outChannels_[b]; ++c)
      ptrs[c] = (blk && blk->channels && c < blk->channel_count && blk->channels[c]) ? blk->channels[c] : trashOut_.data();
    clap_audio_buffer_t& cb = outBufs_[b];
    cb.data32 = ptrs.data();
    cb.data64 = nullptr;
    cb.channel_count = outChannels_[b];
    cb.latency = 0;
    cb.constant_mask = 0;
  }

  // ---- input events: queued main-thread sets (t=0), then automation + MIDI merged by time ----
  const uint32_t cap = static_cast<uint32_t>(evStore_.size());
  evCount_ = 0;
  auto lastFrame = frames ? frames - 1 : 0;
  auto addParamEvent = [&](uint32_t id, uint32_t time, double value) {
    if (evCount_ >= cap) return;
    EventSlot& s = evStore_[evCount_];
    clap_event_param_value_t& e = s.param;
    e = clap_event_param_value_t{};
    e.header.size = sizeof e;
    e.header.time = std::min(time, lastFrame);
    e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    e.header.type = CLAP_EVENT_PARAM_VALUE;
    e.param_id = id;
    e.cookie = nullptr;
    e.note_id = -1;
    e.port_index = -1;
    e.channel = -1;
    e.key = -1;
    e.value = value;
    uint32_t slot = paramSlot(id);
    if (slot != UINT32_MAX) paramValues_[slot].store(value, std::memory_order_relaxed);
    evList_[evCount_] = &s.header;
    ++evCount_;
  };
  auto addMidiEvent = [&](const sq_midi_event& m) {
    if (evCount_ >= cap || !hasNoteInput_) return;
    EventSlot& s = evStore_[evCount_];
    uint32_t time = std::min(m.frame_offset, lastFrame);
    uint8_t st = m.bytes[0];
    if (m.data || st == 0xF0) {
      clap_event_midi_sysex_t& e = s.sysex;
      e = clap_event_midi_sysex_t{};
      e.header = {sizeof e, time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI_SYSEX, 0};
      e.port_index = 0;
      e.buffer = m.data;
      e.size = m.data ? m.size : 0;
      if (!e.buffer || !e.size) return;
    } else if (noteDialectClap_ && ((st & 0xF0) == 0x90 || (st & 0xF0) == 0x80)) {
      bool on = (st & 0xF0) == 0x90 && m.bytes[2] > 0;
      clap_event_note_t& e = s.note;
      e = clap_event_note_t{};
      e.header = {sizeof e, time, CLAP_CORE_EVENT_SPACE_ID, static_cast<uint16_t>(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
      e.note_id = -1;
      e.port_index = 0;
      e.channel = static_cast<int16_t>(st & 0x0F);
      e.key = static_cast<int16_t>(m.bytes[1] & 0x7F);
      e.velocity = m.bytes[2] / 127.0;
    } else {
      clap_event_midi_t& e = s.midi;
      e = clap_event_midi_t{};
      e.header = {sizeof e, time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
      e.port_index = 0;
      e.data[0] = m.bytes[0];
      e.data[1] = m.bytes[1];
      e.data[2] = m.bytes[2];
    }
    evList_[evCount_] = &s.header;
    ++evCount_;
  };
  ParamSet ps{};
  while (toAudio_.pop(ps)) {
    addParamEvent(ps.id, 0, ps.value);
    if (ps.seq == 0) continue;
    const uint32_t slot = paramSlot(ps.id);
    if (slot == UINT32_MAX) continue;
    if (!paramTakenSeq_[slot]) paramTakenSlots_[paramTakenCount_++] = slot;
    paramTakenSeq_[slot] = ps.seq;  // published once the plugin has processed it (below)
  }
  uint32_t mi = 0, pi = 0;
  const uint32_t mn = d->midi_in ? d->midi_in_count : 0;
  const uint32_t pn = d->params_in ? d->params_in_count : 0;
  while (mi < mn || pi < pn) {
    bool takeParam = pi < pn && (mi >= mn || d->params_in[pi].frame_offset <= d->midi_in[mi].frame_offset);
    if (takeParam) {
      const sq_param_change& c = d->params_in[pi++];
      addParamEvent(c.param_id, c.frame_offset, c.value);
    } else {
      addMidiEvent(d->midi_in[mi++]);
    }
  }

  // ---- transport ----
  clap_event_transport_t tr{};
  const clap_event_transport_t* trp = nullptr;
  if (d->transport) {
    const sq_plugin_transport& t = *d->transport;
    tr.header = {sizeof tr, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
    tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
    if (t.flags & SQ_TRANSPORT_PLAYING) tr.flags |= CLAP_TRANSPORT_IS_PLAYING;
    if (t.flags & SQ_TRANSPORT_RECORDING) tr.flags |= CLAP_TRANSPORT_IS_RECORDING;
    if (t.flags & SQ_TRANSPORT_LOOPING) tr.flags |= CLAP_TRANSPORT_IS_LOOP_ACTIVE;
    tr.song_pos_beats = toBeatTime(t.position);
    tr.tempo = t.bpm;
    tr.loop_start_beats = toBeatTime(t.loop_start);
    tr.loop_end_beats = toBeatTime(t.loop_end);
    tr.bar_start = toBeatTime(t.bar_start);
    uint32_t num = t.numerator ? t.numerator : 4, den = t.denominator ? t.denominator : 4;
    double barLen = num * 4.0 / den;
    tr.bar_number = barLen > 0 ? static_cast<int32_t>(std::floor(t.bar_start / barLen + 1e-9)) : 0;
    tr.tsig_num = static_cast<uint16_t>(num);
    tr.tsig_denom = static_cast<uint16_t>(den);
    trp = &tr;
  }

  clap_process_t proc{};
  proc.steady_time = steadyTime_;
  proc.frames_count = frames;
  proc.transport = trp;
  proc.audio_inputs = inBufs_.empty() ? nullptr : inBufs_.data();
  proc.audio_inputs_count = static_cast<uint32_t>(inBufs_.size());
  proc.audio_outputs = outBufs_.empty() ? nullptr : outBufs_.data();
  proc.audio_outputs_count = static_cast<uint32_t>(outBufs_.size());
  proc.in_events = &inEvents_;
  proc.out_events = &outEvents_;

  curProcess_ = d;
  clap_process_status st = plugin_->process(plugin_, &proc);
  curProcess_ = nullptr;
  steadyTime_ += frames;
  // The plugin has processed the queued sets: getParamValue() may ask it for them now.
  for (uint32_t i = 0; i < paramTakenCount_; ++i) {
    const uint32_t slot = paramTakenSlots_[i];
    paramAppliedSeq_[slot].store(paramTakenSeq_[slot], std::memory_order_release);
    paramTakenSeq_[slot] = 0;
  }
  paramTakenCount_ = 0;
  if (heldCount_) pushHeldEdits();  // as soon as the main thread has made room
  if (st == CLAP_PROCESS_ERROR) return SQ_E_FAIL;
  return st == CLAP_PROCESS_SLEEP ? SQ_S_FALSE : SQ_OK;
}

void ClapInstance::reset() {
  if (!active_) return;
  AudioScope scope;
  plugin_->reset(plugin_);
}

uint32_t ClapInstance::getLatencyFrames() {
  pump();
  return latencyFrames_.load(std::memory_order_acquire);
}

uint32_t ClapInstance::getTailFrames() {
  pump();
  if (!tail_) return 0;
  uint32_t t = tail_->get(plugin_);
  return t >= static_cast<uint32_t>(INT32_MAX) ? UINT32_MAX : t;
}

// ---- parameters --------------------------------------------------------------------------------
uint32_t ClapInstance::getParamCount() {
  pump();
  return params_ ? params_->count(plugin_) : 0;
}

sq_result ClapInstance::getParamInfo(uint32_t index, sq_param_info* out) {
  pump();
  if (!out || out->struct_size < sizeof(sq_param_info)) return SQ_E_INVALIDARG;
  if (!params_ || index >= params_->count(plugin_)) return SQ_E_NOTFOUND;
  clap_param_info_t pi{};
  if (!params_->get_info(plugin_, index, &pi)) return SQ_E_FAIL;
  out->id = pi.id;
  copyStr(out->name, sizeof out->name, pi.name);
  out->unit[0] = 0;
  copyStr(out->group, sizeof out->group, pi.module);
  out->min_value = pi.min_value;
  out->max_value = pi.max_value;
  out->default_value = pi.default_value;
  uint32_t f = 0;
  if (pi.flags & CLAP_PARAM_IS_AUTOMATABLE) f |= SQ_PARAM_AUTOMATABLE;
  if (pi.flags & CLAP_PARAM_IS_STEPPED) f |= SQ_PARAM_STEPPED;
  if (pi.flags & CLAP_PARAM_IS_BYPASS) f |= SQ_PARAM_BYPASS;
  if (pi.flags & CLAP_PARAM_IS_READONLY) f |= SQ_PARAM_READONLY;
  if (pi.flags & CLAP_PARAM_IS_HIDDEN) f |= SQ_PARAM_HIDDEN;
  out->flags = f;
  // Same convention as VST3: number of steps between min and max (a toggle has 1).
  out->step_count = (pi.flags & CLAP_PARAM_IS_STEPPED) ? static_cast<uint32_t>(std::lround(pi.max_value - pi.min_value)) : 0;
  return SQ_OK;
}

sq_result ClapInstance::getParamValue(uint32_t paramId, double* value) {
  pump();
  if (!value) return SQ_E_INVALIDARG;
  if (!params_) return SQ_E_NOTFOUND;
  if (active_) {
    // A set the plugin has not processed yet is the value it is about to have.
    const uint32_t slot = paramSlot(paramId);
    if (slot != UINT32_MAX && paramAppliedSeq_[slot].load(std::memory_order_acquire) != paramQueuedSeq_[slot]) {
      *value = paramQueuedValue_[slot];
      return SQ_OK;
    }
  }
  return params_->get_value(plugin_, paramId, value) ? SQ_OK : SQ_E_NOTFOUND;
}

void ClapInstance::flushParamsMainThread(const ParamSet* sets, uint32_t count, bool reportEdits) {
  if (!params_) return;
  std::vector<EventSlot> store(count ? count : 1);
  std::vector<const clap_event_header_t*> list(count ? count : 1);
  for (uint32_t i = 0; i < count; ++i) {
    clap_event_param_value_t& e = store[i].param;
    e = clap_event_param_value_t{};
    e.header = {sizeof e, 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
    e.param_id = sets[i].id;
    e.note_id = -1;
    e.port_index = -1;
    e.channel = -1;
    e.key = -1;
    e.value = sets[i].value;
    list[i] = &store[i].header;
  }
  // Temporarily point the input list at this local batch.
  auto savedList = std::move(evList_);
  uint32_t savedCount = evCount_;
  evList_ = std::move(list);
  evCount_ = count;
  std::vector<sq_param_change> edits;
  flushEdits_ = &edits;
  params_->flush(plugin_, &inEvents_, &outEvents_);
  flushEdits_ = nullptr;
  evList_ = std::move(savedList);
  evCount_ = savedCount;
  if (reportEdits && cb_)
    for (const sq_param_change& c : edits) cb_->onParamEdited(this, &c);
}

sq_result ClapInstance::setParamValue(uint32_t paramId, double value) {
  pump();
  if (!params_) return SQ_E_NOTFOUND;
  if (std::isnan(value)) return SQ_E_INVALIDARG;
  if (active_) {
    const uint32_t slot = paramSlot(paramId);
    uint32_t seq = 0;  // 0 = untracked (an id the parameter table does not know)
    if (slot != UINT32_MAX) seq = paramQueuedSeq_[slot] == UINT32_MAX ? 1u : paramQueuedSeq_[slot] + 1;
    if (!toAudio_.push(ParamSet{paramId, value, seq})) return SQ_E_BUSY;
    if (slot != UINT32_MAX) {
      paramQueuedValue_[slot] = value;
      paramQueuedSeq_[slot] = seq;
    }
    return SQ_OK;
  }
  ParamSet ps{paramId, value};
  flushParamsMainThread(&ps, 1, false);
  return SQ_OK;
}

sq_result ClapInstance::paramValueToText(uint32_t paramId, double value, char* buf, uint32_t bufSize, uint32_t* needed) {
  pump();
  if (!params_ || !params_->value_to_text) return SQ_E_NOTFOUND;
  char tmp[512] = {};
  if (!params_->value_to_text(plugin_, paramId, value, tmp, sizeof tmp)) return SQ_E_FAIL;
  uint32_t n = static_cast<uint32_t>(std::strlen(tmp)) + 1;
  if (needed) *needed = n;
  if (!buf || bufSize < n) return SQ_E_BUFFER_TOO_SMALL;
  std::memcpy(buf, tmp, n);
  return SQ_OK;
}

// ---- state -------------------------------------------------------------------------------------
sq_result ClapInstance::saveState(sq::IByteStream* out) {
  pump();
  if (!out) return SQ_E_INVALIDARG;
  if (!state_) return SQ_E_UNSUPPORTED;
  clap_ostream_t os{out, &ostreamWrite};
  return state_->save(plugin_, &os) ? SQ_OK : SQ_E_FAIL;
}

sq_result ClapInstance::loadState(sq::IByteStream* in) {
  pump();
  if (!in) return SQ_E_INVALIDARG;
  if (!state_) return SQ_E_UNSUPPORTED;
  clap_istream_t is{in, &istreamRead};
  bool ok = state_->load(plugin_, &is);
  pending_.fetch_or(kPendParamsRescan, std::memory_order_acq_rel);  // values changed
  return ok ? SQ_OK : SQ_E_FAIL;
}

// ---- editor ------------------------------------------------------------------------------------
sq_bool ClapInstance::hasEditor() {
  pump();
  return gui_ && gui_->is_api_supported(plugin_, nativeWindowApi(), false) ? SQ_TRUE : SQ_FALSE;
}

sq_result ClapInstance::openEditor(void* parent, const char* platform, uint32_t* width, uint32_t* height) {
  pump();
  if (!gui_) return SQ_E_UNSUPPORTED;
  if (editorOpen_) return SQ_S_FALSE;
  const char* api = nullptr;
  if (platform && !std::strcmp(platform, "win32")) api = CLAP_WINDOW_API_WIN32;
  else if (platform && !std::strcmp(platform, "cocoa")) api = CLAP_WINDOW_API_COCOA;
  else if (platform && !std::strcmp(platform, "x11")) api = CLAP_WINDOW_API_X11;
  if (!api || !parent) return SQ_E_INVALIDARG;
  if (!gui_->is_api_supported(plugin_, api, false)) return SQ_E_UNSUPPORTED;
  if (!gui_->create(plugin_, api, false)) return SQ_E_FAIL;
  clap_window_t w{};
  w.api = api;
  w.ptr = parent;
  if (!gui_->set_parent(plugin_, &w)) {
    gui_->destroy(plugin_);
    return SQ_E_FAIL;
  }
  uint32_t ww = 0, hh = 0;
  gui_->get_size(plugin_, &ww, &hh);
  if (width) *width = ww;
  if (height) *height = hh;
  gui_->show(plugin_);
  editorOpen_ = true;
  return SQ_OK;
}

sq_result ClapInstance::closeEditor() {
  pump();
  if (!gui_ || !editorOpen_) return SQ_S_FALSE;
  gui_->hide(plugin_);
  gui_->destroy(plugin_);
  editorOpen_ = false;
  return SQ_OK;
}

sq_result ClapInstance::setEditorSize(uint32_t width, uint32_t height) {
  pump();
  if (!gui_ || !editorOpen_) return SQ_E_FAIL;
  if (!gui_->can_resize(plugin_)) return SQ_E_UNSUPPORTED;
  uint32_t w = width, h = height;
  gui_->adjust_size(plugin_, &w, &h);
  return gui_->set_size(plugin_, w, h) ? SQ_OK : SQ_E_FAIL;
}

}  // namespace sqclap
