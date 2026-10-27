// video-engine: Media Foundation backend (Windows system component; nothing vendored).
// IMFSourceReader with MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING decodes the first video stream
// and converts it to RGB32 (BGRX); frames are copied top-down into the caller's buffer with
// alpha forced to 255. Seeks land on the previous key frame and decode forward to the target
// (SeekFilter). MF is started lazily on first use, under an SEH guard so editions without the
// Media Feature Pack (mfplat.dll is delay-loaded) report the backend unavailable instead of
// failing to load the module.
#include "backend.hpp"

#if defined(_WIN32)

// SDK headers first: <windows.h> (rpcndr.h) defines a `small` macro.
#include "frame_util.hpp"
#include "sq/sdk.hpp"

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>

namespace sq::video {
namespace {

template <class T>
struct Com {
  T* p = nullptr;
  Com() = default;
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  ~Com() { reset(); }
  void reset() {
    if (p) p->Release();
    p = nullptr;
  }
  T** put() {
    reset();
    return &p;
  }
  T* operator->() const { return p; }
  T* get() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  w.resize(static_cast<size_t>(n - 1));
  return w;
}

// ---- MF lifetime ---------------------------------------------------------------------------
struct MfRuntime {
  std::mutex m;
  bool tried = false;
  bool ok = false;
  CO_MTA_USAGE_COOKIE mta = nullptr;
  std::string detail;
};
MfRuntime& rt() {
  static MfRuntime r;
  return r;
}
std::atomic<int> g_liveReaders{0};

HRESULT guardedStartup() {
  __try {
    return MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return E_FAIL;
  }
}

bool ensureMf(std::string* detail) {
  MfRuntime& r = rt();
  std::lock_guard<std::mutex> lk(r.m);
  if (!r.tried) {
    r.tried = true;
    // Keep an MTA alive for the module's lifetime so readers work from any thread.
    if (FAILED(CoIncrementMTAUsage(&r.mta))) r.mta = nullptr;
    HRESULT hr = guardedStartup();
    r.ok = SUCCEEDED(hr);
    if (!r.ok) {
      char buf[96];
      std::snprintf(buf, sizeof buf, "MFStartup failed (0x%08lX): Media Foundation not installed?",
                    static_cast<unsigned long>(hr));
      r.detail = buf;
      if (r.mta) CoDecrementMTAUsage(r.mta);
      r.mta = nullptr;
    } else {
      r.detail = "Windows Media Foundation";
    }
  }
  if (detail) *detail = r.detail;
  return r.ok;
}

void shutdownMf() {
  MfRuntime& r = rt();
  std::lock_guard<std::mutex> lk(r.m);
  // Readers still alive: the loader will roll the unload back (module busy), so MF must stay up.
  if (g_liveReaders.load() > 0) return;
  if (r.ok) MFShutdown();
  if (r.mta) CoDecrementMTAUsage(r.mta);
  // The module may be initialised again in the same image (busy rollback): start from scratch.
  r.tried = false;
  r.ok = false;
  r.mta = nullptr;
  r.detail.clear();
}

const char* codecName(const GUID& sub) {
  if (sub == MFVideoFormat_H264) return "h264";
  if (sub == MFVideoFormat_HEVC) return "hevc";
  if (sub == MFVideoFormat_VP90) return "vp9";
  if (sub == MFVideoFormat_AV1) return "av1";
  if (sub == MFVideoFormat_MP4V) return "mpeg4";
  if (sub == MFVideoFormat_MPEG2) return "mpeg2";
  if (sub == MFVideoFormat_MJPG) return "mjpeg";
  if (sub == MFVideoFormat_WMV3) return "wmv3";
  if (sub == MFVideoFormat_WVC1) return "vc1";
  if (sub == MFVideoFormat_RGB32) return "rgb32";
  if (sub == MFVideoFormat_NV12) return "nv12";
  if (sub == MFVideoFormat_YUY2) return "yuy2";
  return "unknown";
}

constexpr DWORD kVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

class MfReader final : public Object<MfReader, IVideoReader> {
 public:
  MfReader() { g_liveReaders.fetch_add(1); }
  ~MfReader() override {
    pending_.reset();
    reader_.reset();
    g_liveReaders.fetch_sub(1);
  }
  sq_result open(const std::string& path) {
    Com<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(attrs.put(), 2))) return SQ_E_FAIL;
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    std::wstring w = widen(path);
    HRESULT hr = MFCreateSourceReaderFromURL(w.c_str(), attrs.get(), reader_.put());
    if (FAILED(hr)) {
      if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
        return SQ_E_NOTFOUND;
      return SQ_E_UNSUPPORTED;
    }
    reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (FAILED(reader_->SetStreamSelection(kVideo, TRUE))) return SQ_E_PARSE;
    Com<IMFMediaType> native;
    if (FAILED(reader_->GetNativeMediaType(kVideo, 0, native.put()))) return SQ_E_PARSE;
    GUID sub{};
    native->GetGUID(MF_MT_SUBTYPE, &sub);
    copyName(info_.codec, sizeof info_.codec, codecName(sub));
    UINT32 num = 0, den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(native.get(), MF_MT_FRAME_RATE, &num, &den)) && num && den) {
      info_.frame_rate_num = num;
      info_.frame_rate_den = den;
    }
    Com<IMFMediaType> out;
    if (FAILED(MFCreateMediaType(out.put()))) return SQ_E_FAIL;
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(reader_->SetCurrentMediaType(kVideo, nullptr, out.get()))) return SQ_E_UNSUPPORTED;
    sq_result r = readCurrentType();
    if (SQ_FAILED(r)) return r;

    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(reader_->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &var)) &&
        var.vt == VT_UI8)
      info_.duration_ns = hnsToNs(static_cast<int64_t>(var.uhVal.QuadPart));
    PropVariantClear(&var);
    Com<IMFMediaType> audio;
    info_.has_audio =
        SUCCEEDED(reader_->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, audio.put()))
            ? SQ_TRUE
            : SQ_FALSE;
    copyName(info_.backend, sizeof info_.backend, "mediafoundation");
    return SQ_OK;
  }

  sq_result SQ_CALL getInfo(sq_video_info* out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = info_;
    return SQ_OK;
  }

  sq_result SQ_CALL readFrame(void* buf, uint32_t bufSize, sq_video_frame* frame) override {
    if (!frame) return SQ_E_INVALIDARG;
    try {
      for (;;) {
        if (!pending_) {
          sq_result r = decodeNext();
          if (r != SQ_OK) return r;
        }
        if (filter_.skip(pendingTs_, pendingDur_)) {
          pending_.reset();
          continue;
        }
        break;
      }
      *frame = sq_video_frame{};
      frame->struct_size = sizeof *frame;
      frame->width = width_;
      frame->height = height_;
      frame->stride = width_ * 4;
      frame->pixel_format = SQ_PIXEL_BGRA8;
      frame->timestamp_ns = pendingTs_;
      frame->duration_ns = pendingDur_;
      frame->index = index_;
      uint64_t need = 0;
      if (!frameBytes(width_, height_, need)) return SQ_E_UNSUPPORTED;
      if (!buf || bufSize < need) return SQ_E_BUFFER_TOO_SMALL;
      sq_result r = copyOut(static_cast<uint8_t*>(buf));
      pending_.reset();
      if (SQ_FAILED(r)) return r;
      ++index_;
      return SQ_OK;
    } catch (...) {
      return SQ_E_FAIL;
    }
  }

  sq_result SQ_CALL seek(int64_t timeNs) override {
    if (timeNs < 0) timeNs = 0;
    pending_.reset();
    index_ = 0;
    filter_.disarm();
    if (info_.duration_ns > 0 && timeNs >= info_.duration_ns) {  // past the end: next read is EOS
      eos_ = true;
      return SQ_OK;
    }
    PROPVARIANT var;
    InitPropVariantFromInt64(nsToHns(timeNs), &var);
    HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);
    if (hr == MF_E_OUT_OF_RANGE) {
      eos_ = true;
      return SQ_OK;
    }
    if (FAILED(hr)) return SQ_E_IO;
    eos_ = false;
    filter_.arm(timeNs);
    return SQ_OK;
  }

 private:
  sq_result readCurrentType() {
    Com<IMFMediaType> cur;
    if (FAILED(reader_->GetCurrentMediaType(kVideo, cur.put()))) return SQ_E_FAIL;
    UINT32 w = 0, h = 0;
    if (FAILED(MFGetAttributeSize(cur.get(), MF_MT_FRAME_SIZE, &w, &h))) return SQ_E_PARSE;
    uint64_t bytes = 0;
    if (!frameBytes(w, h, bytes)) return SQ_E_UNSUPPORTED;
    width_ = w;
    height_ = h;
    UINT32 stride = 0;
    defaultStride_ = SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? static_cast<int32_t>(stride)
                                                                                 : static_cast<int32_t>(w * 4);
    UINT32 pn = 1, pd = 1;
    if (FAILED(MFGetAttributeRatio(cur.get(), MF_MT_PIXEL_ASPECT_RATIO, &pn, &pd)) || !pn || !pd) pn = pd = 1;
    info_.struct_size = sizeof info_;
    info_.width = w;
    info_.height = h;
    info_.pixel_aspect_num = pn;
    info_.pixel_aspect_den = pd;
    info_.pixel_format = SQ_PIXEL_BGRA8;
    return SQ_OK;
  }

  sq_result decodeNext() {
    if (eos_) return SQ_S_FALSE;
    for (;;) {
      DWORD actual = 0, flags = 0;
      LONGLONG ts = 0;
      Com<IMFSample> sample;
      HRESULT hr = reader_->ReadSample(kVideo, 0, &actual, &flags, &ts, sample.put());
      if (FAILED(hr)) return SQ_E_IO;
      if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
        sq_result r = readCurrentType();
        if (SQ_FAILED(r)) return r;
      }
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        eos_ = true;
        return SQ_S_FALSE;
      }
      if (flags & MF_SOURCE_READERF_ERROR) return SQ_E_IO;
      if (!sample) continue;  // stream tick / gap
      LONGLONG dur = 0;
      if (FAILED(sample->GetSampleDuration(&dur))) dur = 0;
      pendingTs_ = hnsToNs(ts);
      pendingDur_ = hnsToNs(dur);
      pending_.p = sample.p;
      sample.p = nullptr;
      return SQ_OK;
    }
  }

  sq_result copyOut(uint8_t* dst) {
    Com<IMFMediaBuffer> mb;
    if (FAILED(pending_->ConvertToContiguousBuffer(mb.put()))) return SQ_E_FAIL;
    const size_t rowBytes = size_t(width_) * 4;
    Com<IMF2DBuffer> b2;
    if (SUCCEEDED(mb->QueryInterface(IID_PPV_ARGS(b2.put())))) {
      BYTE* scan0 = nullptr;
      LONG pitch = 0;
      if (SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
        DWORD contiguous = 0;
        const uint64_t absPitch = pitch < 0 ? uint64_t(-int64_t(pitch)) : uint64_t(pitch);
        if (absPitch < rowBytes || FAILED(b2->GetContiguousLength(&contiguous)) || absPitch * height_ > contiguous) {
          b2->Unlock2D();  // buffer smaller than the frame size we were told: never over-read
          return SQ_E_FAIL;
        }
        copyRows(scan0, pitch, dst, rowBytes, height_, rowBytes, true);
        b2->Unlock2D();
        return SQ_OK;
      }
    }
    BYTE* data = nullptr;
    DWORD maxLen = 0, len = 0;
    if (FAILED(mb->Lock(&data, &maxLen, &len))) return SQ_E_FAIL;
    const int64_t absStride = defaultStride_ < 0 ? -int64_t(defaultStride_) : int64_t(defaultStride_);
    if (absStride < int64_t(rowBytes) || uint64_t(absStride) * height_ > len) {
      mb->Unlock();
      return SQ_E_FAIL;
    }
    // Negative default stride: bottom-up image, the first row in memory is the last on screen.
    const uint8_t* first = defaultStride_ < 0 ? data + absStride * (height_ - 1) : data;
    copyRows(first, defaultStride_, dst, rowBytes, height_, rowBytes, true);
    mb->Unlock();
    return SQ_OK;
  }

  Com<IMFSourceReader> reader_;
  sq_video_info info_{};
  uint32_t width_ = 0, height_ = 0;
  int32_t defaultStride_ = 0;
  Com<IMFSample> pending_;
  int64_t pendingTs_ = 0, pendingDur_ = 0;
  uint64_t index_ = 0;
  bool eos_ = false;
  SeekFilter filter_;
};

class MfBackend final : public VideoBackend {
 public:
  const char* name() const override { return "mediafoundation"; }
  bool available(std::string& detail) override { return ensureMf(&detail); }
  sq_result open(const std::string& path, IVideoReader** out) override {
    if (!ensureMf(nullptr)) return SQ_E_UNSUPPORTED;
    auto r = Ptr<MfReader>::adopt(new MfReader());
    sq_result res = r->open(path);
    if (SQ_FAILED(res)) return res;
    *out = r.detach();
    return SQ_OK;
  }
  void shutdown() override { shutdownMf(); }
};

}  // namespace

std::unique_ptr<VideoBackend> makeMediaFoundationBackend() { return std::make_unique<MfBackend>(); }

}  // namespace sq::video

#else

namespace sq::video {
std::unique_ptr<VideoBackend> makeMediaFoundationBackend() { return nullptr; }
}  // namespace sq::video

#endif
