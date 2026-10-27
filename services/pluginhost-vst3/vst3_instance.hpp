// pluginhost-vst3: IPluginInstance over a VST3 component (+ edit controller).
#pragma once

#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "sq/interfaces/pluginhost.h"
#include "vst3_hostobjects.hpp"
#include "vst3_module.hpp"

namespace sqvst3 {

// Scanner side: fills `out` for factory class `index` (audio module classes only). The component
// (and its controller, for the editor flag) is created, inspected and destroyed again.
// Returns SQ_S_FALSE for classes that are not audio modules.
sq_result describeClass(Module& mod, int32_t index, sq_plugin_desc* out);

const char* nativeViewType();

class Vst3Instance;

// IComponentHandler(2) given to the edit controller; forwards to the owning instance.
class ComponentHandler final : public FObject<ComponentHandler, V::IComponentHandler, V::IComponentHandler2> {
 public:
  explicit ComponentHandler(Vst3Instance* owner) : owner_(owner) {}
  void detach() { owner_.store(nullptr, std::memory_order_release); }
  tresult PLUGIN_API beginEdit(V::ParamID id) override;
  tresult PLUGIN_API performEdit(V::ParamID id, V::ParamValue valueNormalized) override;
  tresult PLUGIN_API endEdit(V::ParamID id) override;
  tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override;
  tresult PLUGIN_API setDirty(Steinberg::TBool state) override;
  tresult PLUGIN_API requestOpenEditor(Steinberg::FIDString) override { return Steinberg::kResultFalse; }
  tresult PLUGIN_API startGroupEdit() override { return Steinberg::kResultOk; }
  tresult PLUGIN_API finishGroupEdit() override { return Steinberg::kResultOk; }

 private:
  std::atomic<Vst3Instance*> owner_;
};

// IPlugFrame given to an open editor view.
class PlugFrame final : public FObject<PlugFrame, Steinberg::IPlugFrame> {
 public:
  explicit PlugFrame(Vst3Instance* owner) : owner_(owner) {}
  void detach() { owner_ = nullptr; }
  tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override;

 private:
  Vst3Instance* owner_;
};

class Vst3Instance final : public sq::Object<Vst3Instance, sq::IPluginInstance> {
 public:
  // Creates, initialises and connects component + controller. Main thread.
  static sq_result create(std::shared_ptr<Module> mod, const sq_plugin_desc* desc, sq::IPluginHostCallback* cb,
                          sq::IPluginInstance** out);

  // ---- IPluginInstance ----
  sq_result SQ_CALL getDesc(sq_plugin_desc* out) override;
  uint32_t SQ_CALL getBusCount(sq_bool input) override;
  sq_result SQ_CALL getBusInfo(sq_bool input, uint32_t index, sq_plugin_bus_info* out) override;
  sq_result SQ_CALL setBusActive(sq_bool input, uint32_t index, sq_bool active) override;
  sq_result SQ_CALL activate(const sq_plugin_setup* setup) override;
  sq_result SQ_CALL deactivate() override;
  sq_result SQ_CALL process(sq_plugin_process* data) override;
  void SQ_CALL reset() override;
  uint32_t SQ_CALL getLatencyFrames() override;
  uint32_t SQ_CALL getTailFrames() override;
  uint32_t SQ_CALL getParamCount() override;
  sq_result SQ_CALL getParamInfo(uint32_t index, sq_param_info* out) override;
  sq_result SQ_CALL getParamValue(uint32_t paramId, double* value) override;
  sq_result SQ_CALL setParamValue(uint32_t paramId, double value) override;
  sq_result SQ_CALL paramValueToText(uint32_t paramId, double value, char* buf, uint32_t bufSize, uint32_t* needed) override;
  sq_result SQ_CALL saveState(sq::IByteStream* out) override;
  sq_result SQ_CALL loadState(sq::IByteStream* in) override;
  sq_bool SQ_CALL hasEditor() override;
  sq_result SQ_CALL openEditor(void* parent, const char* platform, uint32_t* width, uint32_t* height) override;
  sq_result SQ_CALL closeEditor() override;
  sq_result SQ_CALL setEditorSize(uint32_t width, uint32_t height) override;

  Vst3Instance();
  ~Vst3Instance() override;

  // ---- plugin -> host (from ComponentHandler / PlugFrame; any thread, delivered by pump()) ----
  void handlerEdit(V::ParamID id, double norm, uint32_t gestureFlags, bool perform);
  void handlerRestart(int32_t flags);
  void handlerDirty();
  bool frameResize(Steinberg::IPlugView* view, Steinberg::ViewRect* r);

 private:
  struct Bus {
    std::string name;
    uint32_t channels = 0;
    V::SpeakerArrangement arr = 0;
    bool main = true;
    bool active = true;
  };
  struct ParamSet {
    V::ParamID id;
    double norm;
  };
  struct ParamMap {  // RT conversion plain <-> normalized, built in activate()
    V::ParamID id;
    int32_t steps;
    bool linear;
    double p0, p1;
    uint32_t lut;  // offset into lut_ (non-linear params)
  };
  struct Edit {
    V::ParamID id;
    double norm;
    uint32_t flags;
  };
  enum Pending : uint32_t {
    kPendLatency = 1u << 0,
    kPendParams = 1u << 1,
    kPendRestart = 1u << 2,
    kPendDirty = 1u << 3,
  };
  static constexpr uint32_t kLutPoints = 65;

  bool isMainThread() const;
  void pump();  // main thread: deliver queued plugin requests to the host callback
  void scanBuses();
  void refreshBuses();      // main thread, inactive: re-read buses after an IO change (keeps active flags)
  void refreshParamIds();   // main thread: sorted ids of the controller's parameters
  bool hasParam(V::ParamID id) const;
  void buildParamMaps();
  void buildMidiMap();
  bool pushToAudio(V::ParamID id, double norm);
  const ParamMap* findMap(V::ParamID id) const noexcept;
  double plainToNormRT(V::ParamID id, double plain) const noexcept;
  double normToPlainRT(V::ParamID id, double norm) const noexcept;
  double plainToNormMain(V::ParamID id, double plain);
  double normToPlainMain(V::ParamID id, double norm);
  void addMidiInput(const sq_midi_event& m) noexcept;
  void collectOutputs(sq_plugin_process* d) noexcept;
  void shutdownPlugin();

  std::shared_ptr<Module> mod_;
  sq::Ptr<sq::IPluginHostCallback> cb_;
  sq_plugin_desc desc_{};
  uint64_t mainThread_ = 0;

  IPtr<V::IComponent> component_;
  IPtr<V::IAudioProcessor> processor_;
  IPtr<V::IEditController> controller_;
  bool controllerSeparate_ = false;
  IPtr<V::IConnectionPoint> compCP_, ctrlCP_;
  IPtr<ConnectionProxy> toCtrl_, toComp_;  // compCP_ talks to toCtrl_, ctrlCP_ to toComp_
  IPtr<ComponentHandler> handler_;
  IPtr<V::IMidiMapping> midiMapping_;

  std::vector<Bus> inBuses_, outBuses_;
  int32_t eventIns_ = 0, eventOuts_ = 0;
  bool busesStale_ = false;            // restart requested: re-read buses once inactive
  std::vector<V::ParamID> paramIds_;   // sorted (main thread)

  bool active_ = false;
  std::atomic<bool> processing_{false};
  double sampleRate_ = 48000.0;
  uint32_t maxFrames_ = 0;
  std::atomic<uint32_t> latency_{0};
  std::atomic<uint32_t> pending_{0};
  bool pumping_ = false;
  std::mutex editsMutex_;
  std::vector<Edit> edits_;

  // ---- editor ----
  IPtr<Steinberg::IPlugView> view_;
  IPtr<PlugFrame> frame_;
  int hasEditor_ = -1;  // unknown / no / yes

  // ---- audio-thread structures (sized in activate) ----
  std::vector<V::AudioBusBuffers> inBufs_, outBufs_;
  std::vector<std::vector<float*>> inPtrs_, outPtrs_;
  std::vector<float> zeroIn_, trashOut_;
  ParameterChanges inChanges_, outChanges_;
  EventList inEvents_, outEvents_;
  V::ProcessContext ctx_{};
  V::ProcessData pd_{};
  std::vector<uint8_t> sysexArena_;
  uint32_t sysexUsed_ = 0;
  std::vector<ParamMap> maps_;  // sorted by id
  std::vector<double> lut_;
  std::vector<V::ParamID> midiMap_;  // [channel * 130 + controller] or empty
  int64_t steadyTime_ = 0;
  uint32_t curFrames_ = 0;
  SpscRing<ParamSet> toAudio_{2048};    // main (any producer, spinlocked) -> audio
  SpscRing<ParamSet> fromAudio_{2048};  // audio -> main (controller sync)
  sq_spinlock toAudioLock_{};
  // Sets made while inactive, coalesced per id (main thread only while inactive; read by
  // process() from `pendingCursor_` on, never resized while active).
  std::vector<ParamSet> pendingSets_;
  size_t pendingCursor_ = 0;
  bool queueSet(V::ParamID id, double norm);  // main thread: pending (inactive) or ring (active)
};

}  // namespace sqvst3
