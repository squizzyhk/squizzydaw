// video-engine: ffmpeg backend seam (LGPL ffmpeg, DYNAMICALLY linked, never vendored).
//
// Default build (no SQ_FFMPEG_DIR): a stub that reports itself unavailable.
// With -DSQ_FFMPEG_DIR=<ffmpeg "shared" dev package with include/> the real decoder below is
// compiled against the headers only: nothing is linked. available() opens the exact runtime
// libraries this build expects (avformat-<major>.dll / libavformat.so.<major> / ...) from
// Services.video-engine.FfmpegDir or the default search path and resolves every function it uses
// with sq_dylib_symbol; only then is any ffmpeg code called. The module therefore loads (and
// reports ffmpeg unavailable) on machines without ffmpeg. Use an LGPL (not GPL) ffmpeg build.
//
// NOTE: only compiled when SQ_FFMPEG_DIR is supplied (not in the default build). Verified on
// 2026-10-28 against the ffmpeg 8.1 LGPL shared Windows build: CTest video_engine_ffmpeg passed
// (same decode/timestamp/seek checks as Media Foundation).
#include "backend.hpp"
#include "frame_util.hpp"

#if defined(SQ_HAVE_FFMPEG)

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswresample/version.h>
#include <libswscale/swscale.h>
}

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "sq/platform.hpp"
#include "sq/sdk.hpp"

namespace sq::video {
namespace {

#define SQ_STR2(x) #x
#define SQ_STR(x) SQ_STR2(x)

struct FfApi {
  decltype(&::avformat_open_input) avformat_open_input = nullptr;
  decltype(&::avformat_find_stream_info) avformat_find_stream_info = nullptr;
  decltype(&::av_find_best_stream) av_find_best_stream = nullptr;
  decltype(&::avcodec_alloc_context3) avcodec_alloc_context3 = nullptr;
  decltype(&::avcodec_parameters_to_context) avcodec_parameters_to_context = nullptr;
  decltype(&::avcodec_open2) avcodec_open2 = nullptr;
  decltype(&::av_packet_alloc) av_packet_alloc = nullptr;
  decltype(&::av_frame_alloc) av_frame_alloc = nullptr;
  decltype(&::av_version_info) av_version_info = nullptr;
  decltype(&::avformat_close_input) avformat_close_input = nullptr;
  decltype(&::avcodec_free_context) avcodec_free_context = nullptr;
  decltype(&::av_packet_free) av_packet_free = nullptr;
  decltype(&::av_frame_free) av_frame_free = nullptr;
  decltype(&::sws_freeContext) sws_freeContext = nullptr;
  decltype(&::av_frame_unref) av_frame_unref = nullptr;
  decltype(&::sws_getCachedContext) sws_getCachedContext = nullptr;
  decltype(&::sws_scale) sws_scale = nullptr;
  decltype(&::av_rescale_q) av_rescale_q = nullptr;
  decltype(&::av_seek_frame) av_seek_frame = nullptr;
  decltype(&::avcodec_flush_buffers) avcodec_flush_buffers = nullptr;
  decltype(&::avcodec_receive_frame) avcodec_receive_frame = nullptr;
  decltype(&::av_read_frame) av_read_frame = nullptr;
  decltype(&::avcodec_send_packet) avcodec_send_packet = nullptr;
  decltype(&::av_packet_unref) av_packet_unref = nullptr;
};

FfApi g_api;  // filled once by loadApi() (under the backend mutex); read-only afterwards
const FfApi& api() { return g_api; }

std::string libName(const char* base, int major) {
#if defined(_WIN32)
  return std::string(base) + "-" + std::to_string(major) + ".dll";
#elif defined(__APPLE__)
  return "lib" + std::string(base) + "." + std::to_string(major) + ".dylib";
#else
  return "lib" + std::string(base) + ".so." + std::to_string(major);
#endif
}

template <class Fn>
bool resolve(sq_dylib* lib, Fn& fn, const char* name, std::string& detail) {
  fn = reinterpret_cast<Fn>(sq_dylib_symbol(lib, name));
  if (!fn) detail = std::string("ffmpeg symbol missing: ") + name;
  return fn != nullptr;
}

// Opens the four libraries (never closed: ffmpeg stays mapped for the process) and resolves
// every function. False with a reason when anything is missing.
bool loadApi(const std::string& dir, std::string& detail) {
  struct Lib {
    const char* base;
    int major;
    sq_dylib* h;
  } libs[] = {{"avutil", LIBAVUTIL_VERSION_MAJOR, nullptr},
              {"swresample", LIBSWRESAMPLE_VERSION_MAJOR, nullptr},  // avcodec's dependency
              {"avcodec", LIBAVCODEC_VERSION_MAJOR, nullptr},
              {"avformat", LIBAVFORMAT_VERSION_MAJOR, nullptr},
              {"swscale", LIBSWSCALE_VERSION_MAJOR, nullptr}};
  for (auto& l : libs) {
    std::string name = libName(l.base, l.major);
    std::string path = dir.empty() ? name : sq::joinPath(dir, name);
    char err[256] = {};
    if (sq_dylib_open(path.c_str(), &l.h, err, sizeof err) != SQ_OK) {
      if (std::strcmp(l.base, "swresample") == 0) continue;  // optional, depending on the build
      detail = "ffmpeg runtime not found: " + name;
      return false;
    }
  }
  FfApi a;
  std::string d;
  bool ok = true;
  sq_dylib* avformat = libs[3].h;
  sq_dylib* avcodec = libs[2].h;
  sq_dylib* avutil = libs[0].h;
  sq_dylib* swscale = libs[4].h;
  ok = ok && resolve(avformat, a.avformat_open_input, "avformat_open_input", d);
  ok = ok && resolve(avformat, a.avformat_find_stream_info, "avformat_find_stream_info", d);
  ok = ok && resolve(avformat, a.av_find_best_stream, "av_find_best_stream", d);
  ok = ok && resolve(avformat, a.avformat_close_input, "avformat_close_input", d);
  ok = ok && resolve(avformat, a.av_seek_frame, "av_seek_frame", d);
  ok = ok && resolve(avformat, a.av_read_frame, "av_read_frame", d);
  ok = ok && resolve(avcodec, a.avcodec_alloc_context3, "avcodec_alloc_context3", d);
  ok = ok && resolve(avcodec, a.avcodec_parameters_to_context, "avcodec_parameters_to_context", d);
  ok = ok && resolve(avcodec, a.avcodec_open2, "avcodec_open2", d);
  ok = ok && resolve(avcodec, a.av_packet_alloc, "av_packet_alloc", d);
  ok = ok && resolve(avcodec, a.avcodec_free_context, "avcodec_free_context", d);
  ok = ok && resolve(avcodec, a.av_packet_free, "av_packet_free", d);
  ok = ok && resolve(avcodec, a.avcodec_flush_buffers, "avcodec_flush_buffers", d);
  ok = ok && resolve(avcodec, a.avcodec_receive_frame, "avcodec_receive_frame", d);
  ok = ok && resolve(avcodec, a.avcodec_send_packet, "avcodec_send_packet", d);
  ok = ok && resolve(avcodec, a.av_packet_unref, "av_packet_unref", d);
  ok = ok && resolve(avutil, a.av_frame_alloc, "av_frame_alloc", d);
  ok = ok && resolve(avutil, a.av_version_info, "av_version_info", d);
  ok = ok && resolve(avutil, a.av_frame_free, "av_frame_free", d);
  ok = ok && resolve(avutil, a.av_frame_unref, "av_frame_unref", d);
  ok = ok && resolve(avutil, a.av_rescale_q, "av_rescale_q", d);
  ok = ok && resolve(swscale, a.sws_freeContext, "sws_freeContext", d);
  ok = ok && resolve(swscale, a.sws_getCachedContext, "sws_getCachedContext", d);
  ok = ok && resolve(swscale, a.sws_scale, "sws_scale", d);
  if (!ok) {
    detail = d;
    return false;
  }
  g_api = a;
  detail = std::string("ffmpeg ") + g_api.av_version_info();
  return true;
}

class FfReader final : public Object<FfReader, IVideoReader> {
 public:
  ~FfReader() override {
    if (sws_) api().sws_freeContext(sws_);
    if (frame_) api().av_frame_free(&frame_);
    if (pkt_) api().av_packet_free(&pkt_);
    if (dec_) api().avcodec_free_context(&dec_);
    if (fmt_) api().avformat_close_input(&fmt_);
  }

  sq_result open(const std::string& path) {
    if (api().avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) return SQ_E_NOTFOUND;
    if (api().avformat_find_stream_info(fmt_, nullptr) < 0) return SQ_E_PARSE;
    const AVCodec* codec = nullptr;
    stream_ = api().av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream_ < 0 || !codec) return stream_ == AVERROR_DECODER_NOT_FOUND ? SQ_E_UNSUPPORTED : SQ_E_PARSE;
    AVStream* st = fmt_->streams[stream_];
    dec_ = api().avcodec_alloc_context3(codec);
    if (!dec_ || api().avcodec_parameters_to_context(dec_, st->codecpar) < 0) return SQ_E_FAIL;
    if (api().avcodec_open2(dec_, codec, nullptr) < 0) return SQ_E_UNSUPPORTED;
    pkt_ = api().av_packet_alloc();
    frame_ = api().av_frame_alloc();
    if (!pkt_ || !frame_) return SQ_E_OUTOFMEMORY;
    uint64_t bytes = 0;
    if (!frameBytes(static_cast<uint32_t>(dec_->width), static_cast<uint32_t>(dec_->height), bytes)) return SQ_E_UNSUPPORTED;
    info_.struct_size = sizeof info_;
    info_.width = static_cast<uint32_t>(dec_->width);
    info_.height = static_cast<uint32_t>(dec_->height);
    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
      info_.frame_rate_num = static_cast<uint32_t>(st->avg_frame_rate.num);
      info_.frame_rate_den = static_cast<uint32_t>(st->avg_frame_rate.den);
    }
    AVRational sar = st->sample_aspect_ratio.num ? st->sample_aspect_ratio : dec_->sample_aspect_ratio;
    info_.pixel_aspect_num = sar.num > 0 ? static_cast<uint32_t>(sar.num) : 1;
    info_.pixel_aspect_den = sar.den > 0 ? static_cast<uint32_t>(sar.den) : 1;
    info_.pixel_format = SQ_PIXEL_BGRA8;
    if (fmt_->duration > 0) info_.duration_ns = rationalToNs(fmt_->duration, 1, AV_TIME_BASE);
    info_.has_audio = api().av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0 ? SQ_TRUE : SQ_FALSE;
    copyName(info_.codec, sizeof info_.codec, codec->name);
    copyName(info_.backend, sizeof info_.backend, "ffmpeg");
    tb_ = st->time_base;
    return SQ_OK;
  }

  sq_result SQ_CALL getInfo(sq_video_info* out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = info_;
    return SQ_OK;
  }

  sq_result SQ_CALL readFrame(void* buf, uint32_t bufSize, sq_video_frame* frame) override {
    if (!frame) return SQ_E_INVALIDARG;
    for (;;) {
      if (!havePending_) {
        sq_result r = decodeNext();
        if (r != SQ_OK) return r;
      }
      if (filter_.skip(pendingTs_, pendingDur_)) {
        api().av_frame_unref(frame_);
        havePending_ = false;
        continue;
      }
      break;
    }
    const uint32_t w = static_cast<uint32_t>(frame_->width), h = static_cast<uint32_t>(frame_->height);
    *frame = sq_video_frame{};
    frame->struct_size = sizeof *frame;
    frame->width = w;
    frame->height = h;
    frame->stride = w * 4;
    frame->pixel_format = SQ_PIXEL_BGRA8;
    frame->timestamp_ns = pendingTs_;
    frame->duration_ns = pendingDur_;
    frame->index = index_;
    uint64_t need = 0;
    if (!frameBytes(w, h, need)) return SQ_E_UNSUPPORTED;
    if (!buf || bufSize < need) return SQ_E_BUFFER_TOO_SMALL;
    sws_ = api().sws_getCachedContext(sws_, frame_->width, frame_->height, static_cast<AVPixelFormat>(frame_->format),
                                frame_->width, frame_->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) return SQ_E_UNSUPPORTED;
    uint8_t* dst[4] = {static_cast<uint8_t*>(buf), nullptr, nullptr, nullptr};
    int dstStride[4] = {static_cast<int>(w * 4), 0, 0, 0};
    api().sws_scale(sws_, frame_->data, frame_->linesize, 0, frame_->height, dst, dstStride);
    for (uint64_t i = 3; i < need; i += 4) static_cast<uint8_t*>(buf)[i] = 0xFF;
    api().av_frame_unref(frame_);
    havePending_ = false;
    ++index_;
    return SQ_OK;
  }

  sq_result SQ_CALL seek(int64_t timeNs) override {
    if (timeNs < 0) timeNs = 0;
    if (info_.duration_ns > 0 && timeNs >= info_.duration_ns) {  // past the end: next read is EOS
      api().av_frame_unref(frame_);
      havePending_ = false;
      eos_ = true;
      index_ = 0;
      filter_.disarm();
      return SQ_OK;
    }
    int64_t ts = api().av_rescale_q(timeNs, AVRational{1, 1000000000}, tb_);
    if (fmt_->streams[stream_]->start_time != AV_NOPTS_VALUE) ts += fmt_->streams[stream_]->start_time;
    if (api().av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_BACKWARD) < 0) return SQ_E_IO;
    api().avcodec_flush_buffers(dec_);
    api().av_frame_unref(frame_);
    havePending_ = false;
    draining_ = false;
    eos_ = false;
    index_ = 0;
    filter_.arm(timeNs);
    return SQ_OK;
  }

 private:
  sq_result decodeNext() {
    if (eos_) return SQ_S_FALSE;
    for (;;) {
      int r = api().avcodec_receive_frame(dec_, frame_);
      if (r == 0) {
        int64_t pts = frame_->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) pts = frame_->pts;
        const int64_t start = fmt_->streams[stream_]->start_time;
        if (pts != AV_NOPTS_VALUE && start != AV_NOPTS_VALUE) pts -= start;
        pendingTs_ = pts == AV_NOPTS_VALUE ? 0 : rationalToNs(pts, tb_.num, tb_.den);
#if LIBAVUTIL_VERSION_MAJOR >= 58
        pendingDur_ = frame_->duration > 0 ? rationalToNs(frame_->duration, tb_.num, tb_.den) : 0;
#else
        pendingDur_ = frame_->pkt_duration > 0 ? rationalToNs(frame_->pkt_duration, tb_.num, tb_.den) : 0;
#endif
        havePending_ = true;
        return SQ_OK;
      }
      if (r == AVERROR_EOF) {
        eos_ = true;
        return SQ_S_FALSE;
      }
      if (r != AVERROR(EAGAIN)) return SQ_E_IO;
      if (draining_) {
        eos_ = true;
        return SQ_S_FALSE;
      }
      r = api().av_read_frame(fmt_, pkt_);
      if (r < 0) {
        draining_ = true;
        api().avcodec_send_packet(dec_, nullptr);  // flush the decoder
        continue;
      }
      if (pkt_->stream_index == stream_) api().avcodec_send_packet(dec_, pkt_);
      api().av_packet_unref(pkt_);
    }
  }

  AVFormatContext* fmt_ = nullptr;
  AVCodecContext* dec_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVFrame* frame_ = nullptr;
  SwsContext* sws_ = nullptr;
  int stream_ = -1;
  AVRational tb_{1, 1};
  sq_video_info info_{};
  bool havePending_ = false, draining_ = false, eos_ = false;
  int64_t pendingTs_ = 0, pendingDur_ = 0;
  uint64_t index_ = 0;
  SeekFilter filter_;
};

class FfmpegBackend final : public VideoBackend {
 public:
  explicit FfmpegBackend(std::string dir) : dir_(std::move(dir)) {}
  const char* name() const override { return "ffmpeg"; }
  bool available(std::string& detail) override {
    std::lock_guard<std::mutex> lk(m_);
    if (!tried_) {
      tried_ = true;
      ok_ = loadApi(dir_, detail_);
    }
    detail = detail_;
    return ok_;
  }
  sq_result open(const std::string& path, IVideoReader** out) override {
    std::string d;
    if (!available(d)) return SQ_E_UNSUPPORTED;
    auto r = Ptr<FfReader>::adopt(new FfReader());
    sq_result res = r->open(path);
    if (SQ_FAILED(res)) return res;
    *out = r.detach();
    return SQ_OK;
  }

 private:
  std::string dir_;
  std::mutex m_;
  bool tried_ = false, ok_ = false;
  std::string detail_;
};

}  // namespace

std::unique_ptr<VideoBackend> makeFfmpegBackend(const std::string& ffmpegDir) {
  return std::make_unique<FfmpegBackend>(ffmpegDir);
}

}  // namespace sq::video

#else  // !SQ_HAVE_FFMPEG

namespace sq::video {
namespace {
class FfmpegStub final : public VideoBackend {
 public:
  const char* name() const override { return "ffmpeg"; }
  bool available(std::string& detail) override {
    detail = "not built: configure with -DSQ_FFMPEG_DIR=<LGPL ffmpeg shared dev package>";
    return false;
  }
  sq_result open(const std::string&, IVideoReader**) override { return SQ_E_UNSUPPORTED; }
};
}  // namespace

std::unique_ptr<VideoBackend> makeFfmpegBackend(const std::string&) { return std::make_unique<FfmpegStub>(); }

}  // namespace sq::video

#endif
