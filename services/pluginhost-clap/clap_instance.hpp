// pluginhost-clap: IPluginInstance over a CLAP plugin.
#pragma once

#include <clap/clap.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "clap_library.hpp"
#include "rt_util.hpp"
#include "sq/interfaces/pluginhost.h"
#include "sq/sdk.hpp"

namespace sqclap {

// Fills the static, plugin-independent part of the host struct (used by the scanner too).
void fillHostInfo(clap_host_t& h);
// Fills a sq_plugin_desc from a descriptor + an initialised plugin (ports, gui).
void describePlugin(const clap_plugin_descriptor_t* d, const clap_plugin_t* plugin, sq_plugin_desc* out);
// Platform window API name for the current OS.
const char* nativeWindowApi();

class ClapInstance final : public sq::Object<ClapInstance, sq::IPluginInstance> {
 public:
  // Creates + inits the plugin. Main thread.
  static sq_result create(std::shared_ptr<ClapLibrary> lib, const sq_plugin_desc* desc, sq::IPluginHostCallback* cb,
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

  ClapInstance();
  ~ClapInstance() override;

 private:
  friend struct HostThunks;
  struct ParamSet {
    uint32_t id;
    double value;
  };
  union EventSlot {
    clap_event_header_t header;
    clap_event_note_t note;
    clap_event_midi_t midi;
    clap_event_midi_sysex_t sysex;
    clap_event_param_value_t param;
  };
  enum Pending : uint32_t {
    kPendRestart = 1u << 0,
    kPendCallback = 1u << 1,
    kPendLatency = 1u << 2,
    kPendParamsRescan = 1u << 3,
    kPendStateDirty = 1u << 4,
    kPendFlush = 1u << 5,
    kPendTail = 1u << 6,
  };

  void pump();  // main thread: deliver deferred plugin requests to the host callback
  void queryExtensions();
  bool isMainThread() const;
  void flushParamsMainThread(const ParamSet* sets, uint32_t count, bool reportEdits);
  uint32_t paramSlot(uint32_t id) const;  // index into paramIds_, or UINT32_MAX (RT-safe)
  bool pushOutputEvent(const clap_event_header_t* h);  // audio thread (or flush on main)

  // CLAP callbacks (static thunks in the .cpp forward here).
  clap_host_t host_{};
  std::shared_ptr<ClapLibrary> lib_;
  const clap_plugin_t* plugin_ = nullptr;
  sq::Ptr<sq::IPluginHostCallback> cb_;
  sq_plugin_desc desc_{};
  uint64_t mainThread_ = 0;

  const clap_plugin_audio_ports_t* audioPorts_ = nullptr;
  const clap_plugin_audio_ports_activation_t* portsActivation_ = nullptr;
  const clap_plugin_note_ports_t* notePorts_ = nullptr;
  const clap_plugin_params_t* params_ = nullptr;
  const clap_plugin_latency_t* latency_ = nullptr;
  const clap_plugin_tail_t* tail_ = nullptr;
  const clap_plugin_state_t* state_ = nullptr;
  const clap_plugin_gui_t* gui_ = nullptr;

  std::atomic<uint32_t> pending_{0};
  std::atomic<uint32_t> latencyFrames_{0};
  bool active_ = false;
  bool processing_ = false;       // start_processing called (audio thread side)
  bool editorOpen_ = false;
  bool noteDialectClap_ = true;
  bool hasNoteInput_ = false;
  std::vector<uint8_t> inActive_, outActive_;  // requested bus activation (main + aux)

  // ---- buffers prepared in activate() (audio thread uses them without allocating) ----
  uint32_t maxFrames_ = 0;
  std::vector<clap_audio_buffer_t> inBufs_, outBufs_;
  std::vector<uint32_t> inChannels_, outChannels_;
  std::vector<std::vector<float*>> inPtrs_, outPtrs_;
  std::vector<float> zeroIn_, trashOut_;
  std::vector<EventSlot> evStore_;
  std::vector<const clap_event_header_t*> evList_;
  uint32_t evCount_ = 0;
  std::vector<uint8_t> sysexArena_;
  uint32_t sysexUsed_ = 0;
  std::vector<uint32_t> paramIds_;                      // sorted
  std::unique_ptr<std::atomic<double>[]> paramValues_;  // parallel to paramIds_
  SpscRing<ParamSet> toAudio_{1024};
  int64_t steadyTime_ = 0;

  // Output routing for the call in progress.
  sq_plugin_process* curProcess_ = nullptr;
  std::vector<sq_param_change>* flushEdits_ = nullptr;

  clap_input_events_t inEvents_{};
  clap_output_events_t outEvents_{};
};

}  // namespace sqclap
