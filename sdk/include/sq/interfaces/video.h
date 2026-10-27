/* Squizzy Studio - video decoding seam (plan 2.2 "video engine", M7 "video track decode").
   OWNER: services/video-engine.
   IMPLEMENTS: services/video-engine (IVideoEngine, IVideoReader). Backends: Media Foundation
               (Windows system component) and an optional ffmpeg backend (LGPL build, dynamically
               linked, only compiled when its SDK path is supplied via a CMake cache var).
   CONSUMES: ui (video track thumbnails / preview), engine (video track timing), tools.
   Registered as a host service (host->registerService(&IVideoEngine::iid)) and as a class in
   the registry under category "Video".
   THREADING: one IVideoReader is NOT thread-safe: use it from one thread at a time (a worker
   thread, never the audio thread: decoding allocates and blocks). Different readers may be used
   on different threads concurrently. IVideoEngine is thread-safe. */
#ifndef SQ_INTERFACES_VIDEO_H
#define SQ_INTERFACES_VIDEO_H

#include "sq/module.h"

/* sq_video_info.pixel_format / sq_video_frame.pixel_format */
#define SQ_PIXEL_BGRA8 1 /* 4 bytes per pixel: B, G, R, A (A = 255); rows top-down */

/* Largest frame a reader accepts (each dimension). */
#define SQ_VIDEO_MAX_DIMENSION 16384u

typedef struct sq_video_info {
  uint32_t struct_size;
  uint32_t width;            /* decoded frame size in pixels */
  uint32_t height;
  uint32_t frame_rate_num;   /* nominal rate; 0/0 when unknown or variable */
  uint32_t frame_rate_den;
  uint32_t pixel_aspect_num; /* 1/1 for square pixels */
  uint32_t pixel_aspect_den;
  uint32_t pixel_format;     /* SQ_PIXEL_* of frames returned by readFrame */
  int64_t duration_ns;       /* 0 when unknown */
  sq_bool has_audio;         /* the container also carries an audio stream (not decoded here) */
  char codec[32];            /* informational, e.g. "h264", "hevc", "vp9" */
  char backend[32];          /* backend that opened the file, e.g. "mediafoundation" */
} sq_video_info;

typedef struct sq_video_frame {
  uint32_t struct_size;
  uint32_t width;
  uint32_t height;
  uint32_t stride;           /* bytes per row written to the caller's buffer (width * 4) */
  uint32_t pixel_format;     /* SQ_PIXEL_* */
  int64_t timestamp_ns;      /* presentation time from the start of the stream */
  int64_t duration_ns;       /* 0 when unknown */
  uint64_t index;            /* frames returned since open or the last seek */
} sq_video_frame;

typedef struct sq_video_backend_info {
  uint32_t struct_size;
  char name[32];             /* "mediafoundation", "ffmpeg" */
  sq_bool available;         /* usable in this process right now */
  char detail[256];          /* why not, or version text */
} sq_video_backend_info;

#ifdef __cplusplus
namespace sq {

struct IVideoReader : IUnknown {
  SQ_INTERFACE(IVideoReader, IUnknown, "{04A1FDC9-025C-4395-857E-BD556A5DADE0}")
  virtual sq_result SQ_CALL getInfo(sq_video_info* out) = 0;
  /* Decodes the next frame into buf (bufSize >= width * height * 4 for SQ_PIXEL_BGRA8) and fills
     *frame. SQ_S_FALSE at end of stream. SQ_E_BUFFER_TOO_SMALL (frame->width/height/stride filled,
     the frame is kept for the next call) when buf is too small. The frame size can change
     mid-stream: check frame->width/height every time. */
  virtual sq_result SQ_CALL readFrame(void* buf, uint32_t bufSize, sq_video_frame* frame) = 0;
  /* After seek(t) the next readFrame returns the frame on screen at time t (the last frame whose
     timestamp <= t), decoding from the previous key frame as needed. t beyond the end: the next
     readFrame returns SQ_S_FALSE. */
  virtual sq_result SQ_CALL seek(int64_t timeNs) = 0;
};

struct IVideoEngine : IUnknown {
  SQ_INTERFACE(IVideoEngine, IUnknown, "{DB350B73-187D-4067-BEC9-5DADAD402C11}")
  /* path: UTF-8 file path. backend: NULL or "" = try every available backend in order;
     otherwise only the named one. SQ_E_NOTFOUND (no file), SQ_E_UNSUPPORTED (no backend can
     decode it / backend unavailable), SQ_E_PARSE (no video stream), SQ_E_INVALIDARG (unknown
     backend name). */
  virtual sq_result SQ_CALL openFile(const char* path, const char* backend, IVideoReader** out) = 0;
  virtual uint32_t SQ_CALL getBackendCount() = 0;
  virtual sq_result SQ_CALL getBackendInfo(uint32_t index, sq_video_backend_info* out) = 0;
};

namespace video {
inline constexpr sq_guid kVideoEngineClassId = sq::guid("{6BFDA90D-C7BD-4B63-8CF6-8753800F1802}");
inline constexpr const char* kVideoCategory = "Video";
}  // namespace video

}  // namespace sq
#endif /* __cplusplus */

#endif /* SQ_INTERFACES_VIDEO_H */
