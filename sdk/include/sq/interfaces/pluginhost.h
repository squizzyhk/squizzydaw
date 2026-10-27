/* Squizzy Studio - plugin format adapters + scanner manifest (plan 2.2, M4).
   OWNER: orchestrator (frozen).
   IMPLEMENTS: services/pluginhost-clap, services/pluginhost-vst3 (IPluginFormat, category
               "PluginFormat"); services/pluginscanner (IPluginRegistry service + the out-of-process
               scanner exe that calls IPluginFormat::scanFile and writes manifest records).
   CONSUMES: engine (graph nodes, PDC, sidechain), ui (browser, editor windows), sqlite-index.
   THREADING:
   - IPluginFormat, IPluginRegistry, and IPluginInstance methods: main thread, EXCEPT
     IPluginInstance::process (audio thread, RT rules) and reset (audio thread while processing).
   - activate/deactivate: main thread, never concurrently with process.
   - IPluginHostCallback: always called on the main thread (adapters marshal plugin requests
     coming from other threads).
   - IPluginScanSink: scanner thread.
   Parameter values crossing this interface are PLAIN values in [min_value, max_value]. */
#ifndef SQ_INTERFACES_PLUGINHOST_H
#define SQ_INTERFACES_PLUGINHOST_H

#include "sq/media.h"
#include "sq/module.h"
#include "sq/interfaces/common.h"

/* sq_plugin_desc.flags */
#define SQ_PLUGIN_INSTRUMENT  0x01u
#define SQ_PLUGIN_EFFECT      0x02u
#define SQ_PLUGIN_MIDI_EFFECT 0x04u
#define SQ_PLUGIN_ANALYZER    0x08u
#define SQ_PLUGIN_HAS_EDITOR  0x10u
#define SQ_PLUGIN_SIDECHAIN   0x20u

/* sq_plugin_desc.status */
#define SQ_PLUGIN_STATUS_OK       0
#define SQ_PLUGIN_STATUS_CRASHED  1 /* scanner crashed loading it: quarantined */
#define SQ_PLUGIN_STATUS_TIMEOUT  2
#define SQ_PLUGIN_STATUS_BLOCKED  3 /* user/blocklist */
#define SQ_PLUGIN_STATUS_INVALID  4

/* sq_plugin_bus_info.flags */
#define SQ_BUS_MAIN     0x01u
#define SQ_BUS_AUX      0x02u /* sidechain / auxiliary */
#define SQ_BUS_ACTIVE   0x04u

/* sq_param_info.flags */
#define SQ_PARAM_AUTOMATABLE 0x01u
#define SQ_PARAM_STEPPED     0x02u
#define SQ_PARAM_BYPASS      0x04u
#define SQ_PARAM_READONLY    0x08u
#define SQ_PARAM_HIDDEN      0x10u

/* sq_plugin_setup.flags */
#define SQ_SETUP_OFFLINE 0x01u

/* sq_param_change.flags (gesture from the plugin's editor) */
#define SQ_PARAM_GESTURE_BEGIN 0x01u
#define SQ_PARAM_GESTURE_END   0x02u

/* Scanner manifest record: one per plugin class found in a file. The scanner persists these
   (JSON with the same field names) and IPluginRegistry serves them. */
typedef struct sq_plugin_desc {
  uint32_t struct_size;
  char format[16];        /* "clap", "vst3" */
  char id[128];           /* format-unique class id (string form) */
  char name[128];
  char vendor[128];
  char version[32];
  char category[64];      /* free-form from the plugin: "Instrument|Synth", "Fx|Reverb" */
  char path[1024];        /* file or bundle path */
  uint32_t sub_index;     /* index of the class inside the file */
  uint32_t flags;         /* SQ_PLUGIN_* */
  uint32_t audio_inputs;  /* main input channels */
  uint32_t audio_outputs;
  uint32_t aux_inputs;    /* number of aux (sidechain) input buses */
  uint32_t midi_inputs;   /* event input ports */
  uint32_t midi_outputs;
  int32_t status;         /* SQ_PLUGIN_STATUS_* */
  int64_t file_mtime;     /* unix seconds, rescan detection */
  int64_t file_size;
} sq_plugin_desc;

typedef struct sq_plugin_format_info {
  uint32_t struct_size;
  char format[16];        /* matches sq_plugin_desc.format */
  char display_name[64];
  char file_extension[16]; /* "clap", "vst3" */
  uint32_t flags;          /* reserved */
} sq_plugin_format_info;

typedef struct sq_plugin_bus_info {
  uint32_t struct_size;
  char name[64];
  uint32_t channels;
  uint32_t flags;          /* SQ_BUS_* */
} sq_plugin_bus_info;

typedef struct sq_param_info {
  uint32_t struct_size;
  uint32_t id;             /* stable across sessions */
  char name[128];
  char unit[32];
  char group[128];         /* "Filter/Env" style path, may be "" */
  double min_value;
  double max_value;
  double default_value;
  uint32_t flags;          /* SQ_PARAM_* */
  uint32_t step_count;     /* stepped params: number of steps, else 0 */
} sq_param_info;

typedef struct sq_param_change {
  uint32_t param_id;
  uint32_t frame_offset;   /* within the block */
  double value;            /* plain */
  uint32_t flags;          /* SQ_PARAM_GESTURE_* (output only) */
} sq_param_change;

typedef struct sq_plugin_setup {
  uint32_t struct_size;
  double sample_rate;
  uint32_t min_block_frames;
  uint32_t max_block_frames;
  uint32_t flags;          /* SQ_SETUP_* */
} sq_plugin_setup;

typedef struct sq_plugin_transport {
  uint32_t struct_size;
  uint32_t flags;          /* SQ_TRANSPORT_* values from audioengine.h (playing/recording/looping) */
  double bpm;
  sq_ppq_time position;
  sq_ppq_time bar_start;
  sq_sample_time frames;
  uint32_t numerator;
  uint32_t denominator;
  sq_ppq_time loop_start;
  sq_ppq_time loop_end;
} sq_plugin_transport;

/* One audio-thread process call. inputs[i]/outputs[i] = bus i (main first, then aux). Inactive
   buses still have a block with channel pointers (silence). */
typedef struct sq_plugin_process {
  uint32_t struct_size;
  uint32_t frames;
  const sq_audio_block* inputs;
  uint32_t input_bus_count;
  sq_audio_block* outputs;
  uint32_t output_bus_count;
  const sq_midi_event* midi_in;   /* sorted by frame_offset */
  uint32_t midi_in_count;
  sq_midi_event* midi_out;        /* caller-owned capacity; sysex data pointers valid until next process */
  uint32_t midi_out_capacity;
  uint32_t midi_out_count;        /* out */
  const sq_param_change* params_in;  /* automation, sorted by frame_offset */
  uint32_t params_in_count;
  sq_param_change* params_out;    /* edits made by the plugin (editor/gesture) */
  uint32_t params_out_capacity;
  uint32_t params_out_count;      /* out */
  const sq_plugin_transport* transport;
} sq_plugin_process;

#ifdef __cplusplus
namespace sq {

struct IPluginInstance;

/* Implemented by the engine, one per instance. Main thread. */
struct IPluginHostCallback : IUnknown {
  SQ_INTERFACE(IPluginHostCallback, IUnknown, "{7FB16E49-73AE-4947-9D58-FB6312ADCE99}")
  virtual void SQ_CALL onLatencyChanged(IPluginInstance* plugin) = 0;
  virtual void SQ_CALL onParamsChanged(IPluginInstance* plugin) = 0;   /* param list/info changed */
  virtual void SQ_CALL onParamEdited(IPluginInstance* plugin, const sq_param_change* change) = 0; /* editor edit outside process */
  virtual void SQ_CALL onRestartRequested(IPluginInstance* plugin) = 0; /* deactivate + activate needed */
  virtual void SQ_CALL onStateDirty(IPluginInstance* plugin) = 0;
  /* Editor wants a new size; return SQ_TRUE if the host resized the parent window. */
  virtual sq_bool SQ_CALL onEditorResize(IPluginInstance* plugin, uint32_t width, uint32_t height) = 0;
};

struct IPluginScanSink : IUnknown {
  SQ_INTERFACE(IPluginScanSink, IUnknown, "{4255AE48-84D2-4B14-92CE-6B128E6DDC17}")
  virtual void SQ_CALL onPlugin(const sq_plugin_desc* desc) = 0;
};

struct IPluginInstance : IUnknown {
  SQ_INTERFACE(IPluginInstance, IUnknown, "{57D7FF3B-24B5-4BDF-864C-0A459F40D026}")
  virtual sq_result SQ_CALL getDesc(sq_plugin_desc* out) = 0;
  virtual uint32_t SQ_CALL getBusCount(sq_bool input) = 0;
  virtual sq_result SQ_CALL getBusInfo(sq_bool input, uint32_t index, sq_plugin_bus_info* out) = 0;
  /* Only while deactivated. Aux (sidechain) buses default to inactive. */
  virtual sq_result SQ_CALL setBusActive(sq_bool input, uint32_t index, sq_bool active) = 0;
  virtual sq_result SQ_CALL activate(const sq_plugin_setup* setup) = 0;
  virtual sq_result SQ_CALL deactivate() = 0;
  /* AUDIO THREAD. Returns SQ_S_FALSE when the output is silent (tail finished). */
  virtual sq_result SQ_CALL process(sq_plugin_process* data) = 0;
  /* AUDIO THREAD: clear internal buffers (transport jump). */
  virtual void SQ_CALL reset() = 0;
  virtual uint32_t SQ_CALL getLatencyFrames() = 0;  /* valid after activate */
  virtual uint32_t SQ_CALL getTailFrames() = 0;     /* UINT32_MAX = infinite */
  virtual uint32_t SQ_CALL getParamCount() = 0;
  virtual sq_result SQ_CALL getParamInfo(uint32_t index, sq_param_info* out) = 0;
  virtual sq_result SQ_CALL getParamValue(uint32_t paramId, double* value) = 0;
  /* Main-thread set (applied before the next process). */
  virtual sq_result SQ_CALL setParamValue(uint32_t paramId, double value) = 0;
  virtual sq_result SQ_CALL paramValueToText(uint32_t paramId, double value, char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  /* Opaque state (preset/session). */
  virtual sq_result SQ_CALL saveState(IByteStream* out) = 0;
  virtual sq_result SQ_CALL loadState(IByteStream* in) = 0;
  virtual sq_bool SQ_CALL hasEditor() = 0;
  /* parent = native window (HWND / NSView* / X11 Window); platform = "win32" | "cocoa" | "x11".
     *width/*height receive the initial size. */
  virtual sq_result SQ_CALL openEditor(void* parent, const char* platform, uint32_t* width, uint32_t* height) = 0;
  virtual sq_result SQ_CALL closeEditor() = 0;
  virtual sq_result SQ_CALL setEditorSize(uint32_t width, uint32_t height) = 0; /* SQ_E_UNSUPPORTED if fixed */
};

/* One per format adapter (category "PluginFormat"). */
struct IPluginFormat : IUnknown {
  SQ_INTERFACE(IPluginFormat, IUnknown, "{83D53BB1-2720-4495-ABF5-770E9FA3FA2B}")
  virtual sq_result SQ_CALL getFormatInfo(sq_plugin_format_info* out) = 0;
  virtual uint32_t SQ_CALL getSearchPathCount() = 0;
  virtual sq_result SQ_CALL getSearchPath(uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  /* Loads the file and reports every class. Called ONLY inside the scanner process. */
  virtual sq_result SQ_CALL scanFile(const char* path, IPluginScanSink* sink) = 0;
  /* host is add-ref'd by the instance. */
  virtual sq_result SQ_CALL instantiate(const sq_plugin_desc* desc, IPluginHostCallback* host, IPluginInstance** out) = 0;
};

/* Host-side view of the scan results (pluginscanner service; host->queryService). QI: ISubject,
   topic "plugins.changed" after a rescan. */
struct IPluginRegistry : IUnknown {
  SQ_INTERFACE(IPluginRegistry, IUnknown, "{EBAD40F2-CB48-4EC5-B072-8E11148B3398}")
  virtual uint32_t SQ_CALL getPluginCount() = 0;
  virtual sq_result SQ_CALL getPlugin(uint32_t index, sq_plugin_desc* out) = 0;
  /* Starts an out-of-process scan of new/changed files (full = SQ_TRUE rescans everything). */
  virtual sq_result SQ_CALL rescan(sq_bool full) = 0;
  virtual sq_bool SQ_CALL isScanning() = 0;
  virtual sq_result SQ_CALL setBlocked(const char* format, const char* id, sq_bool blocked) = 0;
  /* Convenience: find the format adapter by sq_plugin_desc.format and instantiate. */
  virtual sq_result SQ_CALL instantiate(const sq_plugin_desc* desc, IPluginHostCallback* host, IPluginInstance** out) = 0;
};

namespace pluginhost {
inline constexpr const char* kCategory = "PluginFormat";
inline constexpr sq_guid kClapFormatClassId = sq::guid("{D89D8C48-A3A0-4F21-B522-B77B3BC40EBF}");
inline constexpr sq_guid kVst3FormatClassId = sq::guid("{CAE825D5-0CA3-4D05-82E4-E66EA97FE023}");
}  // namespace pluginhost

}  // namespace sq
#endif /* __cplusplus */

#endif /* SQ_INTERFACES_PLUGINHOST_H */
