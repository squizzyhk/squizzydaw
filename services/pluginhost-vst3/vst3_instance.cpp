// pluginhost-vst3: IPluginInstance over a VST3 component (+ edit controller).
//
// Threading (see sq/interfaces/pluginhost.h):
//  - The instantiating thread is the instance's main (UI) thread; every IPluginInstance method
//    except process()/reset() runs there, and so does every IComponent / IEditController call.
//  - process()/reset() run on the audio thread without allocating or locking: buses, parameter
//    queues, event lists, the sysex arena and the plain<->normalized conversion tables are built
//    in activate(). Main-thread parameter sets and controller edits travel to the audio thread in
//    an SPSC ring (producers serialised by a spinlock, never taken on the audio thread); output
//    parameter changes travel back in a second ring and are applied to the controller by pump().
//  - Plugin -> host requests (IComponentHandler, IPlugFrame, messages sent off the main thread)
//    are queued and delivered to the IPluginHostCallback from the next main-thread call into the
//    instance (pump()), never re-entrantly from inside a plugin call. The interface has no idle
//    hook: the engine should call a cheap main-thread method (getLatencyFrames) periodically.
//  - Parameter values crossing IPluginInstance are plain; VST3 is normalized. The main thread
//    converts through the controller; the audio thread uses tables sampled from the controller
//    in activate() (exact for linear and stepped parameters, interpolated otherwise).
#include "vst3_instance.hpp"

#include <pluginterfaces/vst/vstspeaker.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "sq/cpp/bytestream.hpp"
#include "sq/interfaces/audioengine.h"

namespace sqvst3 {

using namespace Steinberg;

namespace {

constexpr int32_t kCtrlCount = 130;  // MIDI CC 0..127 + channel pressure (128) + pitch bend (129)
constexpr int16_t kCtrlAfterTouch = 128;
constexpr int16_t kCtrlPitchBend = 129;
constexpr int16_t kCtrlProgramChange = 130;
constexpr uint32_t kStateMagic = 0x33565153u;  // "SQV3" little endian
constexpr uint32_t kStateVersion = 1;

V::SpeakerArrangement arrangementFor(uint32_t channels) {
  if (channels == 1) return V::SpeakerArr::kMono;
  if (channels == 2) return V::SpeakerArr::kStereo;
  if (channels == 0) return V::SpeakerArr::kEmpty;
  return channels >= 64 ? ~0ull : ((1ull << channels) - 1ull);
}

// NaN maps to 0 (a plugin's conversion function may return NaN for odd input).
double clamp01(double v) { return v > 0.0 ? (v < 1.0 ? v : 1.0) : 0.0; }

void putU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Reads a whole sq::IByteStream (from its current position) into memory.
bool readAll(sq::IByteStream* in, std::vector<uint8_t>& out) {
  uint8_t chunk[16384];
  for (;;) {
    uint32_t got = 0;
    if (SQ_FAILED(in->read(chunk, sizeof chunk, &got))) return false;
    if (!got) return true;
    out.insert(out.end(), chunk, chunk + got);
    if (out.size() > (1ull << 31)) return false;  // refuse absurd state sizes
  }
}

const std::vector<uint8_t>& memBytes(const sq::Ptr<sq::IByteStream>& s) { return static_cast<sq::MemoryStream*>(s.get())->bytes(); }

// Plugin state -> memory through the IBStream adapter.
bool captureState(FUnknown* obj, bool component, std::vector<uint8_t>& out) {
  auto mem = sq::makeMemoryStream();
  IPtr<StreamAdapter> st = owned(new StreamAdapter(mem.get()));
  tresult r = kResultFalse;
  if (component) {
    if (auto c = queryAs<V::IComponent>(obj)) r = c->getState(st);
  } else if (auto e = queryAs<V::IEditController>(obj)) {
    r = e->getState(st);
  }
  if (r != kResultOk) return false;
  out = memBytes(mem);
  return true;
}

IPtr<StreamAdapter> streamOver(const uint8_t* data, size_t size, sq::Ptr<sq::IByteStream>& keep) {
  keep = sq::makeMemoryStream(data, size, false);
  return owned(new StreamAdapter(keep.get()));
}

uint32_t typeFlagsFromCategory(const std::string& cat) {
  uint32_t f = 0;
  size_t start = 0;
  while (start <= cat.size()) {
    size_t e = cat.find('|', start);
    std::string tok = cat.substr(start, e == std::string::npos ? std::string::npos : e - start);
    if (tok == "Fx") f |= SQ_PLUGIN_EFFECT;
    else if (tok == "Instrument") f |= SQ_PLUGIN_INSTRUMENT;
    else if (tok == "Analyzer") f |= SQ_PLUGIN_ANALYZER;
    if (e == std::string::npos) break;
    start = e + 1;
  }
  return f;
}

// Bus summary of a component into a desc (scanner and instance share it).
void describeBuses(V::IComponent* comp, sq_plugin_desc* out) {
  out->audio_inputs = out->audio_outputs = out->aux_inputs = 0;
  for (int dir = 0; dir < 2; ++dir) {
    V::BusDirection bd = dir == 0 ? V::kInput : V::kOutput;
    int32 n = comp->getBusCount(V::kAudio, bd);
    bool mainSeen = false;
    for (int32 i = 0; i < n; ++i) {
      V::BusInfo bi{};
      if (comp->getBusInfo(V::kAudio, bd, i, bi) != kResultOk) continue;
      bool main = bi.busType == V::kMain && !mainSeen;
      if (main) mainSeen = true;
      if (dir == 0) {
        if (main) out->audio_inputs = static_cast<uint32_t>(std::max<int32>(bi.channelCount, 0));
        else ++out->aux_inputs;
      } else if (main) {
        out->audio_outputs = static_cast<uint32_t>(std::max<int32>(bi.channelCount, 0));
      }
    }
  }
  out->midi_inputs = static_cast<uint32_t>(std::max<int32>(comp->getBusCount(V::kEvent, V::kInput), 0));
  out->midi_outputs = static_cast<uint32_t>(std::max<int32>(comp->getBusCount(V::kEvent, V::kOutput), 0));
  if (out->aux_inputs) out->flags |= SQ_PLUGIN_SIDECHAIN;
  if (!(out->flags & (SQ_PLUGIN_INSTRUMENT | SQ_PLUGIN_EFFECT | SQ_PLUGIN_MIDI_EFFECT | SQ_PLUGIN_ANALYZER))) {
    if (out->audio_inputs) out->flags |= SQ_PLUGIN_EFFECT;
    else if (out->midi_inputs && out->audio_outputs) out->flags |= SQ_PLUGIN_INSTRUMENT;
    else if (out->midi_inputs && out->midi_outputs) out->flags |= SQ_PLUGIN_MIDI_EFFECT;
  }
}

bool viewSupported(IPlugView* v) { return v && v->isPlatformTypeSupported(nativeViewType()) == kResultTrue; }

}  // namespace

const char* nativeViewType() {
#if defined(_WIN32)
  return kPlatformTypeHWND;
#elif defined(__APPLE__)
  return kPlatformTypeNSView;
#else
  return kPlatformTypeX11EmbedWindowID;
#endif
}

// ---- scanner ------------------------------------------------------------------------------------
sq_result describeClass(Module& mod, int32_t index, sq_plugin_desc* out) {
  IPluginFactory* f = mod.factory();
  PClassInfo ci{};
  if (f->getClassInfo(index, &ci) != kResultOk) return SQ_E_FAIL;
  if (std::strncmp(ci.category, kVstAudioEffectClass, PClassInfo::kCategorySize) != 0) return SQ_S_FALSE;
  copyStr(out->format, sizeof out->format, "vst3");
  copyStr(out->id, sizeof out->id, cidToString(ci.cid).c_str());
  std::string name = fieldStr(ci.name), vendor, version, cats;
  PClassInfoW ciw{};
  PClassInfo2 ci2{};
  if (mod.factory3() && mod.factory3()->getClassInfoUnicode(index, &ciw) == kResultOk) {
    std::string n = toUtf8(ciw.name);
    if (!n.empty()) name = n;
    vendor = toUtf8(ciw.vendor);
    version = toUtf8(ciw.version);
    cats = fieldStr(ciw.subCategories);
  } else if (mod.factory2() && mod.factory2()->getClassInfo2(index, &ci2) == kResultOk) {
    vendor = fieldStr(ci2.vendor);
    version = fieldStr(ci2.version);
    cats = fieldStr(ci2.subCategories);
  }
  if (vendor.empty()) vendor = mod.factoryVendor();
  copyStr(out->name, sizeof out->name, name.c_str());
  copyStr(out->vendor, sizeof out->vendor, vendor.c_str());
  copyStr(out->version, sizeof out->version, version.c_str());
  copyStr(out->category, sizeof out->category, cats.c_str());
  out->flags = typeFlagsFromCategory(cats);
  out->status = SQ_PLUGIN_STATUS_INVALID;

  // Probe: create + initialise the component for its buses, the controller for the editor flag.
  V::IComponent* raw = nullptr;
  if (f->createInstance(ci.cid, V::IComponent::iid, reinterpret_cast<void**>(&raw)) != kResultOk || !raw) return SQ_OK;
  IPtr<V::IComponent> comp = owned(raw);
  FUnknown* ctx = static_cast<V::IHostApplication*>(mod.hostApp());
  if (comp->initialize(ctx) != kResultOk) return SQ_OK;
  describeBuses(comp, out);
  out->status = SQ_PLUGIN_STATUS_OK;
  IPtr<V::IEditController> ctrl = queryAs<V::IEditController>(comp);
  bool separate = false;
  if (!ctrl) {
    TUID ccid = {};
    V::IEditController* ec = nullptr;
    if (comp->getControllerClassId(ccid) == kResultOk &&
        f->createInstance(ccid, V::IEditController::iid, reinterpret_cast<void**>(&ec)) == kResultOk && ec) {
      ctrl = owned(ec);
      if (ctrl->initialize(ctx) == kResultOk) separate = true;
      else ctrl = nullptr;
    }
  }
  if (ctrl) {
    if (IPlugView* v = ctrl->createView(V::ViewType::kEditor)) {
      if (viewSupported(v)) out->flags |= SQ_PLUGIN_HAS_EDITOR;
      v->release();
    }
    if (separate) ctrl->terminate();
  }
  comp->terminate();
  return SQ_OK;
}

// ---- handler / frame ------------------------------------------------------------------------------
tresult PLUGIN_API ComponentHandler::beginEdit(V::ParamID id) {
  if (auto* o = owner_.load(std::memory_order_acquire)) o->handlerEdit(id, 0.0, SQ_PARAM_GESTURE_BEGIN, false);
  return kResultOk;
}
tresult PLUGIN_API ComponentHandler::performEdit(V::ParamID id, V::ParamValue valueNormalized) {
  if (auto* o = owner_.load(std::memory_order_acquire)) o->handlerEdit(id, valueNormalized, 0, true);
  return kResultOk;
}
tresult PLUGIN_API ComponentHandler::endEdit(V::ParamID id) {
  if (auto* o = owner_.load(std::memory_order_acquire)) o->handlerEdit(id, 0.0, SQ_PARAM_GESTURE_END, false);
  return kResultOk;
}
tresult PLUGIN_API ComponentHandler::restartComponent(int32 flags) {
  if (auto* o = owner_.load(std::memory_order_acquire)) o->handlerRestart(flags);
  return kResultOk;
}
tresult PLUGIN_API ComponentHandler::setDirty(TBool state) {
  if (state)
    if (auto* o = owner_.load(std::memory_order_acquire)) o->handlerDirty();
  return kResultOk;
}
tresult PLUGIN_API PlugFrame::resizeView(IPlugView* view, ViewRect* newSize) {
  if (!owner_ || !view || !newSize) return kInvalidArgument;
  return owner_->frameResize(view, newSize) ? kResultOk : kResultFalse;
}

// ---- lifecycle ----------------------------------------------------------------------------------
Vst3Instance::Vst3Instance() { desc_.struct_size = sizeof desc_; }

Vst3Instance::~Vst3Instance() {
  // The refcount is already 0: no host callback may see this object any more (a callback that
  // add-refs and releases its argument would delete it a second time). shutdownPlugin() pumps.
  cb_ = nullptr;
  shutdownPlugin();
  mod_.reset();
}

void Vst3Instance::shutdownPlugin() {
  if (view_) closeEditor();
  if (active_) deactivate();
  if (compCP_ && toCtrl_) compCP_->disconnect(toCtrl_);
  if (ctrlCP_ && toComp_) ctrlCP_->disconnect(toComp_);
  if (toCtrl_) toCtrl_->close();
  if (toComp_) toComp_->close();
  if (controller_) controller_->setComponentHandler(nullptr);
  if (handler_) handler_->detach();
  if (frame_) frame_->detach();
  midiMapping_ = nullptr;
  if (controller_ && controllerSeparate_) controller_->terminate();
  controller_ = nullptr;
  processor_ = nullptr;
  if (component_) component_->terminate();
  component_ = nullptr;
  compCP_ = ctrlCP_ = nullptr;
  toCtrl_ = toComp_ = nullptr;
  handler_ = nullptr;
  frame_ = nullptr;
}

sq_result Vst3Instance::create(std::shared_ptr<Module> mod, const sq_plugin_desc* desc, sq::IPluginHostCallback* cb,
                               sq::IPluginInstance** out) {
  if (!mod || !desc || !out) return SQ_E_INVALIDARG;
  *out = nullptr;
  TUID cid = {};
  if (!cidFromString(desc->id, cid)) return SQ_E_INVALIDARG;
  sq::Ptr<Vst3Instance> inst = sq::Ptr<Vst3Instance>::adopt(new (std::nothrow) Vst3Instance());
  if (!inst) return SQ_E_OUTOFMEMORY;
  inst->mainThread_ = sq_thread_current_id();
  inst->mod_ = mod;
  inst->cb_ = sq::Ptr<sq::IPluginHostCallback>(cb);
  IPluginFactory* f = mod->factory();
  FUnknown* ctx = static_cast<V::IHostApplication*>(mod->hostApp());

  V::IComponent* raw = nullptr;
  if (f->createInstance(cid, V::IComponent::iid, reinterpret_cast<void**>(&raw)) != kResultOk || !raw) return SQ_E_NOTFOUND;
  inst->component_ = owned(raw);
  if (inst->component_->initialize(ctx) != kResultOk) {
    inst->component_ = nullptr;  // not initialised: no terminate()
    return SQ_E_FAIL;
  }
  inst->processor_ = queryAs<V::IAudioProcessor>(inst->component_);
  if (!inst->processor_) return SQ_E_UNSUPPORTED;

  // Edit controller: the component itself (single component) or a separate class.
  inst->controller_ = queryAs<V::IEditController>(inst->component_);
  if (!inst->controller_) {
    TUID ccid = {};
    V::IEditController* ec = nullptr;
    if (inst->component_->getControllerClassId(ccid) == kResultOk &&
        f->createInstance(ccid, V::IEditController::iid, reinterpret_cast<void**>(&ec)) == kResultOk && ec) {
      IPtr<V::IEditController> c = owned(ec);
      if (c->initialize(ctx) == kResultOk) {
        inst->controller_ = c;
        inst->controllerSeparate_ = true;
      }
    }
  }
  if (inst->controller_) {
    inst->handler_ = owned(new ComponentHandler(inst.get()));
    inst->controller_->setComponentHandler(inst->handler_);
    if (inst->controllerSeparate_) {
      // Connect through host proxies (messages are marshalled to the main thread).
      inst->compCP_ = queryAs<V::IConnectionPoint>(inst->component_);
      inst->ctrlCP_ = queryAs<V::IConnectionPoint>(inst->controller_);
      if (inst->compCP_ && inst->ctrlCP_) {
        inst->toCtrl_ = owned(new ConnectionProxy(inst->ctrlCP_, inst->mainThread_));
        inst->toComp_ = owned(new ConnectionProxy(inst->compCP_, inst->mainThread_));
        inst->compCP_->connect(inst->toCtrl_);
        inst->ctrlCP_->connect(inst->toComp_);
      }
      // Synchronise the controller with the component's initial state.
      std::vector<uint8_t> st;
      if (captureState(inst->component_, true, st)) {
        sq::Ptr<sq::IByteStream> keep;
        inst->controller_->setComponentState(streamOver(st.data(), st.size(), keep));
      }
    }
    inst->midiMapping_ = queryAs<V::IMidiMapping>(inst->controller_);
  }
  inst->scanBuses();
  inst->refreshParamIds();

  inst->desc_ = *desc;
  inst->desc_.struct_size = sizeof inst->desc_;
  copyStr(inst->desc_.format, sizeof inst->desc_.format, "vst3");
  copyStr(inst->desc_.path, sizeof inst->desc_.path, mod->path().c_str());
  inst->desc_.flags &= ~static_cast<uint32_t>(SQ_PLUGIN_SIDECHAIN);
  describeBuses(inst->component_, &inst->desc_);
  inst->desc_.status = SQ_PLUGIN_STATUS_OK;
  inst->pump();
  *out = inst.detach();
  return SQ_OK;
}

void Vst3Instance::scanBuses() {
  for (int dir = 0; dir < 2; ++dir) {
    V::BusDirection bd = dir == 0 ? V::kInput : V::kOutput;
    auto& buses = dir == 0 ? inBuses_ : outBuses_;
    buses.clear();
    int32 n = component_->getBusCount(V::kAudio, bd);
    for (int32 i = 0; i < n; ++i) {
      V::BusInfo bi{};
      Bus b;
      if (component_->getBusInfo(V::kAudio, bd, i, bi) == kResultOk) {
        b.name = toUtf8(bi.name);
        b.channels = static_cast<uint32_t>(std::max<int32>(bi.channelCount, 0));
        b.main = bi.busType == V::kMain;
      }
      V::SpeakerArrangement arr = 0;
      b.arr = processor_->getBusArrangement(bd, i, arr) == kResultOk ? arr : arrangementFor(b.channels);
      // Contract: main buses start active, aux (sidechain) buses inactive.
      b.active = b.main;
      buses.push_back(std::move(b));
    }
  }
  eventIns_ = std::max<int32>(component_->getBusCount(V::kEvent, V::kInput), 0);
  eventOuts_ = std::max<int32>(component_->getBusCount(V::kEvent, V::kOutput), 0);
}

void Vst3Instance::refreshBuses() {
  if (active_ || !component_) return;
  busesStale_ = false;
  try {
    // Keep the host's activation choices for buses that kept their index and kind.
    std::vector<Bus> oldIn = inBuses_, oldOut = outBuses_;
    scanBuses();
    auto keep = [](std::vector<Bus>& now, const std::vector<Bus>& before) {
      for (size_t i = 0; i < now.size() && i < before.size(); ++i)
        if (now[i].main == before[i].main) now[i].active = before[i].active;
    };
    keep(inBuses_, oldIn);
    keep(outBuses_, oldOut);
    uint32_t keepFlags = desc_.flags & ~static_cast<uint32_t>(SQ_PLUGIN_SIDECHAIN);
    desc_.flags = keepFlags;
    describeBuses(component_, &desc_);
  } catch (...) {
    busesStale_ = true;  // try again on the next main-thread call
  }
}

void Vst3Instance::refreshParamIds() {
  paramIds_.clear();
  if (!controller_) return;
  try {
    int32 n = controller_->getParameterCount();
    paramIds_.reserve(static_cast<size_t>(std::max<int32>(n, 0)));
    for (int32 i = 0; i < n; ++i) {
      V::ParameterInfo pi{};
      if (controller_->getParameterInfo(i, pi) == kResultOk) paramIds_.push_back(pi.id);
    }
    std::sort(paramIds_.begin(), paramIds_.end());
  } catch (...) {
    paramIds_.clear();
  }
}

bool Vst3Instance::hasParam(V::ParamID id) const { return std::binary_search(paramIds_.begin(), paramIds_.end(), id); }

bool Vst3Instance::isMainThread() const { return sq_thread_current_id() == mainThread_; }

bool Vst3Instance::queueSet(V::ParamID id, double norm) {
  if (active_ || !isMainThread()) return pushToAudio(id, norm);
  // Inactive: no audio thread drains the ring, so coalesce per id (a preset load on a plugin with
  // thousands of parameters must not overflow). Delivered by the first process() calls.
  for (ParamSet& p : pendingSets_)
    if (p.id == id) {
      p.norm = norm;
      return true;
    }
  try {
    pendingSets_.push_back(ParamSet{id, norm});
  } catch (...) {
    return false;
  }
  return true;
}

// ---- plugin -> host -----------------------------------------------------------------------------
void Vst3Instance::handlerEdit(V::ParamID id, double norm, uint32_t gestureFlags, bool perform) {
  if (perform) queueSet(id, norm);
  std::lock_guard<std::mutex> l(editsMutex_);
  try {
    edits_.push_back(Edit{id, norm, gestureFlags | (perform ? 0x80000000u : 0u)});
  } catch (...) {
  }
}

void Vst3Instance::handlerRestart(int32_t flags) {
  uint32_t p = 0;
  if (flags & V::kLatencyChanged) p |= kPendLatency;
  if (flags & (V::kParamValuesChanged | V::kParamTitlesChanged | V::kParamIDMappingChanged)) p |= kPendParams;
  if (flags & (V::kReloadComponent | V::kIoChanged | V::kRoutingInfoChanged)) p |= kPendRestart;
  pending_.fetch_or(p, std::memory_order_acq_rel);
}

void Vst3Instance::handlerDirty() { pending_.fetch_or(kPendDirty, std::memory_order_acq_rel); }

bool Vst3Instance::frameResize(IPlugView* view, ViewRect* r) {
  if (!isMainThread() || view != view_.get()) return false;
  uint32_t w = static_cast<uint32_t>(std::max<int32>(r->getWidth(), 0));
  uint32_t h = static_cast<uint32_t>(std::max<int32>(r->getHeight(), 0));
  bool ok = cb_ && cb_->onEditorResize(this, w, h) != SQ_FALSE;
  if (ok) view->onSize(r);
  return ok;
}

void Vst3Instance::pump() {
  if (pumping_ || !component_ || !isMainThread()) return;
  pumping_ = true;
  if (toCtrl_) toCtrl_->deliverPending();
  if (toComp_) toComp_->deliverPending();
  // Output parameter changes of the processor -> controller (keeps the editor in sync).
  ParamSet ps{};
  while (fromAudio_.pop(ps))
    if (controller_) controller_->setParamNormalized(ps.id, ps.norm);
  std::vector<Edit> edits;
  {
    std::lock_guard<std::mutex> l(editsMutex_);
    edits.swap(edits_);
  }
  if (cb_) {
    for (const Edit& e : edits) {
      double norm = e.norm;
      if (!(e.flags & 0x80000000u) && controller_) norm = controller_->getParamNormalized(e.id);  // gesture: current value
      sq_param_change c{e.id, 0, normToPlainMain(e.id, norm), e.flags & ~0x80000000u};
      cb_->onParamEdited(this, &c);
    }
  }
  uint32_t p = pending_.exchange(0, std::memory_order_acq_rel);
  if (p & kPendParams) refreshParamIds();
  if (p & kPendRestart) busesStale_ = true;  // IO may have changed: re-read once inactive
  if (busesStale_ && !active_) refreshBuses();
  if (p & kPendLatency) {
    latency_.store(processor_->getLatencySamples(), std::memory_order_release);
    if (cb_) cb_->onLatencyChanged(this);
  }
  if ((p & kPendParams) && cb_) cb_->onParamsChanged(this);
  if ((p & kPendDirty) && cb_) cb_->onStateDirty(this);
  if ((p & kPendRestart) && cb_) cb_->onRestartRequested(this);
  pumping_ = false;
}

// ---- buses --------------------------------------------------------------------------------------
sq_result Vst3Instance::getDesc(sq_plugin_desc* out) {
  pump();
  if (!out || out->struct_size < sizeof(sq_plugin_desc)) return SQ_E_INVALIDARG;
  *out = desc_;
  return SQ_OK;
}

uint32_t Vst3Instance::getBusCount(sq_bool input) {
  pump();
  return static_cast<uint32_t>((input ? inBuses_ : outBuses_).size());
}

sq_result Vst3Instance::getBusInfo(sq_bool input, uint32_t index, sq_plugin_bus_info* out) {
  pump();
  if (!out || out->struct_size < sizeof(sq_plugin_bus_info)) return SQ_E_INVALIDARG;
  const auto& buses = input ? inBuses_ : outBuses_;
  if (index >= buses.size()) return SQ_E_NOTFOUND;
  const Bus& b = buses[index];
  copyStr(out->name, sizeof out->name, b.name.c_str());
  out->channels = b.channels;
  out->flags = (b.main ? SQ_BUS_MAIN : SQ_BUS_AUX) | (b.active ? SQ_BUS_ACTIVE : 0u);
  return SQ_OK;
}

sq_result Vst3Instance::setBusActive(sq_bool input, uint32_t index, sq_bool active) {
  pump();
  if (active_) return SQ_E_BUSY;
  auto& buses = input ? inBuses_ : outBuses_;
  if (index >= buses.size()) return SQ_E_NOTFOUND;
  buses[index].active = active != SQ_FALSE;
  return SQ_OK;
}

// ---- parameters: conversion tables --------------------------------------------------------------
double Vst3Instance::plainToNormMain(V::ParamID id, double plain) {
  if (!controller_) return clamp01(plain);
  return clamp01(controller_->plainParamToNormalized(id, plain));
}
double Vst3Instance::normToPlainMain(V::ParamID id, double norm) {
  if (!controller_) return norm;
  return controller_->normalizedParamToPlain(id, clamp01(norm));
}

void Vst3Instance::buildParamMaps() {
  maps_.clear();
  lut_.clear();
  if (!controller_) return;
  int32 n = controller_->getParameterCount();
  for (int32 i = 0; i < n; ++i) {
    V::ParameterInfo pi{};
    if (controller_->getParameterInfo(i, pi) != kResultOk) continue;
    ParamMap m{};
    m.id = pi.id;
    m.steps = std::max<int32>(pi.stepCount, 0);
    m.p0 = controller_->normalizedParamToPlain(pi.id, 0.0);
    m.p1 = controller_->normalizedParamToPlain(pi.id, 1.0);
    m.linear = true;
    double tol = 1e-9 * std::max(1.0, std::fabs(m.p1 - m.p0));
    for (double t : {0.125, 0.25, 0.5, 0.75, 0.875}) {
      double want = m.p0 + t * (m.p1 - m.p0);
      if (std::fabs(controller_->normalizedParamToPlain(pi.id, t) - want) > tol) {
        m.linear = false;
        break;
      }
    }
    if (!m.linear) {
      m.lut = static_cast<uint32_t>(lut_.size());
      for (uint32_t k = 0; k < kLutPoints; ++k)
        lut_.push_back(controller_->normalizedParamToPlain(pi.id, static_cast<double>(k) / (kLutPoints - 1)));
    }
    maps_.push_back(m);
  }
  std::sort(maps_.begin(), maps_.end(), [](const ParamMap& a, const ParamMap& b) { return a.id < b.id; });
}

void Vst3Instance::buildMidiMap() {
  midiMap_.clear();
  if (!midiMapping_ || eventIns_ <= 0) return;
  midiMap_.assign(16 * kCtrlCount, V::kNoParamId);
  bool any = false;
  for (int16 ch = 0; ch < 16; ++ch)
    for (int16 cc = 0; cc < kCtrlCount; ++cc) {
      V::ParamID id = V::kNoParamId;
      if (midiMapping_->getMidiControllerAssignment(0, ch, cc, id) == kResultTrue && id != V::kNoParamId) {
        midiMap_[static_cast<size_t>(ch * kCtrlCount + cc)] = id;
        any = true;
      }
    }
  if (!any) midiMap_.clear();
}

const Vst3Instance::ParamMap* Vst3Instance::findMap(V::ParamID id) const noexcept {
  auto it = std::lower_bound(maps_.begin(), maps_.end(), id, [](const ParamMap& m, V::ParamID v) { return m.id < v; });
  return (it != maps_.end() && it->id == id) ? &*it : nullptr;
}

double Vst3Instance::normToPlainRT(V::ParamID id, double norm) const noexcept {
  const ParamMap* m = findMap(id);
  norm = clamp01(norm);
  if (!m) return norm;
  if (m->linear) return m->p0 + norm * (m->p1 - m->p0);
  double x = norm * (kLutPoints - 1);
  uint32_t i = std::min<uint32_t>(static_cast<uint32_t>(x), kLutPoints - 2);
  double t = x - i;
  const double* l = lut_.data() + m->lut;
  return l[i] + t * (l[i + 1] - l[i]);
}

double Vst3Instance::plainToNormRT(V::ParamID id, double plain) const noexcept {
  const ParamMap* m = findMap(id);
  if (!m) return clamp01(plain);
  double n;
  if (m->linear) {
    n = m->p1 != m->p0 ? (plain - m->p0) / (m->p1 - m->p0) : 0.0;
  } else {
    // Monotonic table (rising or falling): find the segment containing `plain`.
    const double* l = lut_.data() + m->lut;
    bool rising = l[kLutPoints - 1] >= l[0];
    n = rising ? (plain <= l[0] ? 0.0 : 1.0) : (plain >= l[0] ? 0.0 : 1.0);
    for (uint32_t i = 0; i + 1 < kLutPoints; ++i) {
      double a = l[i], b = l[i + 1];
      if ((plain >= std::min(a, b) && plain <= std::max(a, b))) {
        double t = b != a ? (plain - a) / (b - a) : 0.0;
        n = (i + t) / (kLutPoints - 1);
        break;
      }
    }
  }
  n = clamp01(n);
  if (m->steps > 0) n = std::round(n * m->steps) / m->steps;
  return n;
}

bool Vst3Instance::pushToAudio(V::ParamID id, double norm) {
  sq_spin_lock(&toAudioLock_);
  bool ok = toAudio_.push(ParamSet{id, norm});
  sq_spin_unlock(&toAudioLock_);
  return ok;
}

// ---- activation --------------------------------------------------------------------------------
sq_result Vst3Instance::activate(const sq_plugin_setup* setup) {
  pump();
  if (!setup || setup->struct_size < sizeof(sq_plugin_setup) || setup->sample_rate <= 0 || setup->max_block_frames == 0)
    return SQ_E_INVALIDARG;
  if (active_) return SQ_S_FALSE;
  if (processor_->canProcessSampleSize(V::kSample32) != kResultTrue) return SQ_E_UNSUPPORTED;

  // Bus arrangements: offer the plugin's current ones, then read back what it accepted.
  std::vector<V::SpeakerArrangement> ins, outs;
  try {
    for (const Bus& b : inBuses_) ins.push_back(b.arr);
    for (const Bus& b : outBuses_) outs.push_back(b.arr);
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
  processor_->setBusArrangements(ins.empty() ? nullptr : ins.data(), static_cast<int32>(ins.size()),
                                 outs.empty() ? nullptr : outs.data(), static_cast<int32>(outs.size()));
  for (int dir = 0; dir < 2; ++dir) {
    auto& buses = dir == 0 ? inBuses_ : outBuses_;
    V::BusDirection bd = dir == 0 ? V::kInput : V::kOutput;
    for (size_t i = 0; i < buses.size(); ++i) {
      V::SpeakerArrangement arr = 0;
      if (processor_->getBusArrangement(bd, static_cast<int32>(i), arr) == kResultOk) {
        buses[i].arr = arr;
        buses[i].channels = static_cast<uint32_t>(V::SpeakerArr::getChannelCount(arr));
      }
      component_->activateBus(V::kAudio, bd, static_cast<int32>(i), buses[i].active ? 1 : 0);
    }
  }
  for (int32 i = 0; i < eventIns_; ++i) component_->activateBus(V::kEvent, V::kInput, i, 1);
  for (int32 i = 0; i < eventOuts_; ++i) component_->activateBus(V::kEvent, V::kOutput, i, 1);

  sampleRate_ = setup->sample_rate;
  maxFrames_ = setup->max_block_frames;
  V::ProcessSetup ps{};
  ps.processMode = (setup->flags & SQ_SETUP_OFFLINE) ? V::kOffline : V::kRealtime;
  ps.symbolicSampleSize = V::kSample32;
  ps.maxSamplesPerBlock = static_cast<int32>(maxFrames_);
  ps.sampleRate = sampleRate_;
  if (processor_->setupProcessing(ps) != kResultOk) return SQ_E_FAIL;

  try {
    auto layout = [this](std::vector<Bus>& buses, std::vector<V::AudioBusBuffers>& bufs, std::vector<std::vector<float*>>& ptrs) {
      bufs.assign(buses.size(), V::AudioBusBuffers{});
      ptrs.assign(buses.size(), {});
      for (size_t i = 0; i < buses.size(); ++i) ptrs[i].assign(std::max<uint32_t>(buses[i].channels, 1), nullptr);
    };
    layout(inBuses_, inBufs_, inPtrs_);
    layout(outBuses_, outBufs_, outPtrs_);
    zeroIn_.assign(maxFrames_, 0.0f);
    trashOut_.assign(maxFrames_, 0.0f);
    inChanges_.prepare(512);
    outChanges_.prepare(512);
    inEvents_.prepare(4096);
    outEvents_.prepare(4096);
    sysexArena_.assign(64 * 1024, 0);
    buildParamMaps();
    buildMidiMap();
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
  pd_ = V::ProcessData{};
  pd_.processMode = ps.processMode;
  pd_.symbolicSampleSize = V::kSample32;
  pd_.numInputs = static_cast<int32>(inBufs_.size());
  pd_.numOutputs = static_cast<int32>(outBufs_.size());
  pd_.inputs = inBufs_.empty() ? nullptr : inBufs_.data();
  pd_.outputs = outBufs_.empty() ? nullptr : outBufs_.data();
  pd_.inputParameterChanges = &inChanges_;
  pd_.outputParameterChanges = &outChanges_;
  pd_.inputEvents = eventIns_ > 0 ? &inEvents_ : nullptr;
  pd_.outputEvents = eventOuts_ > 0 ? &outEvents_ : nullptr;
  pd_.processContext = &ctx_;

  if (component_->setActive(1) != kResultOk) return SQ_E_FAIL;
  pendingCursor_ = 0;
  active_ = true;
  steadyTime_ = 0;
  processor_->setProcessing(1);  // many plugins answer kNotImplemented: not an error
  processing_.store(true, std::memory_order_release);
  latency_.store(processor_->getLatencySamples(), std::memory_order_release);
  pending_.fetch_and(~static_cast<uint32_t>(kPendLatency), std::memory_order_acq_rel);  // just read
  pump();
  return SQ_OK;
}

sq_result Vst3Instance::deactivate() {
  if (!active_) return SQ_S_FALSE;
  if (processing_.exchange(false, std::memory_order_acq_rel)) processor_->setProcessing(0);
  component_->setActive(0);
  active_ = false;
  // Queued parameter sets stay in the ring: they reach the processor with the next process().
  // Pending (inactive-time) sets already delivered are dropped.
  pendingSets_.erase(pendingSets_.begin(), pendingSets_.begin() + static_cast<std::ptrdiff_t>(std::min(pendingCursor_, pendingSets_.size())));
  pendingCursor_ = 0;
  pump();
  return SQ_OK;
}

// ---- audio thread ------------------------------------------------------------------------------
void Vst3Instance::addMidiInput(const sq_midi_event& m) noexcept {
  const int32 time = static_cast<int32>(std::min(m.frame_offset, curFrames_ ? curFrames_ - 1 : 0u));
  const uint8_t st = m.bytes[0], kind = st & 0xF0;
  const int16 ch = static_cast<int16>(st & 0x0F);
  auto ccParam = [&](int16 ctrl, double norm) {
    if (midiMap_.empty()) return;
    V::ParamID id = midiMap_[static_cast<size_t>(ch * kCtrlCount + ctrl)];
    if (id != V::kNoParamId) inChanges_.add(id, time, clamp01(norm));
  };
  if (m.data || st == 0xF0) {
    if (!m.data || !m.size || eventIns_ <= 0) return;
    V::Event e{};
    e.busIndex = 0;
    e.sampleOffset = time;
    e.type = V::Event::kDataEvent;
    e.data.type = V::DataEvent::kMidiSysEx;
    e.data.size = m.size;
    e.data.bytes = m.data;
    inEvents_.addEvent(e);
    return;
  }
  if (kind == 0xB0) return ccParam(static_cast<int16>(m.bytes[1] & 0x7F), (m.bytes[2] & 0x7F) / 127.0);
  if (kind == 0xD0) return ccParam(kCtrlAfterTouch, (m.bytes[1] & 0x7F) / 127.0);
  if (kind == 0xE0) return ccParam(kCtrlPitchBend, (((m.bytes[2] & 0x7F) << 7) | (m.bytes[1] & 0x7F)) / 16383.0);
  if (eventIns_ <= 0) return;
  V::Event e{};
  e.busIndex = 0;
  e.sampleOffset = time;
  if (kind == 0x90 && m.bytes[2] > 0) {
    e.type = V::Event::kNoteOnEvent;
    e.noteOn.channel = ch;
    e.noteOn.pitch = static_cast<int16>(m.bytes[1] & 0x7F);
    e.noteOn.velocity = static_cast<float>((m.bytes[2] & 0x7F) / 127.0);
    e.noteOn.noteId = -1;
  } else if (kind == 0x80 || kind == 0x90) {
    e.type = V::Event::kNoteOffEvent;
    e.noteOff.channel = ch;
    e.noteOff.pitch = static_cast<int16>(m.bytes[1] & 0x7F);
    e.noteOff.velocity = static_cast<float>((m.bytes[2] & 0x7F) / 127.0);
    e.noteOff.noteId = -1;
  } else if (kind == 0xA0) {
    e.type = V::Event::kPolyPressureEvent;
    e.polyPressure.channel = ch;
    e.polyPressure.pitch = static_cast<int16>(m.bytes[1] & 0x7F);
    e.polyPressure.pressure = static_cast<float>((m.bytes[2] & 0x7F) / 127.0);
    e.polyPressure.noteId = -1;
  } else {
    return;  // program change / system messages: no VST3 event equivalent
  }
  inEvents_.addEvent(e);
}

void Vst3Instance::collectOutputs(sq_plugin_process* d) noexcept {
  // Parameter changes made by the processor.
  for (int32_t q = 0; q < outChanges_.getParameterCount(); ++q) {
    ParamValueQueue* pq = outChanges_.queue(q);
    if (!pq) continue;
    int32 n = pq->getPointCount();
    for (int32 i = 0; i < n; ++i) {
      int32 off = 0;
      double norm = 0;
      if (pq->getPoint(i, off, norm) != kResultOk) continue;
      if (d->params_out && d->params_out_count < d->params_out_capacity)
        d->params_out[d->params_out_count++] = sq_param_change{pq->getParameterId(), static_cast<uint32_t>(std::max<int32>(off, 0)), normToPlainRT(pq->getParameterId(), norm), 0};
      if (i == n - 1) fromAudio_.push(ParamSet{pq->getParameterId(), norm});
    }
  }
  // Events.
  auto addMidi = [&](uint32_t time, uint8_t b0, uint8_t b1, uint8_t b2, uint32_t size, const uint8_t* data) {
    if (!d->midi_out || d->midi_out_count >= d->midi_out_capacity) return;
    sq_midi_event& m = d->midi_out[d->midi_out_count++];
    m.frame_offset = time;
    m.bytes[0] = b0;
    m.bytes[1] = b1;
    m.bytes[2] = b2;
    m.bytes[3] = 0;
    m.size = size;
    m.data = data;
  };
  for (int32_t i = 0; i < outEvents_.getEventCount(); ++i) {
    const V::Event* e = outEvents_.at(i);
    if (!e) continue;
    uint32_t t = static_cast<uint32_t>(std::max<int32>(e->sampleOffset, 0));
    auto vel = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 127.0f)); };
    switch (e->type) {
      case V::Event::kNoteOnEvent: {
        uint8_t v = vel(e->noteOn.velocity);
        addMidi(t, static_cast<uint8_t>(0x90 | (e->noteOn.channel & 15)), static_cast<uint8_t>(e->noteOn.pitch & 127), v ? v : 1, 3, nullptr);
        break;
      }
      case V::Event::kNoteOffEvent:
        addMidi(t, static_cast<uint8_t>(0x80 | (e->noteOff.channel & 15)), static_cast<uint8_t>(e->noteOff.pitch & 127), vel(e->noteOff.velocity), 3, nullptr);
        break;
      case V::Event::kPolyPressureEvent:
        addMidi(t, static_cast<uint8_t>(0xA0 | (e->polyPressure.channel & 15)), static_cast<uint8_t>(e->polyPressure.pitch & 127), vel(e->polyPressure.pressure), 3, nullptr);
        break;
      case V::Event::kDataEvent:
        if (e->data.type == V::DataEvent::kMidiSysEx && e->data.bytes && e->data.size && sysexUsed_ + e->data.size <= sysexArena_.size()) {
          uint8_t* dst = sysexArena_.data() + sysexUsed_;
          std::memcpy(dst, e->data.bytes, e->data.size);
          sysexUsed_ += e->data.size;
          addMidi(t, 0xF0, 0, 0, e->data.size, dst);
        }
        break;
      case V::Event::kLegacyMIDICCOutEvent: {
        const auto& c = e->midiCCOut;
        uint8_t ch = static_cast<uint8_t>(c.channel & 15);
        uint8_t v1 = static_cast<uint8_t>(c.value & 127), v2 = static_cast<uint8_t>(c.value2 & 127);
        if (c.controlNumber < 128) addMidi(t, static_cast<uint8_t>(0xB0 | ch), c.controlNumber, v1, 3, nullptr);
        else if (c.controlNumber == kCtrlAfterTouch) addMidi(t, static_cast<uint8_t>(0xD0 | ch), v1, 0, 2, nullptr);
        else if (c.controlNumber == kCtrlPitchBend) addMidi(t, static_cast<uint8_t>(0xE0 | ch), v1, v2, 3, nullptr);
        else if (c.controlNumber == kCtrlProgramChange) addMidi(t, static_cast<uint8_t>(0xC0 | ch), v1, 0, 2, nullptr);
        break;
      }
      default:
        break;
    }
  }
}

sq_result Vst3Instance::process(sq_plugin_process* d) {
  if (!d || d->struct_size < sizeof(sq_plugin_process)) return SQ_E_INVALIDARG;
  if (!active_) return SQ_E_FAIL;
  if (d->frames > maxFrames_) return SQ_E_INVALIDARG;
  if (!processing_.load(std::memory_order_acquire)) {
    processor_->setProcessing(1);
    processing_.store(true, std::memory_order_release);
  }
  const uint32_t frames = d->frames;
  curFrames_ = frames;
  d->midi_out_count = 0;
  d->params_out_count = 0;
  sysexUsed_ = 0;

  // ---- audio buffers (inactive buses: numChannels 0, pointers still valid) ----
  std::memset(zeroIn_.data(), 0, sizeof(float) * frames);
  for (size_t b = 0; b < inBufs_.size(); ++b) {
    const sq_audio_block* blk = b < d->input_bus_count && d->inputs ? &d->inputs[b] : nullptr;
    auto& ptrs = inPtrs_[b];
    uint64 silent = 0;
    for (uint32_t c = 0; c < ptrs.size(); ++c) {
      float* p = (blk && blk->channels && c < blk->channel_count && blk->channels[c]) ? blk->channels[c] : zeroIn_.data();
      ptrs[c] = p;
      bool zero = true;
      for (uint32_t i = 0; i < frames && zero; ++i) zero = p[i] == 0.0f;
      if (zero && c < 64) silent |= 1ull << c;
    }
    V::AudioBusBuffers& ab = inBufs_[b];
    ab.numChannels = inBuses_[b].active ? static_cast<int32>(inBuses_[b].channels) : 0;
    ab.channelBuffers32 = ptrs.data();
    ab.silenceFlags = silent;
  }
  for (size_t b = 0; b < outBufs_.size(); ++b) {
    const sq_audio_block* blk = b < d->output_bus_count && d->outputs ? &d->outputs[b] : nullptr;
    auto& ptrs = outPtrs_[b];
    for (uint32_t c = 0; c < ptrs.size(); ++c)
      ptrs[c] = (blk && blk->channels && c < blk->channel_count && blk->channels[c]) ? blk->channels[c] : trashOut_.data();
    V::AudioBusBuffers& ab = outBufs_[b];
    ab.numChannels = outBuses_[b].active ? static_cast<int32>(outBuses_[b].channels) : 0;
    ab.channelBuffers32 = ptrs.data();
    ab.silenceFlags = 0;
  }

  // ---- parameter changes: queued main-thread sets at 0, then automation (plain -> normalized) ----
  inChanges_.clear();
  outChanges_.clear();
  inEvents_.clear();
  outEvents_.clear();
  // Sets made while inactive first (as many as the queues hold; the rest in the next blocks).
  while (pendingCursor_ < pendingSets_.size() && inChanges_.add(pendingSets_[pendingCursor_].id, 0, clamp01(pendingSets_[pendingCursor_].norm)))
    ++pendingCursor_;
  // Main-thread sets: only removed from the ring once they fit in this block's queues (a burst
  // of more distinct ids than the queues hold continues in the next block instead of being lost).
  ParamSet ps{};
  while (toAudio_.peek(ps) && inChanges_.add(ps.id, 0, clamp01(ps.norm))) toAudio_.skip();
  const int32 last = static_cast<int32>(frames ? frames - 1 : 0);
  if (d->params_in)
    for (uint32_t i = 0; i < d->params_in_count; ++i) {
      const sq_param_change& c = d->params_in[i];
      if (!std::isfinite(c.value)) continue;  // never hand NaN/inf automation to the plugin
      inChanges_.add(c.param_id, static_cast<int32>(std::min<uint32_t>(c.frame_offset, static_cast<uint32_t>(last))),
                     plainToNormRT(c.param_id, c.value));
    }
  if (d->midi_in)
    for (uint32_t i = 0; i < d->midi_in_count; ++i) addMidiInput(d->midi_in[i]);

  // ---- transport ----
  ctx_ = V::ProcessContext{};
  ctx_.sampleRate = sampleRate_;
  ctx_.state = V::ProcessContext::kContTimeValid;
  ctx_.continousTimeSamples = steadyTime_;
  ctx_.projectTimeSamples = steadyTime_;
  ctx_.timeSigNumerator = 4;
  ctx_.timeSigDenominator = 4;
  if (const sq_plugin_transport* t = d->transport) {
    ctx_.state |= V::ProcessContext::kTempoValid | V::ProcessContext::kTimeSigValid | V::ProcessContext::kProjectTimeMusicValid |
                  V::ProcessContext::kBarPositionValid;
    if (t->flags & SQ_TRANSPORT_PLAYING) ctx_.state |= V::ProcessContext::kPlaying;
    if (t->flags & SQ_TRANSPORT_RECORDING) ctx_.state |= V::ProcessContext::kRecording;
    if (t->flags & SQ_TRANSPORT_LOOPING) ctx_.state |= V::ProcessContext::kCycleActive | V::ProcessContext::kCycleValid;
    ctx_.projectTimeSamples = t->frames;
    ctx_.projectTimeMusic = t->position;
    ctx_.barPositionMusic = t->bar_start;
    ctx_.cycleStartMusic = t->loop_start;
    ctx_.cycleEndMusic = t->loop_end;
    ctx_.tempo = t->bpm;
    if (t->numerator) ctx_.timeSigNumerator = static_cast<int32>(t->numerator);
    if (t->denominator) ctx_.timeSigDenominator = static_cast<int32>(t->denominator);
  }

  pd_.numSamples = static_cast<int32>(frames);
  tresult r = processor_->process(pd_);
  steadyTime_ += frames;
  if (r != kResultOk) return SQ_E_FAIL;
  // Keep the controller in step with automation / mapped MIDI controllers (last value per id;
  // best effort: dropped when the ring is full).
  for (int32_t q = 0; q < inChanges_.getParameterCount(); ++q) {
    ParamValueQueue* pq = inChanges_.queue(q);
    int32 off = 0, cnt = pq ? pq->getPointCount() : 0;
    double norm = 0;
    if (cnt > 0 && pq->getPoint(cnt - 1, off, norm) == kResultOk) fromAudio_.push(ParamSet{pq->getParameterId(), norm});
  }
  collectOutputs(d);

  // Silent when every active output channel is flagged silent by the plugin.
  bool anyOut = false, allSilent = true;
  for (const V::AudioBusBuffers& ab : outBufs_) {
    if (ab.numChannels <= 0) continue;
    anyOut = true;
    uint64 mask = ab.numChannels >= 64 ? ~0ull : ((1ull << ab.numChannels) - 1ull);
    if ((ab.silenceFlags & mask) != mask) allSilent = false;
  }
  return anyOut && allSilent ? SQ_S_FALSE : SQ_OK;
}

void Vst3Instance::reset() {
  if (!active_) return;
  // VST3 has no reset call: a setProcessing(false)/(true) pair is the sanctioned way to clear
  // delay lines and reverb tails (allowed on the processing thread).
  processor_->setProcessing(0);
  processor_->setProcessing(1);
  processing_.store(true, std::memory_order_release);
}

uint32_t Vst3Instance::getLatencyFrames() {
  pump();
  return latency_.load(std::memory_order_acquire);
}

uint32_t Vst3Instance::getTailFrames() {
  pump();
  uint32 t = processor_->getTailSamples();
  return t >= static_cast<uint32>(INT32_MAX) ? UINT32_MAX : t;
}

// ---- parameters --------------------------------------------------------------------------------
uint32_t Vst3Instance::getParamCount() {
  pump();
  return controller_ ? static_cast<uint32_t>(std::max<int32>(controller_->getParameterCount(), 0)) : 0;
}

sq_result Vst3Instance::getParamInfo(uint32_t index, sq_param_info* out) {
  pump();
  if (!out || out->struct_size < sizeof(sq_param_info)) return SQ_E_INVALIDARG;
  if (!controller_ || index >= static_cast<uint32_t>(std::max<int32>(controller_->getParameterCount(), 0))) return SQ_E_NOTFOUND;
  V::ParameterInfo pi{};
  if (controller_->getParameterInfo(static_cast<int32>(index), pi) != kResultOk) return SQ_E_FAIL;
  out->id = pi.id;
  try {
    copyStr(out->name, sizeof out->name, toUtf8(pi.title).c_str());
    copyStr(out->unit, sizeof out->unit, toUtf8(pi.units).c_str());
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
  out->group[0] = 0;
  double lo = controller_->normalizedParamToPlain(pi.id, 0.0), hi = controller_->normalizedParamToPlain(pi.id, 1.0);
  out->min_value = std::min(lo, hi);
  out->max_value = std::max(lo, hi);
  out->default_value = controller_->normalizedParamToPlain(pi.id, clamp01(pi.defaultNormalizedValue));
  uint32_t f = 0;
  if (pi.flags & V::ParameterInfo::kCanAutomate) f |= SQ_PARAM_AUTOMATABLE;
  if (pi.stepCount > 0) f |= SQ_PARAM_STEPPED;
  if (pi.flags & V::ParameterInfo::kIsBypass) f |= SQ_PARAM_BYPASS;
  if (pi.flags & V::ParameterInfo::kIsReadOnly) f |= SQ_PARAM_READONLY;
  if (pi.flags & V::ParameterInfo::kIsHidden) f |= SQ_PARAM_HIDDEN;
  out->flags = f;
  out->step_count = static_cast<uint32_t>(std::max<int32>(pi.stepCount, 0));
  return SQ_OK;
}

sq_result Vst3Instance::getParamValue(uint32_t paramId, double* value) {
  pump();
  if (!value) return SQ_E_INVALIDARG;
  if (!controller_ || !hasParam(paramId)) return SQ_E_NOTFOUND;
  *value = controller_->normalizedParamToPlain(paramId, controller_->getParamNormalized(paramId));
  return SQ_OK;
}

sq_result Vst3Instance::setParamValue(uint32_t paramId, double value) {
  pump();
  if (!controller_ || !hasParam(paramId)) return SQ_E_NOTFOUND;
  if (!std::isfinite(value)) return SQ_E_INVALIDARG;
  double norm = plainToNormMain(paramId, value);
  if (controller_->setParamNormalized(paramId, norm) != kResultOk) return SQ_E_NOTFOUND;
  sq_result r = queueSet(paramId, norm) ? SQ_OK : SQ_E_BUSY;
  pump();
  return r;
}

sq_result Vst3Instance::paramValueToText(uint32_t paramId, double value, char* buf, uint32_t bufSize, uint32_t* needed) {
  pump();
  if (!controller_ || !hasParam(paramId)) return SQ_E_NOTFOUND;
  if (!std::isfinite(value)) return SQ_E_INVALIDARG;
  V::String128 s = {};
  if (controller_->getParamStringByValue(paramId, plainToNormMain(paramId, value), s) != kResultOk) return SQ_E_FAIL;
  std::string u;
  try {
    u = toUtf8(s);
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
  uint32_t n = static_cast<uint32_t>(u.size()) + 1;
  if (needed) *needed = n;
  if (!buf || bufSize < n) return SQ_E_BUFFER_TOO_SMALL;
  std::memcpy(buf, u.c_str(), n);
  return SQ_OK;
}

// ---- state -------------------------------------------------------------------------------------
// Container: "SQV3" u32 version, u32 component size, u32 controller size, component bytes,
// controller bytes (little endian). loadState also accepts a bare component state.
sq_result Vst3Instance::saveState(sq::IByteStream* out) {
  pump();
  if (!out) return SQ_E_INVALIDARG;
  std::vector<uint8_t> comp, ctrl;
  try {
    if (!captureState(component_, true, comp)) return SQ_E_FAIL;
    if (controller_ && controllerSeparate_ && !captureState(controller_, false, ctrl)) ctrl.clear();
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
  if (comp.size() > UINT32_MAX || ctrl.size() > UINT32_MAX) return SQ_E_FAIL;
  uint8_t hdr[16];
  putU32(hdr, kStateMagic);
  putU32(hdr + 4, kStateVersion);
  putU32(hdr + 8, static_cast<uint32_t>(comp.size()));
  putU32(hdr + 12, static_cast<uint32_t>(ctrl.size()));
  auto put = [out](const uint8_t* p, size_t n) {
    uint32_t w = 0;
    return n == 0 || (SQ_SUCCEEDED(out->write(p, static_cast<uint32_t>(n), &w)) && w == n);
  };
  if (!put(hdr, sizeof hdr) || !put(comp.data(), comp.size()) || !put(ctrl.data(), ctrl.size())) return SQ_E_IO;
  return SQ_OK;
}

sq_result Vst3Instance::loadState(sq::IByteStream* in) {
  pump();
  if (!in) return SQ_E_INVALIDARG;
  std::vector<uint8_t> all;
  try {
    if (!readAll(in, all)) return SQ_E_IO;
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
  const uint8_t* comp = all.data();
  size_t compLen = all.size(), ctrlLen = 0;
  const uint8_t* ctrl = nullptr;
  if (all.size() >= 16 && getU32(all.data()) == kStateMagic) {
    if (getU32(all.data() + 4) != kStateVersion) return SQ_E_PARSE;
    compLen = getU32(all.data() + 8);
    ctrlLen = getU32(all.data() + 12);
    if (16 + compLen + ctrlLen > all.size()) return SQ_E_PARSE;
    comp = all.data() + 16;
    ctrl = comp + compLen;
  }
  try {
    sq::Ptr<sq::IByteStream> keep;
    if (component_->setState(streamOver(comp, compLen, keep)) != kResultOk) return SQ_E_FAIL;
    if (controller_) {
      controller_->setComponentState(streamOver(comp, compLen, keep));
      if (controllerSeparate_ && ctrlLen) controller_->setState(streamOver(ctrl, ctrlLen, keep));
    }
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
  // Sets made before the load must not override the loaded state at the next activation.
  if (!active_) {
    pendingSets_.clear();
    pendingCursor_ = 0;
  }
  pending_.fetch_or(kPendParams, std::memory_order_acq_rel);  // values changed
  pump();
  return SQ_OK;
}

// ---- editor ------------------------------------------------------------------------------------
sq_bool Vst3Instance::hasEditor() {
  pump();
  if (view_) return SQ_TRUE;
  if (hasEditor_ < 0) {
    hasEditor_ = 0;
    if (controller_)
      if (IPlugView* v = controller_->createView(V::ViewType::kEditor)) {
        hasEditor_ = viewSupported(v) ? 1 : 0;
        v->release();
      }
  }
  return hasEditor_ > 0 ? SQ_TRUE : SQ_FALSE;
}

sq_result Vst3Instance::openEditor(void* parent, const char* platform, uint32_t* width, uint32_t* height) {
  pump();
  if (!controller_) return SQ_E_UNSUPPORTED;
  if (view_) return SQ_S_FALSE;
  FIDString type = nullptr;
  if (platform && !std::strcmp(platform, "win32")) type = kPlatformTypeHWND;
  else if (platform && !std::strcmp(platform, "cocoa")) type = kPlatformTypeNSView;
  else if (platform && !std::strcmp(platform, "x11")) type = kPlatformTypeX11EmbedWindowID;
  if (!type || !parent) return SQ_E_INVALIDARG;
  IPlugView* raw = controller_->createView(V::ViewType::kEditor);
  if (!raw) return SQ_E_UNSUPPORTED;
  IPtr<IPlugView> v = owned(raw);
  if (v->isPlatformTypeSupported(type) != kResultTrue) return SQ_E_UNSUPPORTED;
  if (!frame_) frame_ = owned(new (std::nothrow) PlugFrame(this));
  if (!frame_) return SQ_E_OUTOFMEMORY;
  v->setFrame(frame_);
  view_ = v;  // visible to resizeView() during attached()
  if (v->attached(parent, type) != kResultOk) {
    v->setFrame(nullptr);
    view_ = nullptr;
    return SQ_E_FAIL;
  }
  ViewRect r{};
  if (v->getSize(&r) == kResultOk) {
    if (width) *width = static_cast<uint32_t>(std::max<int32>(r.getWidth(), 0));
    if (height) *height = static_cast<uint32_t>(std::max<int32>(r.getHeight(), 0));
  }
  return SQ_OK;
}

sq_result Vst3Instance::closeEditor() {
  pump();
  if (!view_) return SQ_S_FALSE;
  IPtr<IPlugView> v = view_;
  view_ = nullptr;
  v->removed();
  v->setFrame(nullptr);
  return SQ_OK;
}

sq_result Vst3Instance::setEditorSize(uint32_t width, uint32_t height) {
  pump();
  if (!view_) return SQ_E_FAIL;
  if (view_->canResize() != kResultTrue) return SQ_E_UNSUPPORTED;
  ViewRect r(0, 0, static_cast<int32>(width), static_cast<int32>(height));
  view_->checkSizeConstraint(&r);
  return view_->onSize(&r) == kResultOk ? SQ_OK : SQ_E_FAIL;
}

}  // namespace sqvst3
