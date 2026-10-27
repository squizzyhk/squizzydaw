/* Squizzy Studio - DSP: runtime-dispatched SIMD op table, time-stretch, analysis, neural
   inference seam (plan 2.7, 2.10).
   OWNER: orchestrator (frozen).
   IMPLEMENTS:
     sq_vector_ops     engine/dsp: static library sq::dsp (link it from any module; it has no DLL
                       dependency) exporting sq_dsp_vector_ops(); kernels generic/SSE4.2/AVX2/
                       AVX-512/NEON selected once at first call.
     ITimeStretcher    services/time-stretch (category "TimeStretch")
     IAnalyzer         services/analysis-beat, analysis-chords, analysis-stems, tuner (category
                       "Analysis")
     INNRuntime        whichever analysis service vendors the inference runtime first registers it
                       with host->registerService; others query it (one runtime per process).
   CONSUMES: engine, plugin hosts, codecs (sample conversion), analysis services, ui (tuner).
   THREADING:
   - sq_vector_ops functions: pure, RT-safe, any thread.
   - ITimeStretcher: configure()/latency queries on a control thread; push/pull/reset/setRatios
     are RT-safe (no allocation after configure).
   - IAnalyzer: prepare/finish on a worker. process() RT-safe when the analyzer reports
     SQ_ANALYZER_REALTIME, else worker only. getRealtimeValue: wait-free, any thread.
   - INNRuntime/INNModel: worker threads only (never RT). run() is not re-entrant per model. */
#ifndef SQ_INTERFACES_DSP_H
#define SQ_INTERFACES_DSP_H

#include "sq/media.h"
#include "sq/module.h"
#include "sq/interfaces/common.h"

/* sq_vector_ops.isa */
#define SQ_ISA_GENERIC 0
#define SQ_ISA_SSE42   1
#define SQ_ISA_AVX2    2
#define SQ_ISA_AVX512  3
#define SQ_ISA_NEON    4

/* Runtime-selected vector kernels. n = element count; buffers need no special alignment.
   In-place (dst == src) is allowed unless noted. */
typedef struct sq_vector_ops {
  uint32_t struct_size;
  int32_t isa;                 /* SQ_ISA_* actually selected */
  const char* isa_name;
  void (*clear)(float* dst, uint32_t n);
  void (*copy)(float* dst, const float* src, uint32_t n);
  void (*add)(float* dst, const float* src, uint32_t n);                     /* dst += src */
  void (*mul)(float* dst, const float* src, uint32_t n);                     /* dst *= src */
  void (*scale)(float* dst, float gain, uint32_t n);                         /* dst *= gain */
  void (*mix)(float* dst, const float* src, float gain, uint32_t n);         /* dst += src*gain */
  void (*mix_ramp)(float* dst, const float* src, float g0, float g1, uint32_t n); /* linear gain g0->g1 */
  float (*peak)(const float* src, uint32_t n);                               /* max |x| */
  float (*sum_squares)(const float* src, uint32_t n);
  float (*dot)(const float* a, const float* b, uint32_t n);
  void (*clamp)(float* dst, float lo, float hi, uint32_t n);
  /* (de)interleave: frames x channels; NOT in-place */
  void (*interleave)(float* dst, const float* const* src, uint32_t channels, uint32_t frames);
  void (*deinterleave)(float* const* dst, const float* src, uint32_t channels, uint32_t frames);
  /* integer <-> float conversion, little endian; i24 = packed 3 bytes. NOT in-place. */
  void (*from_i16)(float* dst, const int16_t* src, uint32_t n);
  void (*from_i24)(float* dst, const uint8_t* src, uint32_t n);
  void (*from_i32)(float* dst, const int32_t* src, uint32_t n);
  void (*to_i16)(int16_t* dst, const float* src, uint32_t n);   /* clipped, no dither */
  void (*to_i24)(uint8_t* dst, const float* src, uint32_t n);
  void (*to_i32)(int32_t* dst, const float* src, uint32_t n);
  void (*from_f64)(float* dst, const double* src, uint32_t n);
  void (*to_f64)(double* dst, const float* src, uint32_t n);
} sq_vector_ops;

#ifdef __cplusplus
extern "C" {
#endif
/* Provided by sq::dsp (engine/dsp). Never NULL; the table is immutable and process-lived.
   Set env SQ_DSP_ISA=generic|sse42|avx2|avx512 to force a lower kernel set (tests/bisecting). */
const sq_vector_ops* sq_dsp_vector_ops(void);
#ifdef __cplusplus
}
#endif

/* ITimeStretcher quality / flags */
#define SQ_STRETCH_QUALITY_PREVIEW  0
#define SQ_STRETCH_QUALITY_STANDARD 1
#define SQ_STRETCH_QUALITY_HIGH     2
#define SQ_STRETCH_REALTIME          0x01u
#define SQ_STRETCH_PRESERVE_FORMANTS 0x02u
#define SQ_STRETCH_PRESERVE_TRANSIENTS 0x04u

typedef struct sq_stretch_config {
  uint32_t struct_size;
  double sample_rate;
  uint32_t channels;
  uint32_t max_block_frames;   /* max frames per push/pull */
  int32_t quality;             /* SQ_STRETCH_QUALITY_* */
  uint32_t flags;
  double max_time_ratio;       /* largest ratio that will be set (buffer sizing) */
} sq_stretch_config;

/* Analyzer */
#define SQ_ANALYZER_REALTIME 0x01u
#define SQ_ANALYZER_OFFLINE  0x02u

#define SQ_ANALYSIS_BEAT     1 /* value: beat index in bar (1 = downbeat) */
#define SQ_ANALYSIS_ONSET    2
#define SQ_ANALYSIS_TEMPO    3 /* value: bpm from this time */
#define SQ_ANALYSIS_CHORD    4 /* label: chord name, value: root pitch class */
#define SQ_ANALYSIS_KEY      5 /* label: key name */
#define SQ_ANALYSIS_NOTE     6 /* value: MIDI pitch (fractional), duration set */
#define SQ_ANALYSIS_DRUM     7 /* value: GM drum note */

typedef struct sq_analyzer_info {
  uint32_t struct_size;
  char kind[32];               /* "tuner", "beat", "chord", "stems", "pitch", "drums" */
  char name[64];
  uint32_t flags;              /* SQ_ANALYZER_* */
} sq_analyzer_info;

typedef struct sq_analysis_event {
  uint32_t struct_size;
  int32_t type;                /* SQ_ANALYSIS_* */
  double time;                 /* seconds from analysis start */
  double duration;
  double value;
  float confidence;            /* 0..1 */
  char label[32];
} sq_analysis_event;

typedef struct sq_tuner_value {
  uint32_t struct_size;
  double frequency_hz;         /* 0 = no pitch */
  double cents;                /* deviation from midi_note */
  int32_t midi_note;
  float confidence;
} sq_tuner_value;

/* NN tensors (plain C layout, row-major). */
#define SQ_TENSOR_F32 1
#define SQ_TENSOR_I64 2
#define SQ_TENSOR_I32 3
#define SQ_TENSOR_U8  4
#define SQ_TENSOR_MAX_RANK 8

typedef struct sq_tensor_info {
  uint32_t struct_size;
  char name[64];
  int32_t dtype;               /* SQ_TENSOR_* */
  uint32_t rank;
  int64_t shape[SQ_TENSOR_MAX_RANK]; /* -1 = dynamic */
} sq_tensor_info;

typedef struct sq_tensor {
  uint32_t struct_size;
  int32_t dtype;
  uint32_t rank;
  int64_t shape[SQ_TENSOR_MAX_RANK];
  void* data;                  /* caller-owned */
  uint64_t byte_size;          /* capacity of data */
} sq_tensor;

#ifdef __cplusplus
namespace sq {

struct ITimeStretcher : IUnknown {
  SQ_INTERFACE(ITimeStretcher, IUnknown, "{ED516DF1-0C1D-48C0-8B23-16C45A219541}")
  virtual sq_result SQ_CALL configure(const sq_stretch_config* config) = 0;
  /* timeRatio = output length / input length (2.0 = half speed); pitch in semitones. RT-safe. */
  virtual sq_result SQ_CALL setRatios(double timeRatio, double pitchSemitones) = 0;
  virtual uint32_t SQ_CALL getLatencyFrames() = 0;
  /* Input frames needed now to be able to pull `outputFrames`. RT-safe. */
  virtual uint32_t SQ_CALL getRequiredInput(uint32_t outputFrames) = 0;
  /* Feeds input (all frames consumed). RT-safe. */
  virtual sq_result SQ_CALL push(const sq_audio_block* input) = 0;
  /* Pulls up to output->frames; *produced may be less until enough input arrived. RT-safe. */
  virtual sq_result SQ_CALL pull(sq_audio_block* output, uint32_t* produced) = 0;
  /* No more input: subsequent pulls drain the remaining output. RT-safe. */
  virtual sq_result SQ_CALL flush() = 0;
  virtual void SQ_CALL reset() = 0;
};

struct IAnalysisResult : IUnknown {
  SQ_INTERFACE(IAnalysisResult, IUnknown, "{0930CC86-7035-401C-8315-FDCA5491FC29}")
  virtual uint32_t SQ_CALL getEventCount() = 0;
  virtual sq_result SQ_CALL getEvent(uint32_t index, sq_analysis_event* out) = 0;
  /* Stem separation output (0 stems for other analyzers). */
  virtual uint32_t SQ_CALL getStemCount() = 0;
  virtual sq_result SQ_CALL getStemName(uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  virtual sq_result SQ_CALL readStem(uint32_t index, sq_sample_time start, sq_audio_block* dst, uint32_t* framesRead) = 0;
};

struct IAnalyzer : IUnknown {
  SQ_INTERFACE(IAnalyzer, IUnknown, "{4A35E15C-5E43-48C5-AE34-67494A0C3A31}")
  virtual sq_result SQ_CALL getInfo(sq_analyzer_info* out) = 0;
  /* options (nullable): analyzer-specific ("stems": int count, "reference_hz": double). */
  virtual sq_result SQ_CALL prepare(double sampleRate, uint32_t channels, uint32_t maxBlockFrames, IAttributes* options) = 0;
  virtual sq_result SQ_CALL process(const sq_audio_block* block) = 0;
  virtual sq_result SQ_CALL finish(IAnalysisResult** out) = 0; /* offline analyzers */
  /* Realtime analyzers (tuner): latest value, wait-free. */
  virtual sq_result SQ_CALL getRealtimeValue(sq_tuner_value* out) = 0;
  virtual void SQ_CALL reset() = 0;
};

struct INNModel : IUnknown {
  SQ_INTERFACE(INNModel, IUnknown, "{C25C36FB-0C59-4173-9EFE-C1EB78B211AD}")
  virtual uint32_t SQ_CALL getInputCount() = 0;
  virtual sq_result SQ_CALL getInputInfo(uint32_t index, sq_tensor_info* out) = 0;
  virtual uint32_t SQ_CALL getOutputCount() = 0;
  virtual sq_result SQ_CALL getOutputInfo(uint32_t index, sq_tensor_info* out) = 0;
  /* outputs[i].data/byte_size supplied by the caller; shape/rank are written. If too small:
     SQ_E_BUFFER_TOO_SMALL with shape + byte_size set to the required values. */
  virtual sq_result SQ_CALL run(const sq_tensor* inputs, uint32_t inputCount, sq_tensor* outputs, uint32_t outputCount) = 0;
};

/* Prediction dispatcher seam: host->queryService(&INNRuntime::iid). */
struct INNRuntime : IUnknown {
  SQ_INTERFACE(INNRuntime, IUnknown, "{9CA7B231-6ED4-4F38-AAA5-83C81A4652E2}")
  virtual sq_result SQ_CALL getRuntimeInfo(char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  /* File path or package:// URL of a model (models ship as data packages). */
  virtual sq_result SQ_CALL loadModel(const char* pathOrUrl, INNModel** out) = 0;
  /* Worker threads the runtime may use per run (0 = runtime default). */
  virtual sq_result SQ_CALL setThreadCount(uint32_t threads) = 0;
};

namespace dsp {
inline constexpr const char* kTimeStretchCategory = "TimeStretch";
inline constexpr const char* kAnalysisCategory = "Analysis";
inline constexpr sq_guid kTimeStretchClassId = sq::guid("{17EA45EE-FBE1-4B4E-A529-9FE4A02C10C7}");
inline constexpr sq_guid kBeatAnalyzerClassId = sq::guid("{B61E2AB6-0D38-4DAE-87BF-9F644A3707CD}");
inline constexpr sq_guid kChordAnalyzerClassId = sq::guid("{73C00257-3793-4B4B-954E-10EC12390BE3}");
inline constexpr sq_guid kStemSeparatorClassId = sq::guid("{CA6C1310-0588-47A8-96BB-F1F9614DFBA0}");
inline constexpr sq_guid kTunerClassId = sq::guid("{C09DCE4F-A121-4B31-B591-26E6FF344401}");
}  // namespace dsp

}  // namespace sq
#endif /* __cplusplus */

#endif /* SQ_INTERFACES_DSP_H */
