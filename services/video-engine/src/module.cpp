// video-engine module: IVideoEngine (sq/interfaces/video.h) over the Media Foundation backend
// and the optional ffmpeg seam. Settings (read when the engine object is created):
//   Services.video-engine.Backends   "mediafoundation,ffmpeg"  order tried by openFile(path, "")
//   Services.video-engine.FfmpegDir  ""  folder with the LGPL ffmpeg DLLs (ffmpeg seam builds only)
// Nothing is started in onInit: Media Foundation starts on the first open/getBackendInfo.
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "backend.hpp"
#include "frame_util.hpp"
#include "sq/platform.hpp"
#include "sq/sdk.hpp"

namespace {

using sq::video::VideoBackend;

// Backends are process-wide (Media Foundation start/stop is global): created on first use and
// shut down in onShutdown. Guarded by g_m.
std::mutex g_m;
std::vector<std::unique_ptr<VideoBackend>>* g_backends = nullptr;

std::vector<VideoBackend*> backends() {
  std::lock_guard<std::mutex> lk(g_m);
  if (!g_backends) {
    g_backends = new std::vector<std::unique_ptr<VideoBackend>>();
    std::string ffDir;
    std::string order = "mediafoundation,ffmpeg";
    if (sq::IHostContext* h = sq::host()) {
      ffDir = sq::getString(h->getSettings(), "Services.video-engine.FfmpegDir", "");
      order = sq::getString(h->getSettings(), "Services.video-engine.Backends", order);
    }
    std::vector<std::unique_ptr<VideoBackend>> all;
    if (auto mf = sq::video::makeMediaFoundationBackend()) all.push_back(std::move(mf));
    all.push_back(sq::video::makeFfmpegBackend(ffDir));
    // Order by the setting; backends it does not name are appended.
    size_t pos = 0;
    while (pos <= order.size()) {
      size_t comma = order.find(',', pos);
      std::string name = order.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
      for (auto& b : all)
        if (b && name == b->name()) g_backends->push_back(std::move(b));
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
    for (auto& b : all)
      if (b) g_backends->push_back(std::move(b));
  }
  std::vector<VideoBackend*> out;
  for (auto& b : *g_backends) out.push_back(b.get());
  return out;
}

class VideoEngine final : public sq::Object<VideoEngine, sq::IVideoEngine> {
 public:
  sq_result SQ_CALL openFile(const char* path, const char* backend, sq::IVideoReader** out) override {
    if (!path || !*path || !out) return SQ_E_INVALIDARG;
    *out = nullptr;
    try {
      if (!sq_fs_is_file(path)) return SQ_E_NOTFOUND;
      const bool any = !backend || !*backend;
      sq_result best = SQ_E_UNSUPPORTED;
      bool matched = false;
      for (VideoBackend* b : backends()) {
        if (!any && std::strcmp(backend, b->name()) != 0) continue;
        matched = true;
        std::string detail;
        if (!b->available(detail)) continue;
        sq_result r = b->open(path, out);
        if (SQ_SUCCEEDED(r)) return r;
        if (r == SQ_E_PARSE) best = r;  // "no video stream" beats "cannot decode"
      }
      if (!matched) return SQ_E_INVALIDARG;  // unknown backend name
      return best;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  uint32_t SQ_CALL getBackendCount() override {
    try {
      return static_cast<uint32_t>(backends().size());
    } catch (...) {
      return 0;
    }
  }

  sq_result SQ_CALL getBackendInfo(uint32_t index, sq_video_backend_info* out) override {
    if (!out) return SQ_E_INVALIDARG;
    try {
      auto list = backends();
      if (index >= list.size()) return SQ_E_NOTFOUND;
      std::string detail;
      bool ok = list[index]->available(detail);
      *out = sq_video_backend_info{};
      out->struct_size = sizeof *out;
      sq::video::copyName(out->name, sizeof out->name, list[index]->name());
      out->available = ok ? SQ_TRUE : SQ_FALSE;
      sq::video::copyName(out->detail, sizeof out->detail, detail.c_str());
      return SQ_OK;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }
};

VideoEngine* g_engine = nullptr;

void onShutdown() {
  if (g_engine) {
    if (sq::IHostContext* h = sq::host()) h->unregisterService(&sq::IVideoEngine::iid, g_engine);
    g_engine->release();
    g_engine = nullptr;
  }
  std::vector<std::unique_ptr<VideoBackend>>* list = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_m);
    list = g_backends;
    g_backends = nullptr;
  }
  if (list) {
    for (auto& b : *list) b->shutdown();
    delete list;
  }
}

sq_result onInit(sq::IHostContext* host) {
  g_engine = nullptr;
  try {
    g_engine = new VideoEngine();
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
  if (SQ_FAILED(host->registerService(&sq::IVideoEngine::iid, g_engine)))
    sq::logf(SQ_LOG_INFO, "IVideoEngine already provided; not registering the singleton");
  return SQ_OK;
}

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<VideoEngine>(sq::video::kVideoEngineClassId, sq::video::kVideoCategory, "Video Engine"),
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('V', 'I', 'D', 'E'),
    .name = "video-engine",
    .description = "Video decoding to frames + timestamps (Media Foundation; optional LGPL ffmpeg seam)",
    .classes = kClasses,
    .onInit = &onInit,
    .onShutdown = &onShutdown,
};

}  // namespace

SQ_MODULE_EXPORTS(kModule)
