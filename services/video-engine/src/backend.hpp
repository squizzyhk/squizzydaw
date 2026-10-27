// video-engine: backend seam. Each backend opens files into IVideoReader objects.
#pragma once

#include <memory>
#include <string>

#include "sq/interfaces/video.h"

namespace sq::video {

class VideoBackend {
 public:
  virtual ~VideoBackend() = default;
  virtual const char* name() const = 0;
  // Cheap check; detail explains why not (or carries version text).
  virtual bool available(std::string& detail) = 0;
  // SQ_E_NOTFOUND, SQ_E_UNSUPPORTED (cannot decode), SQ_E_PARSE (no video stream).
  virtual sq_result open(const std::string& path, IVideoReader** out) = 0;
  virtual void shutdown() {}
};

std::unique_ptr<VideoBackend> makeMediaFoundationBackend();  // nullptr off Windows
// Always returns a backend; without SQ_HAVE_FFMPEG it reports itself unavailable. ffmpegDir:
// optional folder holding the LGPL ffmpeg DLLs (Services.video-engine.FfmpegDir).
std::unique_ptr<VideoBackend> makeFfmpegBackend(const std::string& ffmpegDir);

}  // namespace sq::video
