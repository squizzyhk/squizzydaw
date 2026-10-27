/* Squizzy Studio - audio file codecs (plan 2.2, M7).
   OWNER: orchestrator (frozen).
   IMPLEMENTS: services/codec-wav (WAV/RF64/AIFF), codec-flac, codec-mp3, codec-ogg (Vorbis/Opus),
               codec-aac. One ICodecService class per module, category "Codec".
   CONSUMES: engine (playback/record/render), formats, sqlite-index (metadata), tools.
   THREADING: reader/writer objects are single-threaded and NOT realtime safe (file I/O). The
   engine reads/writes them on disk-streaming worker threads and hands buffers to the RT thread.
   ICodecService methods are thread-safe.

   METADATA KEYS (generic key/blob API; values are raw bytes, text is UTF-8 without NUL):
     "chunk:<id>"  raw container chunk payload, <id> = the 4-char chunk id with trailing spaces
                   kept, e.g. "chunk:bext", "chunk:smpl", "chunk:CHAN", "chunk:cue ", "chunk:LIST",
                   "chunk:iXML", "chunk:ID3 ". Writers emit unknown chunk keys verbatim (if the
                   container has chunks) and SQ_E_UNSUPPORTED otherwise.
     "tag:<name>"  normalized text tags: title, artist, album, genre, date, comment, track,
                   copyright, encoder, bpm, key, isrc, originator (bext), origination_date.
   Markers/regions/loops (cue + LIST/adtl + smpl loops, ID3 chapters...) are ALSO parsed into
   sq_audio_file_marker for convenience; writers turn addMarker() into the container's form. */
#ifndef SQ_INTERFACES_CODEC_H
#define SQ_INTERFACES_CODEC_H

#include <string.h>
#include "sq/media.h"
#include "sq/module.h"
#include "sq/interfaces/common.h"

/* sq_codec_format_info.flags / supports() flags */
#define SQ_CODEC_READ     0x01u
#define SQ_CODEC_WRITE    0x02u
#define SQ_CODEC_LOSSY    0x04u
#define SQ_CODEC_METADATA 0x08u /* chunk/tag metadata supported */
#define SQ_CODEC_MARKERS  0x10u

/* sq_audio_file_format.flags */
#define SQ_AUDIOFILE_SEEKABLE     0x01u
#define SQ_AUDIOFILE_SAMPLE_EXACT 0x02u /* seeks are sample accurate */
#define SQ_AUDIOFILE_VBR          0x04u

/* sq_audio_file_marker.flags */
#define SQ_MARKER_REGION 0x01u /* length > 0 */
#define SQ_MARKER_LOOP   0x02u /* smpl loop */

typedef struct sq_codec_info {
  uint32_t struct_size;
  char name[64];         /* "WAV/AIFF", "FLAC", ... */
  uint32_t flags;        /* union of the formats' flags */
} sq_codec_info;

typedef struct sq_codec_format_info {
  uint32_t struct_size;
  char extension[16];    /* lowercase, no dot: "wav", "aif", "flac" */
  char mime[64];         /* "audio/wav" */
  char description[96];
  uint32_t flags;        /* SQ_CODEC_* */
} sq_codec_format_info;

typedef struct sq_audio_file_format {
  uint32_t struct_size;
  double sample_rate;
  uint32_t channels;
  sq_sample_format sample_format; /* stored format (writer: requested; lossy codecs: F32) */
  uint32_t bits_per_sample;       /* stored bits (e.g. 24) */
  sq_sample_time frame_count;     /* -1 = unknown (streams) */
  uint32_t bitrate_kbps;          /* lossy; 0 = n/a or writer default */
  uint32_t flags;                 /* SQ_AUDIOFILE_* (reader only) */
  char container[16];             /* format extension: "wav", "rf64", "aif", "flac", ... */
} sq_audio_file_format;

typedef struct sq_audio_file_marker {
  uint32_t struct_size;
  uint32_t id;           /* cue point id or index */
  sq_sample_time position;
  sq_sample_time length; /* 0 = point marker */
  uint32_t flags;        /* SQ_MARKER_* */
  char name[128];
} sq_audio_file_marker;

#ifdef __cplusplus
namespace sq {

struct IAudioFileReader : IUnknown {
  SQ_INTERFACE(IAudioFileReader, IUnknown, "{02494C14-10A1-4BDE-8D05-176863513F96}")
  virtual sq_result SQ_CALL open(const char* path) = 0;
  virtual sq_result SQ_CALL openStream(IByteStream* stream) = 0; /* stream add-ref'd */
  virtual sq_result SQ_CALL getFormat(sq_audio_file_format* out) = 0;
  /* Reads up to dst->frames frames into dst->channels (float32, non-interleaved; channel_count
     must equal the file's). *framesRead < frames only at end of file. dst->position ignored. */
  virtual sq_result SQ_CALL read(sq_audio_block* dst, uint32_t* framesRead) = 0;
  virtual sq_result SQ_CALL seek(sq_sample_time frame) = 0;
  virtual sq_result SQ_CALL tell(sq_sample_time* frame) = 0;
  virtual uint32_t SQ_CALL getMetadataCount() = 0;
  virtual sq_result SQ_CALL getMetadataKey(uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) = 0;
  virtual sq_result SQ_CALL getMetadata(const char* key, void* buf, uint32_t bufSize, uint32_t* needed) = 0;
  virtual uint32_t SQ_CALL getMarkerCount() = 0;
  virtual sq_result SQ_CALL getMarker(uint32_t index, sq_audio_file_marker* out) = 0;
};

/* Sequence: open -> setMetadata/addMarker (any time before finalize) -> write* -> finalize.
   Releasing without finalize() aborts: a file opened by path is deleted. */
struct IAudioFileWriter : IUnknown {
  SQ_INTERFACE(IAudioFileWriter, IUnknown, "{A48FE537-D298-4D66-8B8D-13AA2F16A137}")
  /* options (nullable): "quality" double 0..1, "bitrate_kbps" int, "dither" bool, "rf64" bool
     ("auto" if absent: switch to RF64 above 4 GB). Unknown options are ignored. */
  virtual sq_result SQ_CALL open(const char* path, const sq_audio_file_format* format, IAttributes* options) = 0;
  virtual sq_result SQ_CALL openStream(IByteStream* stream, const sq_audio_file_format* format, IAttributes* options) = 0;
  virtual sq_result SQ_CALL setMetadata(const char* key, const void* data, uint32_t size) = 0;
  virtual sq_result SQ_CALL addMarker(const sq_audio_file_marker* marker) = 0;
  /* float32 non-interleaved, channel_count == format channels; converted to the stored format. */
  virtual sq_result SQ_CALL write(const sq_audio_block* src) = 0;
  virtual sq_result SQ_CALL finalize() = 0;
};

/* One per codec module (category "Codec"). */
struct ICodecService : IUnknown {
  SQ_INTERFACE(ICodecService, IUnknown, "{F28C8571-4293-428A-AACA-56A419A5430D}")
  virtual sq_result SQ_CALL getCodecInfo(sq_codec_info* out) = 0;
  virtual uint32_t SQ_CALL getFormatCount() = 0;
  virtual sq_result SQ_CALL getFormatInfo(uint32_t index, sq_codec_format_info* out) = 0;
  /* extension (no dot, any case) and/or mime may be NULL; flags = SQ_CODEC_READ / _WRITE wanted. */
  virtual sq_bool SQ_CALL supports(const char* extension, const char* mime, uint32_t flags) = 0;
  /* Content sniffing: score 0 (not mine) .. 100 (certain) from the first bytes of a file. */
  virtual int32_t SQ_CALL probe(const void* header, uint32_t size) = 0;
  virtual sq_result SQ_CALL createReader(IAudioFileReader** out) = 0;
  /* extension selects the container when the codec writes several ("wav" / "aif"). */
  virtual sq_result SQ_CALL createWriter(const char* extension, IAudioFileWriter** out) = 0;
};

namespace codec {
inline constexpr const char* kCategory = "Codec";
inline constexpr sq_guid kWavCodecClassId = sq::guid("{DF39FBA4-98ED-4162-B041-5E2BB589B536}");
inline constexpr sq_guid kFlacCodecClassId = sq::guid("{8FBB943E-37BC-4B8A-B0AB-ABD18C348B03}");
inline constexpr sq_guid kMp3CodecClassId = sq::guid("{2CC9BFE1-6A7F-4678-A952-EE1B6C3239CD}");
inline constexpr sq_guid kOggCodecClassId = sq::guid("{8EC95276-A9A0-4E0F-B051-EE4188A49A4F}");
inline constexpr sq_guid kAacCodecClassId = sq::guid("{C8A345D8-4023-450B-977D-353DC296497E}");

/* Capability query across all loaded codecs: first ICodecService supporting extension/mime
   with `flags`. Returns an add-ref'd service or nullptr. Main/worker thread. */
inline ICodecService* findCodec(IClassRegistry* reg, const char* extension, const char* mime, uint32_t flags) {
  if (!reg) return nullptr;
  const uint32_t n = reg->getClassCount();
  for (uint32_t i = 0; i < n; ++i) {
    sq_class_info ci{};
    ci.struct_size = sizeof ci;
    if (reg->getClassInfo(i, &ci) != SQ_OK || !ci.category || strcmp(ci.category, kCategory) != 0) continue;
    void* p = nullptr;
    if (reg->createInstance(&ci.class_id, &ICodecService::iid, &p) != SQ_OK || !p) continue;
    ICodecService* svc = static_cast<ICodecService*>(p);
    if (svc->supports(extension, mime, flags)) return svc;
    svc->release();
  }
  return nullptr;
}
}  // namespace codec

}  // namespace sq
#endif /* __cplusplus */

#endif /* SQ_INTERFACES_CODEC_H */
