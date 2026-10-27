/* Squizzy Studio - audio device drivers (plan 2.2, M2).
   OWNER: orchestrator (frozen).  IMPLEMENTS: services/audiodriver-wasapi, services/audiodriver-asio
   (one IAudioDriver class each, category "AudioDriver").  CONSUMES: engine (IAudioCallback
   implementer), app/ui (device selection).
   THREADING:
   - IAudioDriver / IAudioDevice methods: main thread (or one control thread), never the RT thread.
   - IAudioCallback::process runs on the driver's realtime thread: CONTRACTS section 8 rules apply
     (no locks, no allocation, no blocking syscalls). onStart/onStop run on the control thread.
   - IAudioDeviceListener is called on a driver notification thread: just post to the main thread.
   Samples cross the boundary as non-interleaved float32 (sq_audio_block); the driver converts to
   and from the device's native format. */
#ifndef SQ_INTERFACES_AUDIODEVICE_H
#define SQ_INTERFACES_AUDIODEVICE_H

#include "sq/media.h"
#include "sq/module.h"

/* sq_audio_device_info.flags */
#define SQ_AUDIO_DEVICE_INPUT          0x01u
#define SQ_AUDIO_DEVICE_OUTPUT         0x02u
#define SQ_AUDIO_DEVICE_DEFAULT_INPUT  0x04u
#define SQ_AUDIO_DEVICE_DEFAULT_OUTPUT 0x08u
#define SQ_AUDIO_DEVICE_EXCLUSIVE      0x10u /* supports exclusive / low-latency mode */
#define SQ_AUDIO_DEVICE_CONTROL_PANEL  0x20u /* showControlPanel() is implemented */

/* sq_audio_device_config.flags */
#define SQ_AUDIO_CONFIG_EXCLUSIVE 0x01u /* request exclusive mode (WASAPI) */

/* sq_audio_io.flags */
#define SQ_AUDIO_IO_XRUN 0x01u /* a dropout happened before this buffer */

/* IAudioDeviceListener::onDeviceReset reasons */
#define SQ_AUDIO_RESET_FORMAT_CHANGED 1 /* driver wants re-configure (sample rate/block size changed) */
#define SQ_AUDIO_RESET_DEVICE_LOST    2 /* unplugged / disabled: device stopped */
#define SQ_AUDIO_RESET_REQUESTED      3 /* driver asks for stop+configure+start (e.g. its panel changed) */

typedef struct sq_audio_driver_info {
  uint32_t struct_size;
  char name[64];       /* "WASAPI", "ASIO", ... */
  uint32_t flags;      /* reserved */
} sq_audio_driver_info;

typedef struct sq_audio_device_info {
  uint32_t struct_size;
  char id[256];        /* stable across runs, UTF-8; persisted in Devices.* settings */
  char name[128];
  uint32_t flags;      /* SQ_AUDIO_DEVICE_* */
  uint32_t max_input_channels;
  uint32_t max_output_channels;
  double default_sample_rate;
  uint32_t default_block_frames;
  uint32_t min_block_frames;
  uint32_t max_block_frames;
  uint32_t block_granularity; /* 0 = any size in [min,max], else multiples/powers per driver */
} sq_audio_device_info;

/* Negotiation: caller fills a request, the device answers with what it actually granted. */
typedef struct sq_audio_device_config {
  uint32_t struct_size;
  double sample_rate;           /* 0 = keep current/default */
  uint32_t block_frames;        /* 0 = default */
  const uint32_t* input_channels;  /* device channel indices to open (NULL + count 0 = none) */
  uint32_t input_channel_count;
  const uint32_t* output_channels;
  uint32_t output_channel_count;
  uint32_t flags;               /* SQ_AUDIO_CONFIG_* */
  sq_sample_format device_format; /* out: native format chosen by the driver */
} sq_audio_device_config;

/* One RT callback's I/O. input/output blocks have the channel counts granted by configure(). */
typedef struct sq_audio_io {
  uint32_t struct_size;
  const sq_audio_block* input;  /* may be NULL when no inputs are open; read-only */
  sq_audio_block* output;       /* must be fully written (silence if nothing to play) */
  sq_sample_time device_time;   /* frames since start() */
  uint64_t host_time_ns;        /* sq_time_monotonic_ns() at the start of this buffer (MIDI alignment) */
  uint32_t flags;               /* SQ_AUDIO_IO_* */
} sq_audio_io;

#ifdef __cplusplus
namespace sq {

struct IAudioDevice;

/* Implemented by the engine. */
struct IAudioCallback : IUnknown {
  SQ_INTERFACE(IAudioCallback, IUnknown, "{CC5974BD-53FD-4751-98F0-6E3820353CE2}")
  /* Control thread, before the first process(). Allocate everything here. */
  virtual sq_result SQ_CALL onStart(const sq_stream_format* input, const sq_stream_format* output,
                                    const sq_latency_info* latency) = 0;
  /* REALTIME thread. */
  virtual void SQ_CALL process(const sq_audio_io* io) = 0;
  /* Control thread, after the last process() returned. */
  virtual void SQ_CALL onStop() = 0;
};

/* Implemented by the consumer (engine/app). Driver notification thread. */
struct IAudioDeviceListener : IUnknown {
  SQ_INTERFACE(IAudioDeviceListener, IUnknown, "{AE9FC969-908F-4E93-BFD5-242687E738C2}")
  virtual void SQ_CALL onDeviceListChanged() = 0;
  virtual void SQ_CALL onDeviceReset(IAudioDevice* device, int32_t reason) = 0; /* device borrowed */
};

/* An opened device. Releasing the last reference stops and closes it. */
struct IAudioDevice : IUnknown {
  SQ_INTERFACE(IAudioDevice, IUnknown, "{42767B7A-9364-4C97-8287-F606AE6DC09C}")
  virtual sq_result SQ_CALL getInfo(sq_audio_device_info* out) = 0;
  /* Fill up to `capacity` entries; *count = total available. */
  virtual sq_result SQ_CALL getSupportedSampleRates(double* rates, uint32_t capacity, uint32_t* count) = 0;
  virtual sq_result SQ_CALL getSupportedBlockSizes(uint32_t* sizes, uint32_t capacity, uint32_t* count) = 0;
  virtual sq_result SQ_CALL getChannelName(sq_bool input, uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  /* Only while stopped. `actual` (required) receives what was granted; SQ_S_FALSE if it differs. */
  virtual sq_result SQ_CALL configure(const sq_audio_device_config* request, sq_audio_device_config* actual) = 0;
  /* Current formats (max_block_frames = granted block size; format = SQ_SAMPLE_F32 at the API). */
  virtual sq_result SQ_CALL getStreamFormat(sq_stream_format* input, sq_stream_format* output) = 0;
  /* Driver-reported input/output latency in frames incl. safety offsets; process_frames = 0. */
  virtual sq_result SQ_CALL getLatency(sq_latency_info* out) = 0;
  /* callback is add-ref'd until stop() returns. */
  virtual sq_result SQ_CALL start(IAudioCallback* callback) = 0;
  /* Blocks until the RT thread left process(); then onStop() is called. */
  virtual sq_result SQ_CALL stop() = 0;
  virtual sq_bool SQ_CALL isRunning() = 0;
  virtual uint64_t SQ_CALL getXrunCount() = 0;
  /* Driver's own settings UI (ASIO). parentWindow = native handle or NULL. SQ_E_NOTIMPL if none. */
  virtual sq_result SQ_CALL showControlPanel(void* parentWindow) = 0;
};

/* One per driver backend (class category "AudioDriver"). */
struct IAudioDriver : IUnknown {
  SQ_INTERFACE(IAudioDriver, IUnknown, "{4F567461-6CB8-45C2-A982-6E843902D3FF}")
  virtual sq_result SQ_CALL getDriverInfo(sq_audio_driver_info* out) = 0;
  /* Re-enumerates hardware; indices are stable until the next refresh. */
  virtual sq_result SQ_CALL refreshDevices() = 0;
  virtual uint32_t SQ_CALL getDeviceCount() = 0;
  virtual sq_result SQ_CALL getDeviceInfo(uint32_t index, sq_audio_device_info* out) = 0;
  /* deviceId from sq_audio_device_info.id. ASIO-style drivers may allow one open device. */
  virtual sq_result SQ_CALL openDevice(const char* deviceId, IAudioDevice** out) = 0;
  /* Listener is add-ref'd until removed. */
  virtual sq_result SQ_CALL addListener(IAudioDeviceListener* listener) = 0;
  virtual sq_result SQ_CALL removeListener(IAudioDeviceListener* listener) = 0;
};

namespace audiodevice {
inline constexpr const char* kCategory = "AudioDriver";
inline constexpr sq_guid kWasapiDriverClassId = sq::guid("{270EDB69-6D46-471E-A5B5-88FEBA6A3019}");
inline constexpr sq_guid kAsioDriverClassId = sq::guid("{59376083-F3CD-4B04-AA5C-8994525282C8}");
}  // namespace audiodevice

}  // namespace sq
#endif /* __cplusplus */

#endif /* SQ_INTERFACES_AUDIODEVICE_H */
