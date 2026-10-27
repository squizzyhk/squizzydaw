/* Squizzy Studio SDK - shared media POD types used across module boundaries
   (audio drivers <-> engine <-> plugin hosts <-> codecs <-> analysis).
   Part of the core ABI (guarded by sdk/abi.lock). Keep it small: richer, area-specific types go
   into sdk/include/sq/interfaces/<area>.h. All structs are plain data, safe on the audio thread. */
#ifndef SQ_MEDIA_H
#define SQ_MEDIA_H

#include "sq/types.h"

/* Sample positions / lengths in frames at the stream's sample rate. */
typedef int64_t sq_sample_time;

/* Musical time in PPQ (quarter notes) as double. */
typedef double sq_ppq_time;

typedef enum sq_sample_format {
  SQ_SAMPLE_UNKNOWN = 0,
  SQ_SAMPLE_F32 = 1,      /* engine native */
  SQ_SAMPLE_F64 = 2,      /* 64-bit mix bus option */
  SQ_SAMPLE_I16 = 3,
  SQ_SAMPLE_I24 = 4,      /* packed 3 bytes, little endian */
  SQ_SAMPLE_I24_IN32 = 5, /* 24-bit in the low/high bytes of 32 per driver flag */
  SQ_SAMPLE_I32 = 6
} sq_sample_format;

/* Non-interleaved block of float32 audio, the engine's processing currency.
   channels[c] points to `frames` samples. Buffers are owned by the caller of the process call;
   the callee must not keep the pointers after returning. */
typedef struct sq_audio_block {
  float* const* channels;
  uint32_t channel_count;
  uint32_t frames;
  sq_sample_time position; /* stream position of the first frame */
} sq_audio_block;

/* Stream format negotiated between a device/codec and the engine. */
typedef struct sq_stream_format {
  uint32_t struct_size; /* = sizeof(sq_stream_format) */
  double sample_rate;
  uint32_t channels;
  uint32_t max_block_frames;
  sq_sample_format format;
} sq_stream_format;

/* Latency bookkeeping in frames (plan 2.3: designed before the mixer). */
typedef struct sq_latency_info {
  uint32_t struct_size;
  uint32_t input_frames;   /* device/driver input latency */
  uint32_t output_frames;  /* device/driver output latency */
  uint32_t process_frames; /* algorithmic latency of a processor (plugin delay compensation) */
} sq_latency_info;

/* Short MIDI 1.0 message; sysex goes through data/size. */
typedef struct sq_midi_event {
  uint32_t frame_offset; /* within the current block */
  uint8_t bytes[4];      /* status, data1, data2, (unused) */
  uint32_t size;         /* 1..3 for short messages; sysex length when data != NULL */
  const uint8_t* data;   /* sysex payload (valid during the call only) or NULL */
} sq_midi_event;

#endif /* SQ_MEDIA_H */
