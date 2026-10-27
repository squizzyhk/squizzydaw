/* Squizzy Studio - audio engine control: device binding, transport, monitoring, latency, render
   tasks (plan 2.3, M2/M3/M7).
   OWNER: orchestrator (frozen).
   IMPLEMENTS: engine (registers IAudioEngine with host->registerService; QI gives ITransport and
               ISubject).
   CONSUMES: app, ui, scripting-js, tools, remote (transport/tempo sync).
   THREADING:
   - Control methods (device, document, monitoring, render): main thread.
   - ITransport commands: main thread. ITransport::getTransportState, IAudioEngine::getStatus and
     getMeter are wait-free snapshots, callable from any thread except the audio thread.
   - IRenderObserver runs on the render worker thread.
   - Topics on ISubject (main thread): "engine.device" (device opened/closed/reset),
     "engine.latency" (latency totals changed), "transport.state" (play/stop/record/loop changed). */
#ifndef SQ_INTERFACES_AUDIOENGINE_H
#define SQ_INTERFACES_AUDIOENGINE_H

#include "sq/media.h"
#include "sq/module.h"
#include "sq/interfaces/common.h"
#include "sq/interfaces/audiodevice.h"
#include "sq/interfaces/document.h"

/* sq_engine_status.flags */
#define SQ_ENGINE_DEVICE_OPEN 0x01u
#define SQ_ENGINE_RUNNING     0x02u
#define SQ_ENGINE_LOW_LATENCY 0x04u /* fast-lane monitoring active */
#define SQ_ENGINE_OFFLINE     0x08u /* rendering faster than realtime */

/* sq_transport_state.flags */
#define SQ_TRANSPORT_PLAYING   0x01u
#define SQ_TRANSPORT_RECORDING 0x02u
#define SQ_TRANSPORT_LOOPING   0x04u
#define SQ_TRANSPORT_PUNCH     0x08u
#define SQ_TRANSPORT_METRONOME 0x10u

/* sq_render_request.kind */
#define SQ_RENDER_MIXDOWN         1 /* master to one file */
#define SQ_RENDER_STEMS           2 /* one file per track in track_ids (all tracks if none) */
#define SQ_RENDER_BOUNCE_IN_PLACE 3 /* tracks' range rendered, clips replaced in one undo step */

/* sq_render_request.flags */
#define SQ_RENDER_REALTIME   0x01u /* render through the device clock (external gear) */
#define SQ_RENDER_TAIL       0x02u /* append tail_seconds (effects decay) */
#define SQ_RENDER_NORMALIZE  0x04u
#define SQ_RENDER_PRE_FADER  0x08u /* stems: before fader/pan */

typedef struct sq_engine_latency {
  uint32_t struct_size;
  uint32_t device_input_frames;   /* from IAudioDevice::getLatency */
  uint32_t device_output_frames;
  uint32_t block_frames;
  uint32_t plugin_delay_frames;   /* max compensated processing latency in the main graph */
  uint32_t main_roundtrip_frames; /* input -> main graph -> output */
  uint32_t fast_lane_roundtrip_frames; /* input -> fast-lane monitor -> output */
} sq_engine_latency;

typedef struct sq_engine_status {
  uint32_t struct_size;
  uint32_t flags;                 /* SQ_ENGINE_* */
  double sample_rate;
  uint32_t block_frames;
  double cpu_load;                /* 0..1 of the block budget, smoothed */
  uint64_t xruns;
  sq_guid driver_class;           /* null when no device */
  char device_id[256];
} sq_engine_status;

typedef struct sq_transport_state {
  uint32_t struct_size;
  uint32_t flags;                 /* SQ_TRANSPORT_* */
  sq_ppq_time position;
  double seconds;
  sq_sample_time frames;
  double bpm;
  uint32_t numerator;
  uint32_t denominator;
  sq_ppq_time loop_start;
  sq_ppq_time loop_end;
} sq_transport_state;

typedef struct sq_meter_value {
  float peak;                     /* linear, since the previous read */
  float rms;
} sq_meter_value;

typedef struct sq_render_request {
  uint32_t struct_size;
  int32_t kind;                   /* SQ_RENDER_* */
  uint32_t flags;                 /* SQ_RENDER_* */
  sq_ppq_time start;
  sq_ppq_time end;
  double tail_seconds;
  const sq_guid* track_ids;       /* stems / bounce: tracks; NULL+0 = all (stems) */
  uint32_t track_count;
  const char* output;             /* mixdown: file path; stems: directory; bounce: NULL (media folder) */
  const char* file_extension;     /* codec lookup (codec.h findCodec), e.g. "wav" */
  double sample_rate;             /* 0 = document rate */
  uint32_t channels;              /* 0 = source channels */
  sq_sample_format sample_format; /* stored format; SQ_SAMPLE_UNKNOWN = codec default */
  sq_IAttributes* codec_options;  /* nullable, passed to IAudioFileWriter::open */
} sq_render_request;

#ifdef __cplusplus
namespace sq {

struct IRenderTask;

struct IRenderObserver : IUnknown {
  SQ_INTERFACE(IRenderObserver, IUnknown, "{34AD59D5-519C-4423-B8D9-871ACBF68E6F}")
  virtual void SQ_CALL onRenderProgress(IRenderTask* task, double progress) = 0; /* 0..1 */
  virtual void SQ_CALL onRenderFinished(IRenderTask* task, sq_result result) = 0;
};

struct IRenderTask : IUnknown {
  SQ_INTERFACE(IRenderTask, IUnknown, "{52B211AE-AEE1-46B8-9F44-84667768A622}")
  virtual double SQ_CALL getProgress() = 0;
  virtual sq_result SQ_CALL cancel() = 0;
  /* SQ_E_TIMEOUT if still running after timeoutMs (0 = poll). */
  virtual sq_result SQ_CALL wait(uint32_t timeoutMs) = 0;
  virtual sq_result SQ_CALL getResult() = 0; /* SQ_E_BUSY while running */
  virtual uint32_t SQ_CALL getOutputCount() = 0;
  virtual sq_result SQ_CALL getOutputPath(uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) = 0;
};

struct ITransport : IUnknown {
  SQ_INTERFACE(ITransport, IUnknown, "{A3D3B5E8-2C15-43F4-92BB-33A3290696C5}")
  virtual sq_result SQ_CALL play() = 0;
  virtual sq_result SQ_CALL stop() = 0;
  virtual sq_result SQ_CALL record(sq_bool enable) = 0;
  virtual sq_result SQ_CALL locate(sq_ppq_time position) = 0;
  virtual sq_result SQ_CALL setLoop(sq_ppq_time start, sq_ppq_time end, sq_bool enabled) = 0;
  virtual sq_result SQ_CALL setMetronome(sq_bool enabled) = 0;
  virtual sq_result SQ_CALL getTransportState(sq_transport_state* out) = 0; /* wait-free */
};

/* Engine singleton: host->queryService(&IAudioEngine::iid). */
struct IAudioEngine : IUnknown {
  SQ_INTERFACE(IAudioEngine, IUnknown, "{D97E9623-B716-478D-B31A-BC8B9FBA370E}")
  /* Opens deviceId of the driver class, negotiates `request` (nullable = settings Devices.*),
     starts the render thread. Replaces any open device. */
  virtual sq_result SQ_CALL openDevice(const sq_guid* driverClassId, const char* deviceId,
                                       const sq_audio_device_config* request) = 0;
  virtual sq_result SQ_CALL closeDevice() = 0;
  virtual sq_result SQ_CALL getStatus(sq_engine_status* out) = 0; /* wait-free */
  /* The document the engine plays/records (NULL = none). Engine rebuilds its RT graph on
     "document.changed" itself. */
  virtual sq_result SQ_CALL setDocument(IDocument* doc) = 0;
  /* Fast-lane (low-latency input monitoring path). */
  virtual sq_result SQ_CALL setLowLatencyMonitoring(sq_bool enabled) = 0;
  virtual sq_result SQ_CALL setTrackMonitoring(const sq_guid* trackId, sq_bool enabled) = 0;
  virtual sq_result SQ_CALL getLatency(sq_engine_latency* out) = 0;
  /* Per node (track/plugin slot) processing latency; process_frames used. */
  virtual sq_result SQ_CALL getNodeLatency(const sq_guid* nodeId, sq_latency_info* out) = 0;
  /* Wait-free meter read of a track (null id = master). *count = channels written. */
  virtual sq_result SQ_CALL getMeter(const sq_guid* trackId, sq_meter_value* out, uint32_t capacity, uint32_t* count) = 0;
  /* Starts a render task on a worker; observer (nullable) add-ref'd until finished. */
  virtual sq_result SQ_CALL startRender(const sq_render_request* request, IRenderObserver* observer, IRenderTask** out) = 0;
};

}  // namespace sq
#endif /* __cplusplus */

#endif /* SQ_INTERFACES_AUDIOENGINE_H */
