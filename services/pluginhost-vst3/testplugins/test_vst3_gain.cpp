// Squizzy Studio VST3 test plugin (original code, written against pluginterfaces only).
//
// "Squizzy Test Gain": stereo effect with
//   - main stereo in/out, a stereo aux "Sidechain" input (inactive by default, mixed in by
//     param 3), one event input and one event output bus;
//   - a pure delay latency of 64 frames (param 4 = 1 -> 128 frames, applied on setActive);
//   - params: 1 Gain dB [-60, 12] (linear), 2 Bypass (toggle), 3 Sidechain mix [0, 1],
//     4 Latency mode (stepped), 5 Peak (read-only, written by the processor every block),
//     6 Drive [1, 10] (non-linear: 1 + 9 n^2, multiplies the output), 100 Host rate (read-only,
//     hidden; set by the controller from the processor's activation message), 101 Edit probe
//     (hidden; the controller edits it through IComponentHandler when that message arrives);
//   - 7 Bank Sum (read-only output: mean of the 600 hidden "Bank" params 1000..1599, which only
//     exist to push more distinct parameter ids through one block than the host queues hold),
//     8 Extra Out (stepped; a change asks for kIoChanged and, applied on setActive(false), adds
//     a second stereo output bus carrying a copy of the main output), 102 Dirty probe (hidden;
//     reading it makes the controller call setDirty, i.e. a plugin-initiated request between
//     host calls);
//   - note on/off events are echoed one octave up on the event output, sysex is echoed as is;
//   - CC 7 (any channel) maps to Gain through IMidiMapping;
//   - processor and controller are separate classes connected through IConnectionPoint;
//   - GetPluginFactory refuses to work unless the module entry (InitDll / ModuleEntry /
//     bundleEntry) was called first.
#define INIT_CLASS_IID
#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivstattributes.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/vst/vstspeaker.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

DECLARE_UID(kProcessorCid, 0x6D2F880B, 0xA9134121, 0xA830C04A, 0x1A7D830E)
DECLARE_UID(kControllerCid, 0x4524CEF3, 0x7E0B4E75, 0x8EE63835, 0x8EE4F562)

enum ParamIds : ParamID {
  kGain = 1,
  kBypass = 2,
  kScMix = 3,
  kLatencyMode = 4,
  kPeak = 5,
  kDrive = 6,
  kBankSum = 7,
  kExtraOut = 8,
  kHostRate = 100,
  kEditProbe = 101,
  kDirtyProbe = 102,
  kBankFirst = 1000,
};
constexpr int32 kBankCount = 600;
constexpr int32 kFixedParams = 11;
constexpr int32 kParamCount = kFixedParams + kBankCount;
bool isBank(ParamID id) { return id >= kBankFirst && id < kBankFirst + static_cast<ParamID>(kBankCount); }
constexpr uint32 kStateMagic = 0x47545153u;  // "SQTG"
constexpr uint32 kCtrlMagic = 0x43545153u;   // "SQTC"
constexpr const char* kMsgActivated = "sq.test.activated";
constexpr double kGainDefaultNorm = 60.0 / 72.0;  // 0 dB

std::atomic<int> gEntered{0};

bool sameId(const TUID a, const TUID b) { return std::memcmp(a, b, sizeof(TUID)) == 0; }
bool iidIs(const TUID a, const FUID& b) { return FUnknownPrivate::iidEqual(a, b.toTUID()); }

void wide(const char* s, char16* out, size_t cap) {
  size_t i = 0;
  for (; s && s[i] && i + 1 < cap; ++i) out[i] = static_cast<char16>(static_cast<unsigned char>(s[i]));
  out[i] = 0;
}

// ---- tiny refcounted base ----
template <class... Ifaces>
class Unknown : public Ifaces... {
 public:
  uint32 PLUGIN_API addRef() override { return ++refs_; }
  uint32 PLUGIN_API release() override {
    uint32 r = --refs_;
    if (!r) delete this;
    return r;
  }
  virtual ~Unknown() = default;

 protected:
  template <class I>
  bool answer(const TUID riid, void** obj, I* self) {
    if (!iidIs(riid, I::iid)) return false;
    addRef();
    *obj = self;
    return true;
  }

 private:
  std::atomic<uint32> refs_{1};
};

// ---- parameter model shared by both classes ----
double toPlain(ParamID id, double n) {
  n = std::clamp(n, 0.0, 1.0);
  switch (id) {
    case kGain: return -60.0 + 72.0 * n;
    case kBypass: return n >= 0.5 ? 1.0 : 0.0;
    case kLatencyMode: return n >= 0.5 ? 1.0 : 0.0;
    case kExtraOut: return n >= 0.5 ? 1.0 : 0.0;
    case kDrive: return 1.0 + 9.0 * n * n;
    case kHostRate: return n * 192000.0;
    default: return n;
  }
}
double toNorm(ParamID id, double p) {
  double n;
  switch (id) {
    case kGain: n = (p + 60.0) / 72.0; break;
    case kBypass:
    case kExtraOut:
    case kLatencyMode: n = p >= 0.5 ? 1.0 : 0.0; break;
    case kDrive: n = std::sqrt(std::max(0.0, (p - 1.0) / 9.0)); break;
    case kHostRate: n = p / 192000.0; break;
    default: n = p; break;
  }
  return std::clamp(n, 0.0, 1.0);
}

struct State {
  double gain = kGainDefaultNorm, bypass = 0, scMix = 0, latencyMode = 0, drive = 0;  // normalized
};

bool writeAll(IBStream* s, const void* p, int32 n) {
  int32 w = 0;
  return s && s->write(const_cast<void*>(p), n, &w) == kResultOk && w == n;
}
bool readAll(IBStream* s, void* p, int32 n) {
  int32 r = 0;
  return s && s->read(p, n, &r) == kResultOk && r == n;
}
bool saveModel(IBStream* s, const State& st) {
  uint32 hdr[2] = {kStateMagic, 1};
  double v[5] = {st.gain, st.bypass, st.scMix, st.latencyMode, st.drive};
  return writeAll(s, hdr, sizeof hdr) && writeAll(s, v, sizeof v);
}
bool loadModel(IBStream* s, State& st) {
  uint32 hdr[2] = {};
  double v[5] = {};
  if (!readAll(s, hdr, sizeof hdr) || hdr[0] != kStateMagic || hdr[1] != 1 || !readAll(s, v, sizeof v)) return false;
  st.gain = v[0];
  st.bypass = v[1];
  st.scMix = v[2];
  st.latencyMode = v[3];
  st.drive = v[4];
  return true;
}

// =================================================================================================
class Processor final : public Unknown<IComponent, IAudioProcessor, IConnectionPoint> {
 public:
  tresult PLUGIN_API queryInterface(const TUID riid, void** obj) override {
    if (!obj) return kInvalidArgument;
    if (answer<IComponent>(riid, obj, this) || answer<IAudioProcessor>(riid, obj, this) || answer<IConnectionPoint>(riid, obj, this))
      return kResultOk;
    if (iidIs(riid, IPluginBase::iid) || iidIs(riid, FUnknown::iid)) {
      addRef();
      *obj = static_cast<IComponent*>(this);
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }
  // Disambiguate the refcount methods of the three bases.
  uint32 PLUGIN_API addRef() override { return Unknown::addRef(); }
  uint32 PLUGIN_API release() override { return Unknown::release(); }

  // IPluginBase
  tresult PLUGIN_API initialize(FUnknown* context) override {
    if (host_) return kResultFalse;
    if (!context) return kInvalidArgument;
    IHostApplication* app = nullptr;
    if (context->queryInterface(IHostApplication::iid, reinterpret_cast<void**>(&app)) != kResultOk || !app) return kResultFalse;
    host_ = app;
    return kResultOk;
  }
  tresult PLUGIN_API terminate() override {
    if (peer_) peer_->release();
    peer_ = nullptr;
    if (host_) host_->release();
    host_ = nullptr;
    return kResultOk;
  }
  // IComponent
  tresult PLUGIN_API getControllerClassId(TUID classId) override {
    std::memcpy(classId, kControllerCid, sizeof(TUID));
    return kResultOk;
  }
  tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
  int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override {
    if (type == kAudio) return dir == kInput ? 2 : (extraOut_ ? 2 : 1);
    return 1;  // one event bus each way
  }
  tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
    if (index < 0 || index >= getBusCount(type, dir)) return kInvalidArgument;
    bus = BusInfo{};
    bus.mediaType = type;
    bus.direction = dir;
    if (type == kAudio) {
      bus.channelCount = 2;
      bus.busType = index == 0 ? kMain : kAux;
      bus.flags = index == 0 ? BusInfo::kDefaultActive : 0;
      wide(index == 0 ? (dir == kInput ? "Main In" : "Main Out") : (dir == kInput ? "Sidechain" : "Aux Out"), bus.name, 128);
    } else {
      bus.channelCount = 16;
      bus.busType = kMain;
      bus.flags = BusInfo::kDefaultActive;
      wide(dir == kInput ? "MIDI In" : "MIDI Out", bus.name, 128);
    }
    return kResultOk;
  }
  tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
  tresult PLUGIN_API activateBus(MediaType type, BusDirection dir, int32 index, TBool state) override {
    if (index < 0 || index >= getBusCount(type, dir)) return kInvalidArgument;
    if (type == kAudio && dir == kInput && index == 1) scActive_ = state != 0;
    return kResultOk;
  }
  tresult PLUGIN_API setActive(TBool state) override {
    if (state) {
      latency_ = st_.latencyMode >= 0.5 ? 128u : 64u;
      delay_.assign(2 * latency_, 0.0f);
      pos_ = 0;
      silentRun_ = 0;
      // Tell the controller (through the host's connection) that we run, and at what rate.
      if (peer_ && host_) {
        IMessage* msg = nullptr;
        TUID msgCid, msgIid;
        std::memcpy(msgCid, IMessage::iid.toTUID(), sizeof(TUID));
        std::memcpy(msgIid, IMessage::iid.toTUID(), sizeof(TUID));
        if (host_->createInstance(msgCid, msgIid, reinterpret_cast<void**>(&msg)) == kResultOk && msg) {
          msg->setMessageID(kMsgActivated);
          if (IAttributeList* a = msg->getAttributes()) a->setFloat("rate", sampleRate_);
          peer_->notify(msg);
          msg->release();
        }
      }
    }
    if (!state) extraOut_ = wantExtra_;  // IO changes only take effect while inactive
    active_ = state != 0;
    return kResultOk;
  }
  tresult PLUGIN_API setState(IBStream* state) override {
    State s;
    if (!loadModel(state, s)) return kResultFalse;
    st_ = s;
    return kResultOk;
  }
  tresult PLUGIN_API getState(IBStream* state) override { return saveModel(state, st_) ? kResultOk : kResultFalse; }

  // IAudioProcessor
  tresult PLUGIN_API setBusArrangements(SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs, int32 numOuts) override {
    if (numIns != 2 || numOuts != getBusCount(kAudio, kOutput)) return kResultFalse;
    for (int32 i = 0; i < numIns; ++i)
      if (inputs[i] != SpeakerArr::kStereo) return kResultFalse;
    for (int32 i = 0; i < numOuts; ++i)
      if (outputs[i] != SpeakerArr::kStereo) return kResultFalse;
    return kResultTrue;
  }
  tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) override {
    if (index < 0 || index >= getBusCount(kAudio, dir)) return kInvalidArgument;
    arr = SpeakerArr::kStereo;
    return kResultOk;
  }
  tresult PLUGIN_API canProcessSampleSize(int32 s) override { return s == kSample32 ? kResultTrue : kResultFalse; }
  uint32 PLUGIN_API getLatencySamples() override { return latency_; }
  tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
    if (active_) return kResultFalse;
    sampleRate_ = setup.sampleRate;
    return kResultOk;
  }
  tresult PLUGIN_API setProcessing(TBool state) override {
    if (state) {  // clears the delay line (the host's reset path)
      std::fill(delay_.begin(), delay_.end(), 0.0f);
      silentRun_ = 0;
    }
    return kResultOk;
  }
  uint32 PLUGIN_API getTailSamples() override { return 256; }

  tresult PLUGIN_API process(ProcessData& data) override {
    // Parameter changes: a cursor per queue, applied sample-accurately.
    struct Cursor {
      IParamValueQueue* q;
      ParamID id;
      int32 next, count;
    } cur[16];
    int32 nc = 0;
    if (IParameterChanges* pc = data.inputParameterChanges)
      for (int32 i = 0; i < pc->getParameterCount(); ++i) {
        IParamValueQueue* q = pc->getParameterData(i);
        if (!q) continue;
        const ParamID id = q->getParameterId();
        int32 off = 0;
        ParamValue v = 0;
        const int32 cnt = q->getPointCount();
        if (isBank(id) || id == kExtraOut) {  // block-rate: last value wins
          if (cnt > 0 && q->getPoint(cnt - 1, off, v) == kResultOk) {
            if (id == kExtraOut) wantExtra_ = v >= 0.5;
            else bank_[id - kBankFirst] = v;
          }
        } else if (nc < 16) {
          cur[nc++] = Cursor{q, id, 0, cnt};
        }
      }
    auto applyUpTo = [&](int32 frame) {
      for (int32 c = 0; c < nc; ++c) {
        Cursor& k = cur[c];
        int32 off = 0;
        ParamValue v = 0;
        while (k.next < k.count && k.q->getPoint(k.next, off, v) == kResultOk && off <= frame) {
          switch (k.id) {
            case kGain: st_.gain = v; break;
            case kBypass: st_.bypass = v; break;
            case kScMix: st_.scMix = v; break;
            case kLatencyMode: st_.latencyMode = v; break;
            case kDrive: st_.drive = v; break;
            case kEditProbe: probe_ = v; break;
            default: break;
          }
          ++k.next;
        }
      }
    };

    // Events: echo notes one octave up, sysex as is.
    if (data.inputEvents && data.outputEvents) {
      for (int32 i = 0; i < data.inputEvents->getEventCount(); ++i) {
        Event e{};
        if (data.inputEvents->getEvent(i, e) != kResultOk) continue;
        if (e.type == Event::kNoteOnEvent) e.noteOn.pitch = static_cast<int16>(std::min(127, e.noteOn.pitch + 12));
        else if (e.type == Event::kNoteOffEvent) e.noteOff.pitch = static_cast<int16>(std::min(127, e.noteOff.pitch + 12));
        else if (e.type != Event::kDataEvent) continue;
        data.outputEvents->addEvent(e);
      }
    }

    const int32 n = data.numSamples;
    if (n <= 0 || data.numOutputs < 1 || data.outputs[0].numChannels < 2 || delay_.empty()) {
      applyUpTo(INT32_MAX);
      return kResultOk;
    }
    float* const* in = data.numInputs > 0 && data.inputs[0].numChannels >= 2 ? data.inputs[0].channelBuffers32 : nullptr;
    float* const* sc = scActive_ && data.numInputs > 1 && data.inputs[1].numChannels >= 2 ? data.inputs[1].channelBuffers32 : nullptr;
    float* const* out = data.outputs[0].channelBuffers32;
    float* const* out2 = extraOut_ && data.numOutputs > 1 && data.outputs[1].numChannels >= 2 ? data.outputs[1].channelBuffers32 : nullptr;
    float peak = 0.0f;
    bool blockSilent = true;
    const uint32 len = static_cast<uint32>(delay_.size() / 2);
    for (int32 i = 0; i < n; ++i) {
      applyUpTo(i);
      double g = st_.bypass >= 0.5 ? 1.0 : std::pow(10.0, toPlain(kGain, st_.gain) / 20.0) * toPlain(kDrive, st_.drive);
      double mix = st_.bypass >= 0.5 ? 0.0 : st_.scMix;
      for (int ch = 0; ch < 2; ++ch) {
        float x = in ? in[ch][i] : 0.0f;
        float s = sc ? sc[ch][i] : 0.0f;
        peak = std::max(peak, std::fabs(x));
        if (x != 0.0f || (s != 0.0f && mix != 0.0)) blockSilent = false;
        float y = static_cast<float>(x * g + s * mix);
        float* line = delay_.data() + ch * len;
        float o = line[pos_];
        line[pos_] = y;
        out[ch][i] = o;
        if (out2) out2[ch][i] = o;
      }
      pos_ = (pos_ + 1) % len;
    }
    applyUpTo(INT32_MAX);
    silentRun_ = blockSilent ? silentRun_ + static_cast<uint32>(n) : 0;
    data.outputs[0].silenceFlags = silentRun_ >= len + static_cast<uint32>(n) ? 0x3 : 0;
    if (IParameterChanges* oc = data.outputParameterChanges) {
      int32 qi = 0, pi = 0;
      if (IParamValueQueue* q = oc->addParameterData(kPeak, qi)) q->addPoint(n - 1, std::min(1.0f, peak), pi);
      double sum = 0;
      for (double bv : bank_) sum += bv;
      if (IParamValueQueue* q = oc->addParameterData(kBankSum, qi)) q->addPoint(n - 1, sum / kBankCount, pi);
    }
    return kResultOk;
  }

  // IConnectionPoint
  tresult PLUGIN_API connect(IConnectionPoint* other) override {
    if (!other) return kInvalidArgument;
    if (peer_) return kResultFalse;
    peer_ = other;
    peer_->addRef();
    return kResultOk;
  }
  tresult PLUGIN_API disconnect(IConnectionPoint* other) override {
    if (!peer_ || other != peer_) return kResultFalse;
    peer_->release();
    peer_ = nullptr;
    return kResultOk;
  }
  tresult PLUGIN_API notify(IMessage*) override { return kResultOk; }

 private:
  ~Processor() override { terminate(); }
  IHostApplication* host_ = nullptr;
  IConnectionPoint* peer_ = nullptr;
  State st_;
  double probe_ = 0;
  double sampleRate_ = 44100.0;
  bool active_ = false, scActive_ = false, extraOut_ = false, wantExtra_ = false;
  double bank_[kBankCount] = {};
  uint32 latency_ = 64;
  std::vector<float> delay_;
  uint32 pos_ = 0, silentRun_ = 0;
};

// =================================================================================================
class Controller final : public Unknown<IEditController, IConnectionPoint, IMidiMapping> {
 public:
  tresult PLUGIN_API queryInterface(const TUID riid, void** obj) override {
    if (!obj) return kInvalidArgument;
    if (answer<IEditController>(riid, obj, this) || answer<IConnectionPoint>(riid, obj, this) || answer<IMidiMapping>(riid, obj, this))
      return kResultOk;
    if (iidIs(riid, IPluginBase::iid) || iidIs(riid, FUnknown::iid)) {
      addRef();
      *obj = static_cast<IEditController*>(this);
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }
  uint32 PLUGIN_API addRef() override { return Unknown::addRef(); }
  uint32 PLUGIN_API release() override { return Unknown::release(); }

  tresult PLUGIN_API initialize(FUnknown* context) override { return context ? kResultOk : kInvalidArgument; }
  tresult PLUGIN_API terminate() override {
    setComponentHandler(nullptr);
    if (peer_) peer_->release();
    peer_ = nullptr;
    return kResultOk;
  }

  tresult PLUGIN_API setComponentState(IBStream* state) override {
    State s;
    if (!loadModel(state, s)) return kResultFalse;
    values_[0] = s.gain;
    values_[1] = s.bypass;
    values_[2] = s.scMix;
    values_[3] = s.latencyMode;
    values_[5] = s.drive;
    return kResultOk;
  }
  tresult PLUGIN_API setState(IBStream* state) override {
    uint32 magic = 0;
    double rate = 0;
    if (!readAll(state, &magic, sizeof magic) || magic != kCtrlMagic || !readAll(state, &rate, sizeof rate)) return kResultFalse;
    hostRateNorm_ = rate;
    return kResultOk;
  }
  tresult PLUGIN_API getState(IBStream* state) override {
    uint32 magic = kCtrlMagic;
    return writeAll(state, &magic, sizeof magic) && writeAll(state, &hostRateNorm_, sizeof hostRateNorm_) ? kResultOk : kResultFalse;
  }

  int32 PLUGIN_API getParameterCount() override { return kParamCount; }
  tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override {
    struct Def {
      ParamID id;
      const char* title;
      const char* units;
      int32 steps;
      double def;
      int32 flags;
    };
    static const Def kDefs[] = {
        {kGain, "Gain", "dB", 0, kGainDefaultNorm, ParameterInfo::kCanAutomate},
        {kBypass, "Bypass", "", 1, 0.0, ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass},
        {kScMix, "Sidechain Mix", "", 0, 0.0, ParameterInfo::kCanAutomate},
        {kLatencyMode, "Latency Mode", "", 1, 0.0, 0},
        {kPeak, "Peak", "", 0, 0.0, ParameterInfo::kIsReadOnly},
        {kDrive, "Drive", "x", 0, 0.0, ParameterInfo::kCanAutomate},
        {kHostRate, "Host Rate", "Hz", 0, 0.0, ParameterInfo::kIsReadOnly | ParameterInfo::kIsHidden},
        {kEditProbe, "Edit Probe", "", 0, 0.0, ParameterInfo::kIsHidden},
        {kBankSum, "Bank Sum", "", 0, 0.0, ParameterInfo::kIsReadOnly | ParameterInfo::kIsHidden},
        {kExtraOut, "Extra Out", "", 1, 0.0, 0},
        {kDirtyProbe, "Dirty Probe", "", 0, 0.0, ParameterInfo::kIsHidden},
    };
    if (index < 0 || index >= kParamCount) return kInvalidArgument;
    if (index >= kFixedParams) {
      info = ParameterInfo{};
      info.id = kBankFirst + static_cast<ParamID>(index - kFixedParams);
      char t[32];
      std::snprintf(t, sizeof t, "Bank %d", static_cast<int>(index - kFixedParams));
      wide(t, info.title, 128);
      wide(t, info.shortTitle, 128);
      info.flags = ParameterInfo::kCanAutomate | ParameterInfo::kIsHidden;
      return kResultOk;
    }
    const Def& d = kDefs[index];
    info = ParameterInfo{};
    info.id = d.id;
    wide(d.title, info.title, 128);
    wide(d.title, info.shortTitle, 128);
    wide(d.units, info.units, 128);
    info.stepCount = d.steps;
    info.defaultNormalizedValue = d.def;
    info.unitId = 0;
    info.flags = d.flags;
    return kResultOk;
  }
  tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue valueNormalized, String128 string) override {
    char buf[64];
    double p = toPlain(id, valueNormalized);
    if (id == kGain) std::snprintf(buf, sizeof buf, "%.2f dB", p);
    else if (id == kBypass) std::snprintf(buf, sizeof buf, "%s", p >= 0.5 ? "On" : "Off");
    else std::snprintf(buf, sizeof buf, "%.3f", p);
    wide(buf, string, 128);
    return kResultOk;
  }
  tresult PLUGIN_API getParamValueByString(ParamID id, TChar* string, ParamValue& valueNormalized) override {
    char buf[64] = {};
    for (int i = 0; i < 63 && string && string[i]; ++i) buf[i] = static_cast<char>(string[i]);
    valueNormalized = toNorm(id, std::atof(buf));
    return kResultOk;
  }
  ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue n) override { return toPlain(id, n); }
  ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue p) override { return toNorm(id, p); }
  ParamValue PLUGIN_API getParamNormalized(ParamID id) override {
    int s = slot(id);
    if (id == kDirtyProbe && handler_) {
      IComponentHandler2* h2 = nullptr;
      if (handler_->queryInterface(IComponentHandler2::iid, reinterpret_cast<void**>(&h2)) == kResultOk && h2) {
        h2->setDirty(true);
        h2->release();
      }
    }
    return s < 0 ? 0.0 : (id == kHostRate ? hostRateNorm_ : values_[s]);
  }
  tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override {
    int s = slot(id);
    if (s < 0) return kInvalidArgument;
    double v = std::clamp(value, 0.0, 1.0);
    if (id == kHostRate) hostRateNorm_ = v;
    bool latencyChange = id == kLatencyMode && (values_[s] >= 0.5) != (v >= 0.5);
    bool ioChange = id == kExtraOut && (values_[s] >= 0.5) != (v >= 0.5);
    values_[s] = v;
    if (latencyChange && handler_) handler_->restartComponent(kLatencyChanged);
    if (ioChange && handler_) handler_->restartComponent(kIoChanged);
    return kResultOk;
  }
  tresult PLUGIN_API setComponentHandler(IComponentHandler* handler) override {
    if (handler) handler->addRef();
    if (handler_) handler_->release();
    handler_ = handler;
    return kResultOk;
  }
  IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }

  // IMidiMapping: CC 7 on any channel -> Gain.
  tresult PLUGIN_API getMidiControllerAssignment(int32 busIndex, int16, CtrlNumber cc, ParamID& id) override {
    if (busIndex != 0 || cc != 7) return kResultFalse;
    id = kGain;
    return kResultTrue;
  }

  // IConnectionPoint
  tresult PLUGIN_API connect(IConnectionPoint* other) override {
    if (!other) return kInvalidArgument;
    if (peer_) return kResultFalse;
    peer_ = other;
    peer_->addRef();
    return kResultOk;
  }
  tresult PLUGIN_API disconnect(IConnectionPoint* other) override {
    if (!peer_ || other != peer_) return kResultFalse;
    peer_->release();
    peer_ = nullptr;
    return kResultOk;
  }
  tresult PLUGIN_API notify(IMessage* message) override {
    if (!message || !message->getMessageID() || std::strcmp(message->getMessageID(), kMsgActivated) != 0) return kResultFalse;
    double rate = 0;
    if (!message->getAttributes() || message->getAttributes()->getFloat("rate", rate) != kResultOk) return kResultFalse;
    hostRateNorm_ = toNorm(kHostRate, rate);
    if (handler_) {
      handler_->restartComponent(kParamValuesChanged);
      handler_->beginEdit(kEditProbe);
      values_[slot(kEditProbe)] = 0.75;
      handler_->performEdit(kEditProbe, 0.75);
      handler_->endEdit(kEditProbe);
    }
    return kResultOk;
  }

 private:
  ~Controller() override { terminate(); }
  static int slot(ParamID id) {
    switch (id) {
      case kGain: return 0;
      case kBypass: return 1;
      case kScMix: return 2;
      case kLatencyMode: return 3;
      case kPeak: return 4;
      case kDrive: return 5;
      case kHostRate: return 6;
      case kEditProbe: return 7;
      case kBankSum: return 8;
      case kExtraOut: return 9;
      case kDirtyProbe: return 10;
      default: return isBank(id) ? kFixedParams + static_cast<int>(id - kBankFirst) : -1;
    }
  }
  double values_[kParamCount] = {kGainDefaultNorm};
  double hostRateNorm_ = 0;
  IComponentHandler* handler_ = nullptr;
  IConnectionPoint* peer_ = nullptr;
};

// =================================================================================================
class Factory;
Factory* gFactory = nullptr;

class Factory final : public Unknown<IPluginFactory3> {
 public:
  ~Factory() override { gFactory = nullptr; }
  tresult PLUGIN_API queryInterface(const TUID riid, void** obj) override {
    if (!obj) return kInvalidArgument;
    if (iidIs(riid, IPluginFactory3::iid) || iidIs(riid, IPluginFactory2::iid) || iidIs(riid, IPluginFactory::iid) || iidIs(riid, FUnknown::iid)) {
      addRef();
      *obj = static_cast<IPluginFactory3*>(this);
      return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
  }
  tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
    if (!info) return kInvalidArgument;
    *info = PFactoryInfo("Squizzy Studio Tests", "", "", PFactoryInfo::kUnicode);
    return kResultOk;
  }
  int32 PLUGIN_API countClasses() override { return 2; }
  tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
    if (!info || index < 0 || index > 1) return kInvalidArgument;
    *info = index == 0 ? PClassInfo(kProcessorCid, PClassInfo::kManyInstances, kVstAudioEffectClass, "Squizzy Test Gain")
                       : PClassInfo(kControllerCid, PClassInfo::kManyInstances, kVstComponentControllerClass, "Squizzy Test Gain Controller");
    return kResultOk;
  }
  tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2* info) override {
    if (!info || index < 0 || index > 1) return kInvalidArgument;
    *info = index == 0 ? PClassInfo2(kProcessorCid, PClassInfo::kManyInstances, kVstAudioEffectClass, "Squizzy Test Gain", 0, "Fx|Tools",
                                     "Squizzy Studio Tests", "1.0.0", "VST 3.8.1")
                       : PClassInfo2(kControllerCid, PClassInfo::kManyInstances, kVstComponentControllerClass, "Squizzy Test Gain Controller",
                                     0, "", "Squizzy Studio Tests", "1.0.0", "VST 3.8.1");
    return kResultOk;
  }
  tresult PLUGIN_API getClassInfoUnicode(int32 index, PClassInfoW* info) override {
    PClassInfo2 c2;
    if (!info || getClassInfo2(index, &c2) != kResultOk) return kInvalidArgument;
    *info = PClassInfoW{};
    std::memcpy(info->cid, c2.cid, sizeof(TUID));
    info->cardinality = c2.cardinality;
    std::memcpy(info->category, c2.category, sizeof info->category);
    wide(c2.name, info->name, PClassInfo::kNameSize);
    info->classFlags = c2.classFlags;
    std::memcpy(info->subCategories, c2.subCategories, sizeof info->subCategories);
    wide(c2.vendor, info->vendor, PClassInfoW::kVendorSize);
    wide(c2.version, info->version, PClassInfoW::kVersionSize);
    wide(c2.sdkVersion, info->sdkVersion, PClassInfoW::kVersionSize);
    return kResultOk;
  }
  tresult PLUGIN_API createInstance(FIDString cid, FIDString riid, void** obj) override {
    if (!cid || !riid || !obj) return kInvalidArgument;
    *obj = nullptr;
    FUnknown* o = nullptr;
    if (sameId(cid, kProcessorCid)) o = static_cast<IComponent*>(new Processor());
    else if (sameId(cid, kControllerCid)) o = static_cast<IEditController*>(new Controller());
    else return kNoInterface;
    tresult r = o->queryInterface(riid, obj);
    o->release();
    return r;
  }
  tresult PLUGIN_API setHostContext(FUnknown*) override { return kResultOk; }
};

}  // namespace

extern "C" {

SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory() {
  if (gEntered.load() <= 0) return nullptr;  // the host must call the module entry first
  if (!gFactory) {
    gFactory = new Factory();  // refcount 1: owned by the caller
    return gFactory;
  }
  gFactory->addRef();
  return gFactory;
}

#if defined(_WIN32)
SMTG_EXPORT_SYMBOL bool InitDll() {
  ++gEntered;
  return true;
}
SMTG_EXPORT_SYMBOL bool ExitDll() {
  --gEntered;
  return true;
}
#elif defined(__APPLE__)
SMTG_EXPORT_SYMBOL bool bundleEntry(void*) {
  ++gEntered;
  return true;
}
SMTG_EXPORT_SYMBOL bool bundleExit() {
  --gEntered;
  return true;
}
#else
SMTG_EXPORT_SYMBOL bool ModuleEntry(void*) {
  ++gEntered;
  return true;
}
SMTG_EXPORT_SYMBOL bool ModuleExit() {
  --gEntered;
  return true;
}
#endif

}  // extern "C"
