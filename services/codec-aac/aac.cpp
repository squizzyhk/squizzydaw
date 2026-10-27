// codec-aac: Squizzy Studio AAC codec service backed by Windows Media Foundation.
//   read:  .m4a / .mp4 / .aac (ADTS) through IMFSourceReader -> float PCM. Tags from the MF
//          property handler ("tag:*"). Seeks are made sample accurate by seeking early (pre-roll)
//          and discarding decoded frames up to the target using the sample timestamps.
//   write: .m4a (MPEG-4) / .aac (ADTS) through IMFSinkWriter with the system AAC encoder:
//          44.1 / 48 kHz, 1, 2 or 6 channels, 96/128/160/192 kbps. Tags/markers are not written
//          (the MF sinks offer no metadata API): setMetadata/addMarker return SQ_E_UNSUPPORTED.
// Non-Windows builds compile a stub service that supports nothing.
#include "sqcodec/common.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <initguid.h>
#include <cguid.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objidl.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>

#include <mutex>
#endif

namespace sq::aac {
namespace {

using namespace sq::cx;

#if defined(_WIN32)

template <class T>
struct Com {  // minimal COM smart pointer
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

std::wstring widen(const char* s) {
  if (!s) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
  w.resize(static_cast<size_t>(n - 1));
  return w;
}
std::string narrow(const wchar_t* w) {
  if (!w) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  s.resize(static_cast<size_t>(n - 1));
  return s;
}

// ---- Media Foundation lifetime (module scope) -----------------------------------------------------
struct MfState {
  bool started = false;
  CO_MTA_USAGE_COOKIE mta = nullptr;
};
MfState& mf() {
  static MfState s;
  return s;
}
// MF DLLs are delay-loaded: a missing mfplat.dll surfaces as an SEH exception here.
HRESULT guardedStartup() {
  __try {
    return MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return E_FAIL;
  }
}
sq_result onInit(IHostContext*) {
  MfState& s = mf();
  // keep an MTA alive so worker threads without CoInitialize can use MF objects
  if (FAILED(CoIncrementMTAUsage(&s.mta))) s.mta = nullptr;
  s.started = SUCCEEDED(guardedStartup());
  if (!s.started) sq::logf(SQ_LOG_WARN, "Media Foundation unavailable: AAC codec disabled");
  return SQ_OK;
}
void onShutdown() {
  MfState& s = mf();
  if (s.started) MFShutdown();
  s.started = false;
  if (s.mta) CoDecrementMTAUsage(s.mta);
  s.mta = nullptr;
}
bool mfReady() { return mf().started; }

// ---- IStream over sq::IByteStream ------------------------------------------------------------------
class StreamAdapter final : public ::IStream {
 public:
  explicit StreamAdapter(sq::IByteStream* s, bool writable) : s_(s), writable_(writable) { s_->addRef(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == __uuidof(::IUnknown) || riid == __uuidof(::IStream) || riid == __uuidof(::ISequentialStream)) {
      *out = static_cast<::IStream*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
  ULONG STDMETHODCALLTYPE Release() override {
    LONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return static_cast<ULONG>(r);
  }
  HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* read) override {
    std::lock_guard<std::mutex> g(m_);
    uint32_t got = 0;
    if (SQ_FAILED(s_->read(pv, cb, &got))) return STG_E_READFAULT;
    if (read) *read = got;
    return got == cb ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Write(const void* pv, ULONG cb, ULONG* written) override {
    std::lock_guard<std::mutex> g(m_);
    if (!writable_) return STG_E_ACCESSDENIED;
    uint32_t w = 0;
    if (SQ_FAILED(s_->write(pv, cb, &w))) return STG_E_WRITEFAULT;
    if (written) *written = w;
    return w == cb ? S_OK : STG_E_MEDIUMFULL;
  }
  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPos) override {
    std::lock_guard<std::mutex> g(m_);
    int64_t np = 0;
    int32_t o = origin == STREAM_SEEK_SET ? SQ_SEEK_SET : origin == STREAM_SEEK_CUR ? SQ_SEEK_CUR : SQ_SEEK_END;
    if (SQ_FAILED(s_->seek(move.QuadPart, o, &np))) return STG_E_SEEKERROR;
    if (newPos) newPos->QuadPart = static_cast<ULONGLONG>(np);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return writable_ ? S_OK : STG_E_ACCESSDENIED; }
  HRESULT STDMETHODCALLTYPE CopyTo(::IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE Commit(DWORD) override {
    std::lock_guard<std::mutex> g(m_);
    return SQ_SUCCEEDED(s_->flush()) ? S_OK : STG_E_WRITEFAULT;
  }
  HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
  HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
  HRESULT STDMETHODCALLTYPE Stat(STATSTG* st, DWORD) override {
    if (!st) return E_POINTER;
    std::lock_guard<std::mutex> g(m_);
    std::memset(st, 0, sizeof *st);
    st->type = STGTY_STREAM;
    int64_t n = 0;
    if (SQ_SUCCEEDED(s_->getSize(&n))) st->cbSize.QuadPart = static_cast<ULONGLONG>(n);
    st->grfMode = writable_ ? STGM_READWRITE : STGM_READ;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Clone(::IStream**) override { return E_NOTIMPL; }

 private:
  ~StreamAdapter() { s_->release(); }
  sq::IByteStream* s_;
  bool writable_;
  LONG refs_ = 1;
  std::mutex m_;
};

HRESULT byteStreamOn(sq::IByteStream* s, bool writable, const wchar_t* contentType, IMFByteStream** out) {
  auto* a = new (std::nothrow) StreamAdapter(s, writable);
  if (!a) return E_OUTOFMEMORY;
  HRESULT hr = MFCreateMFByteStreamOnStream(a, out);
  a->Release();
  if (SUCCEEDED(hr) && contentType) {
    Com<IMFAttributes> at;
    if (SUCCEEDED((*out)->QueryInterface(IID_PPV_ARGS(at.put())))) at->SetString(MF_BYTESTREAM_CONTENT_TYPE, contentType);
  }
  return hr;
}

bool isAdts(const uint8_t* p, size_t n) { return n >= 2 && p[0] == 0xFF && (p[1] & 0xF6) == 0xF0; }
bool isMp4(const uint8_t* p, size_t n) { return n >= 8 && !std::memcmp(p + 4, "ftyp", 4); }

// ---- reader --------------------------------------------------------------------------------------
constexpr int64_t kPrerollFrames = 4096;

class Reader final : public ReaderBase<Reader> {
 public:
  sq_result SQ_CALL open(const char* path) override {
    if (reader_) return SQ_E_EXISTS;
    if (!mfReady()) return SQ_E_UNSUPPORTED;
    if (!path || !sq_fs_is_file(path)) return SQ_E_NOTFOUND;
    {
      Ptr<IByteStream> f;
      uint8_t head[16] = {};
      if (SQ_SUCCEEDED(FileStream::open(path, false, f))) {
        uint32_t got = 0;
        if (SQ_SUCCEEDED(f->read(head, sizeof head, &got))) adts_ = !isMp4(head, got) && isAdts(head, got);
      }
    }
    std::wstring w = widen(path);
    if (FAILED(MFCreateSourceReaderFromURL(w.c_str(), nullptr, reader_.put()))) return SQ_E_PARSE;
    return setup();
  }
  sq_result SQ_CALL openStream(IByteStream* stream) override {
    if (reader_) return SQ_E_EXISTS;
    if (!stream) return SQ_E_INVALIDARG;
    if (!mfReady()) return SQ_E_UNSUPPORTED;
    uint8_t head[16] = {};
    uint32_t got = 0;
    if (SQ_FAILED(stream->seek(0, SQ_SEEK_SET, nullptr)) || SQ_FAILED(stream->read(head, sizeof head, &got))) return SQ_E_IO;
    stream->seek(0, SQ_SEEK_SET, nullptr);
    const wchar_t* type = isMp4(head, got) ? L"audio/mp4" : isAdts(head, got) ? L"audio/aac" : nullptr;
    if (!type) return SQ_E_PARSE;
    adts_ = !isMp4(head, got);
    Com<IMFByteStream> bs;
    if (FAILED(byteStreamOn(stream, false, type, bs.put()))) return SQ_E_FAIL;
    if (FAILED(MFCreateSourceReaderFromByteStream(bs.get(), nullptr, reader_.put()))) return SQ_E_PARSE;
    return setup();
  }
  sq_result SQ_CALL getFormat(sq_audio_file_format* out) override {
    if (!out || out->struct_size < sizeof(sq_audio_file_format)) return SQ_E_INVALIDARG;
    if (!reader_) return SQ_E_CLOSED;
    out->sample_rate = rate_;
    out->channels = channels_;
    out->sample_format = SQ_SAMPLE_F32;
    out->bits_per_sample = 32;
    out->frame_count = frames_;
    out->bitrate_kbps = bitrate_;
    out->flags = SQ_AUDIOFILE_SEEKABLE | SQ_AUDIOFILE_SAMPLE_EXACT;
    std::snprintf(out->container, sizeof out->container, "%s", container_);
    return SQ_OK;
  }
  sq_result SQ_CALL read(sq_audio_block* dst, uint32_t* framesRead) override {
    if (framesRead) *framesRead = 0;
    if (!reader_) return SQ_E_CLOSED;
    if (!blockOk(dst, channels_)) return SQ_E_INVALIDARG;
    uint32_t done = 0;
    while (done < dst->frames) {
      if (frames_ >= 0 && pos_ >= frames_) break;
      if (pendPos_ >= pendFrames()) {
        if (!fetch()) break;
        continue;
      }
      uint32_t n = static_cast<uint32_t>(std::min<size_t>(dst->frames - done, pendFrames() - pendPos_));
      if (frames_ >= 0) n = static_cast<uint32_t>(std::min<int64_t>(n, frames_ - pos_));
      for (uint32_t c = 0; c < channels_; ++c) {
        float* o = dst->channels[c] + done;
        const float* in = pend_.data() + pendPos_ * channels_ + c;
        for (uint32_t i = 0; i < n; ++i) o[i] = in[static_cast<size_t>(i) * channels_];
      }
      pendPos_ += n;
      done += n;
      pos_ += n;
    }
    if (framesRead) *framesRead = done;
    return SQ_OK;
  }
  sq_result SQ_CALL seek(sq_sample_time frame) override {
    if (!reader_) return SQ_E_CLOSED;
    if (frame < 0 || (frames_ >= 0 && frame > frames_)) return SQ_E_INVALIDARG;
    // early seek, then decode-and-discard to the exact frame using sample timestamps
    for (int attempt = 0; attempt < 2; ++attempt) {
      int64_t from = attempt == 0 ? std::max<int64_t>(0, frame - kPrerollFrames) : 0;
      if (!seekTo(from)) return SQ_E_IO;
      bool landed = false;
      for (;;) {
        if (!fetch()) {  // end of stream before the target
          pos_ = frame;
          pendPos_ = pendFrames();
          return SQ_OK;
        }
        if (pendStart_ > frame) break;  // landed after the target: retry from the start
        if (pendStart_ + static_cast<int64_t>(pendFrames()) > frame) {
          pendPos_ = static_cast<size_t>(frame - pendStart_);
          landed = true;
          break;
        }
      }
      if (landed) {
        pos_ = frame;
        return SQ_OK;
      }
    }
    return SQ_E_IO;
  }
  sq_result SQ_CALL tell(sq_sample_time* frame) override {
    if (!frame) return SQ_E_INVALIDARG;
    if (!reader_) return SQ_E_CLOSED;
    *frame = pos_;
    return SQ_OK;
  }

 private:
  size_t pendFrames() const { return channels_ ? pend_.size() / channels_ : 0; }

  sq_result setup() {
    const DWORD audio = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    reader_->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (FAILED(reader_->SetStreamSelection(audio, TRUE))) { reader_.reset(); return SQ_E_PARSE; }
    Com<IMFMediaType> native;
    if (SUCCEEDED(reader_->GetNativeMediaType(audio, 0, native.put()))) {
      GUID sub{};
      native->GetGUID(MF_MT_SUBTYPE, &sub);
      UINT32 avg = 0;
      if (SUCCEEDED(native->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &avg))) bitrate_ = avg * 8 / 1000;
      if (sub == MFAudioFormat_AAC) std::snprintf(container_, sizeof container_, "%s", adts_ ? "aac" : "m4a");
    }
    Com<IMFMediaType> want;
    if (FAILED(MFCreateMediaType(want.put()))) { reader_.reset(); return SQ_E_OUTOFMEMORY; }
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (FAILED(reader_->SetCurrentMediaType(audio, nullptr, want.get()))) { reader_.reset(); return SQ_E_UNSUPPORTED; }
    Com<IMFMediaType> cur;
    if (FAILED(reader_->GetCurrentMediaType(audio, cur.put()))) { reader_.reset(); return SQ_E_PARSE; }
    UINT32 ch = 0, sr = 0;
    cur->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
    cur->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
    if (ch == 0 || ch > 64 || sr == 0) { reader_.reset(); return SQ_E_PARSE; }
    channels_ = ch;
    rate_ = sr;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(reader_->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &v)) &&
        v.vt == VT_UI8)
      frames_ = static_cast<sq_sample_time>(std::llround(static_cast<double>(v.uhVal.QuadPart) * rate_ / 1e7));
    PropVariantClear(&v);
    readTags();
    if (!fetch()) { reader_.reset(); return SQ_E_PARSE; }  // decode the first block now: fails early on garbage
    firstStart_ = pendStart_;
    pendStart_ = 0;
    offset_ = firstStart_;
    pos_ = 0;
    return SQ_OK;
  }

  void readTags() {
    Com<IPropertyStore> ps;
    if (FAILED(reader_->GetServiceForStream(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PROPERTY_HANDLER_SERVICE,
                                            IID_PPV_ARGS(ps.put()))))
      return;
    struct K { const PROPERTYKEY* key; const char* tag; };
    const K keys[] = {{&PKEY_Title, "title"},           {&PKEY_Music_Artist, "artist"},   {&PKEY_Music_AlbumTitle, "album"},
                      {&PKEY_Music_Genre, "genre"},     {&PKEY_Media_Year, "date"},       {&PKEY_Music_TrackNumber, "track"},
                      {&PKEY_Copyright, "copyright"},   {&PKEY_Comment, "comment"},       {&PKEY_Media_EncodingSettings, "encoder"},
                      {&PKEY_Music_BeatsPerMinute, "bpm"}, {&PKEY_Music_InitialKey, "key"}};
    for (const auto& k : keys) {
      PROPVARIANT v;
      PropVariantInit(&v);
      if (SUCCEEDED(ps->GetValue(*k.key, &v)) && v.vt != VT_EMPTY) {
        PWSTR s = nullptr;
        if (SUCCEEDED(PropVariantToStringAlloc(v, &s)) && s) {
          std::string t = narrow(s);
          if (!t.empty() && t != "0") meta_.setText(std::string("tag:") + k.tag, t);
          CoTaskMemFree(s);
        }
      }
      PropVariantClear(&v);
    }
  }

  bool seekTo(int64_t frame) {
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_I8;
    v.hVal.QuadPart = std::llround((static_cast<double>(frame) + static_cast<double>(offset_)) * 1e7 / rate_);
    if (v.hVal.QuadPart < 0) v.hVal.QuadPart = 0;
    HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, v);
    PropVariantClear(&v);
    pend_.clear();
    pendPos_ = 0;
    eof_ = false;
    return SUCCEEDED(hr);
  }

  // Decodes the next MF sample into pend_; pendStart_ = its frame index (timestamp based).
  bool fetch() {
    pend_.clear();
    pendPos_ = 0;
    if (eof_) return false;
    for (int guard = 0; guard < 1000; ++guard) {
      DWORD flags = 0;
      LONGLONG ts = 0;
      Com<IMFSample> s;
      HRESULT hr = reader_->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, &ts, s.put());
      if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) { eof_ = true; return false; }
      if (!s) {
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { eof_ = true; return false; }
        continue;
      }
      Com<IMFMediaBuffer> b;
      if (FAILED(s->ConvertToContiguousBuffer(b.put()))) { eof_ = true; return false; }
      BYTE* data = nullptr;
      DWORD len = 0;
      if (FAILED(b->Lock(&data, nullptr, &len))) { eof_ = true; return false; }
      const size_t floats = len / sizeof(float);
      pend_.assign(reinterpret_cast<const float*>(data), reinterpret_cast<const float*>(data) + (floats - floats % channels_));
      b->Unlock();
      pendStart_ = std::llround(static_cast<double>(ts) * rate_ / 1e7) - offset_;
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) eof_ = true;
      if (!pend_.empty()) return true;
      if (eof_) return false;
    }
    eof_ = true;
    return false;
  }

  Com<IMFSourceReader> reader_;
  uint32_t channels_ = 0, bitrate_ = 0;
  double rate_ = 0;
  sq_sample_time frames_ = -1, pos_ = 0;
  int64_t firstStart_ = 0, offset_ = 0, pendStart_ = 0;
  std::vector<float> pend_;
  size_t pendPos_ = 0;
  bool eof_ = false, adts_ = false;
  char container_[16] = "mp4";
};

// ---- writer --------------------------------------------------------------------------------------
class Writer final : public Object<Writer, IAudioFileWriter> {
 public:
  explicit Writer(bool adts) : adts_(adts) {}
  ~Writer() override {
    if (!finalized_) {
      writer_.reset();
      if (!path_.empty()) DeleteFileW(path_.c_str());
    }
  }
  sq_result SQ_CALL open(const char* path, const sq_audio_file_format* format, IAttributes* options) override {
    if (writer_) return SQ_E_EXISTS;
    if (!path || !*path) return SQ_E_INVALIDARG;
    sq_result r = check(format, options);
    if (SQ_FAILED(r)) return r;
    std::wstring w = widen(path);
    Com<IMFAttributes> at;
    MFCreateAttributes(at.put(), 2);
    at->SetGUID(MF_TRANSCODE_CONTAINERTYPE, adts_ ? MFTranscodeContainerType_ADTS : MFTranscodeContainerType_MPEG4);
    if (FAILED(MFCreateSinkWriterFromURL(w.c_str(), nullptr, at.get(), writer_.put()))) return SQ_E_IO;
    path_ = w;
    return configure();
  }
  sq_result SQ_CALL openStream(IByteStream* stream, const sq_audio_file_format* format, IAttributes* options) override {
    if (writer_) return SQ_E_EXISTS;
    if (!stream) return SQ_E_INVALIDARG;
    sq_result r = check(format, options);
    if (SQ_FAILED(r)) return r;
    Com<IMFByteStream> bs;
    if (FAILED(byteStreamOn(stream, true, nullptr, bs.put()))) return SQ_E_FAIL;
    Com<IMFAttributes> at;
    MFCreateAttributes(at.put(), 2);
    at->SetGUID(MF_TRANSCODE_CONTAINERTYPE, adts_ ? MFTranscodeContainerType_ADTS : MFTranscodeContainerType_MPEG4);
    if (FAILED(MFCreateSinkWriterFromURL(adts_ ? L".aac" : L".m4a", bs.get(), at.get(), writer_.put()))) return SQ_E_IO;
    return configure();
  }
  sq_result SQ_CALL setMetadata(const char*, const void*, uint32_t) override { return SQ_E_UNSUPPORTED; }
  sq_result SQ_CALL addMarker(const sq_audio_file_marker*) override { return SQ_E_UNSUPPORTED; }
  sq_result SQ_CALL write(const sq_audio_block* src) override {
    if (!writer_ || finalized_ || failed_) return SQ_E_CLOSED;
    if (!blockOk(src, channels_)) return SQ_E_INVALIDARG;
    for (uint32_t at = 0; at < src->frames;) {
      const uint32_t n = std::min<uint32_t>(src->frames - at, 4096);
      Com<IMFMediaBuffer> buf;
      const DWORD bytes = n * channels_ * 2;
      if (FAILED(MFCreateMemoryBuffer(bytes, buf.put()))) { failed_ = true; return SQ_E_OUTOFMEMORY; }
      BYTE* p = nullptr;
      if (FAILED(buf->Lock(&p, nullptr, nullptr))) { failed_ = true; return SQ_E_FAIL; }
      int16_t* o = reinterpret_cast<int16_t*>(p);
      for (uint32_t i = 0; i < n; ++i)
        for (uint32_t c = 0; c < channels_; ++c) o[i * channels_ + c] = static_cast<int16_t>(floatToInt(dither_.apply(src->channels[c][at + i], 16), 16));
      buf->Unlock();
      buf->SetCurrentLength(bytes);
      Com<IMFSample> s;
      MFCreateSample(s.put());
      s->AddBuffer(buf.get());
      s->SetSampleTime(static_cast<LONGLONG>(std::llround(static_cast<double>(written_) * 1e7 / rate_)));
      s->SetSampleDuration(static_cast<LONGLONG>(std::llround(static_cast<double>(n) * 1e7 / rate_)));
      if (FAILED(writer_->WriteSample(stream_, s.get()))) { failed_ = true; return SQ_E_IO; }
      written_ += n;
      at += n;
    }
    return SQ_OK;
  }
  sq_result SQ_CALL finalize() override {
    if (finalized_) return SQ_S_FALSE;
    if (!writer_) return SQ_E_CLOSED;
    if (failed_) return SQ_E_IO;
    if (FAILED(writer_->Finalize())) { failed_ = true; return SQ_E_IO; }
    finalized_ = true;
    writer_.reset();
    return SQ_OK;
  }

 private:
  sq_result check(const sq_audio_file_format* f, IAttributes* options) {
    if (!mfReady()) return SQ_E_UNSUPPORTED;
    if (!f || f->struct_size < sizeof(sq_audio_file_format)) return SQ_E_INVALIDARG;
    if (f->sample_rate != 44100.0 && f->sample_rate != 48000.0) return SQ_E_UNSUPPORTED;  // system encoder limits
    if (f->channels != 1 && f->channels != 2 && f->channels != 6) return SQ_E_UNSUPPORTED;
    channels_ = f->channels;
    rate_ = f->sample_rate;
    int64_t kbps = f->bitrate_kbps ? f->bitrate_kbps : 192;
    if (options) {
      if (options->getType("quality") != SQ_VALUE_NONE) kbps = 96 + std::llround(std::clamp(options->getDouble("quality", 1.0), 0.0, 1.0) * 3) * 32;
      kbps = options->getInt("bitrate_kbps", kbps);
      dither_ = Dither(options->getBool("dither", SQ_FALSE) != SQ_FALSE);
    }
    static const int kRates[] = {96, 128, 160, 192};
    int best = 192;
    for (int r : kRates)
      if (std::llabs(r - kbps) < std::llabs(best - kbps)) best = r;
    kbps_ = static_cast<uint32_t>(best);
    return SQ_OK;
  }
  sq_result configure() {
    Com<IMFMediaType> out, in;
    MFCreateMediaType(out.put());
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(rate_));
    out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels_);
    out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kbps_ * 1000 / 8);
    if (FAILED(writer_->AddStream(out.get(), &stream_))) { writer_.reset(); return SQ_E_UNSUPPORTED; }
    MFCreateMediaType(in.put());
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, static_cast<UINT32>(rate_));
    in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels_);
    in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels_ * 2);
    in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(rate_) * channels_ * 2);
    if (FAILED(writer_->SetInputMediaType(stream_, in.get(), nullptr))) { writer_.reset(); return SQ_E_UNSUPPORTED; }
    if (FAILED(writer_->BeginWriting())) { writer_.reset(); return SQ_E_IO; }
    return SQ_OK;
  }

  bool adts_;
  Com<IMFSinkWriter> writer_;
  DWORD stream_ = 0;
  std::wstring path_;
  uint32_t channels_ = 0, kbps_ = 192;
  double rate_ = 0;
  uint64_t written_ = 0;
  bool finalized_ = false, failed_ = false;
  Dither dither_;
};

#endif  // _WIN32

// ---- service ---------------------------------------------------------------------------------------
constexpr uint32_t kRW = SQ_CODEC_READ | SQ_CODEC_WRITE | SQ_CODEC_LOSSY | SQ_CODEC_METADATA;
constexpr FormatDesc kFormats[] = {
    {"m4a", "audio/mp4", "MPEG-4 AAC audio (Media Foundation)", kRW},
    {"aac", "audio/aac", "AAC ADTS stream (Media Foundation)", kRW},
    {"mp4", "video/mp4", "MPEG-4 file, audio track only (decode)", SQ_CODEC_READ | SQ_CODEC_LOSSY | SQ_CODEC_METADATA},
};
constexpr size_t kFormatCount = sizeof kFormats / sizeof kFormats[0];

bool available() {
#if defined(_WIN32)
  return mfReady();
#else
  return false;
#endif
}

class Service final : public Object<Service, ICodecService> {
 public:
  sq_result SQ_CALL getCodecInfo(sq_codec_info* out) override {
    if (!out || out->struct_size < sizeof(sq_codec_info)) return SQ_E_INVALIDARG;
    std::snprintf(out->name, sizeof out->name, "AAC (Media Foundation)");
    out->flags = available() ? kRW : 0u;
    return SQ_OK;
  }
  uint32_t SQ_CALL getFormatCount() override { return available() ? static_cast<uint32_t>(kFormatCount) : 0u; }
  sq_result SQ_CALL getFormatInfo(uint32_t index, sq_codec_format_info* out) override {
    if (!out || out->struct_size < sizeof(sq_codec_format_info)) return SQ_E_INVALIDARG;
    if (index >= getFormatCount()) return SQ_E_NOTFOUND;
    fillFormat(kFormats[index], ~0u, out);
    return SQ_OK;
  }
  sq_bool SQ_CALL supports(const char* extension, const char* mime, uint32_t flags) override {
    if (!available()) return SQ_FALSE;
    return formatSupports(kFormats, kFormatCount, ~0u, extension, mime, flags) ? SQ_TRUE : SQ_FALSE;
  }
  int32_t SQ_CALL probe(const void* header, uint32_t size) override {
    const uint8_t* p = static_cast<const uint8_t*>(header);
    if (!available() || !p || size < 8) return 0;
    if (!std::memcmp(p + 4, "ftyp", 4)) {
      if (size >= 12 && (!std::memcmp(p + 8, "M4A ", 4) || !std::memcmp(p + 8, "M4B ", 4))) return 95;
      return 60;  // generic MP4: may be video or another codec
    }
    if (p[0] == 0xFF && (p[1] & 0xF6) == 0xF0) return 60;  // ADTS sync, layer 0
    return 0;
  }
  sq_result SQ_CALL createReader(IAudioFileReader** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
#if defined(_WIN32)
    *out = new (std::nothrow) Reader();
    return *out ? SQ_OK : SQ_E_OUTOFMEMORY;
#else
    return SQ_E_UNSUPPORTED;
#endif
  }
  sq_result SQ_CALL createWriter(const char* extension, IAudioFileWriter** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (!available()) return SQ_E_UNSUPPORTED;
#if defined(_WIN32)
    const char* e = extension ? (*extension == '.' ? extension + 1 : extension) : "m4a";
    bool adts;
    if (ieq(e, "m4a") || ieq(e, "mp4")) adts = false;
    else if (ieq(e, "aac")) adts = true;
    else return SQ_E_UNSUPPORTED;
    *out = new (std::nothrow) Writer(adts);
    return *out ? SQ_OK : SQ_E_OUTOFMEMORY;
#else
    (void)extension;
    return SQ_E_UNSUPPORTED;
#endif
  }
};

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<Service>(sq::codec::kAacCodecClassId, sq::codec::kCategory, "AAC Codec"),
};
constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('C', 'A', 'A', 'C'),
    .name = "codec-aac",
    .description = "AAC / M4A codec via Windows Media Foundation",
    .classes = kClasses,
#if defined(_WIN32)
    .onInit = &onInit,
    .onShutdown = &onShutdown,
#endif
};

}  // namespace
}  // namespace sq::aac

SQ_MODULE_EXPORTS(sq::aac::kModule)
