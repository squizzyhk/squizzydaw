// Windows Imaging Component decode (PNG/JPEG/BMP/GIF/TIFF/...) and JPEG encode. Other platforms get
// stubs; decodeImage() then uses the portable PNG decoder.
#include "sq/ui/image.hpp"

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <cstdio>
#include <new>

namespace sq::ui {
namespace {

template <class T>
class Com {
 public:
  Com() = default;
  Com(const Com&) = delete;
  Com& operator=(const Com&) = delete;
  ~Com() { reset(); }
  void reset() {
    if (p_) p_->Release();
    p_ = nullptr;
  }
  T** put() {
    reset();
    return &p_;
  }
  T* operator->() const { return p_; }
  T* get() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }

 private:
  T* p_ = nullptr;
};

// COM for the calling thread for the duration of a call. Any apartment the thread already has is
// fine (WIC objects are free-threaded); we only balance what we initialised ourselves.
class ComScope {
 public:
  ComScope() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    owned_ = SUCCEEDED(hr);
    ok_ = owned_ || hr == RPC_E_CHANGED_MODE;
  }
  ~ComScope() {
    if (owned_) CoUninitialize();
  }
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;
  bool ok() const { return ok_; }

 private:
  bool owned_ = false, ok_ = false;
};

bool fail(std::string* e, const char* what, HRESULT hr) {
  if (e) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "WIC: %s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    *e = buf;
  }
  return false;
}

}  // namespace

const char* imageDecoderName() { return "wic"; }

ImagePtr decodeImageWic(const void* data, size_t size, float density, std::string* error) {
  if (!data || size == 0 || size > 0x7FFFFFFFu) {
    if (error) *error = "WIC: bad input size";
    return nullptr;
  }
  try {
    ComScope com;
    if (!com.ok()) {
      if (error) *error = "WIC: COM is unavailable on this thread";
      return nullptr;
    }
    std::vector<uint8_t> px;
    UINT w = 0, h = 0;
    {
      Com<IWICImagingFactory> factory;
      HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()));
      if (FAILED(hr)) { fail(error, "CoCreateInstance(ImagingFactory)", hr); return nullptr; }
      Com<IWICStream> stream;
      if (FAILED(hr = factory->CreateStream(stream.put()))) { fail(error, "CreateStream", hr); return nullptr; }
      if (FAILED(hr = stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)), static_cast<DWORD>(size))))
        { fail(error, "InitializeFromMemory", hr); return nullptr; }
      Com<IWICBitmapDecoder> decoder;
      if (FAILED(hr = factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())))
        { fail(error, "CreateDecoderFromStream (unknown format?)", hr); return nullptr; }
      Com<IWICBitmapFrameDecode> frame;
      if (FAILED(hr = decoder->GetFrame(0, frame.put()))) { fail(error, "GetFrame", hr); return nullptr; }
      if (FAILED(hr = frame->GetSize(&w, &h))) { fail(error, "GetSize", hr); return nullptr; }
      if (w == 0 || h == 0 || w > 16384 || h > 16384 || uint64_t{w} * h > kMaxImagePixels) {
        if (error) *error = "WIC: image dimensions out of range";
        return nullptr;
      }
      Com<IWICFormatConverter> conv;
      if (FAILED(hr = factory->CreateFormatConverter(conv.put()))) { fail(error, "CreateFormatConverter", hr); return nullptr; }
      if (FAILED(hr = conv->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom)))
        { fail(error, "FormatConverter::Initialize", hr); return nullptr; }
      const UINT stride = w * 4;
      px.resize(static_cast<size_t>(stride) * h);
      if (FAILED(hr = conv->CopyPixels(nullptr, stride, static_cast<UINT>(px.size()), px.data())))
        { fail(error, "CopyPixels", hr); return nullptr; }
    }
    return std::make_shared<Image>(static_cast<int>(w), static_cast<int>(h), std::move(px), density);
  } catch (const std::bad_alloc&) {
    if (error) *error = "WIC: out of memory";
    return nullptr;
  }
}

bool encodeJpegWic(const PixelBuffer& pb, float quality, std::string& out, std::string* error) {
  if (pb.width <= 0 || pb.height <= 0 || pb.rgba.size() != static_cast<size_t>(pb.width) * static_cast<size_t>(pb.height) * 4) {
    if (error) *error = "empty or inconsistent pixel buffer";
    return false;
  }
  try {
    ComScope com;
    if (!com.ok()) {
      if (error) *error = "WIC: COM is unavailable on this thread";
      return false;
    }
    const UINT w = static_cast<UINT>(pb.width), h = static_cast<UINT>(pb.height);
    std::vector<uint8_t> bgr(static_cast<size_t>(w) * 3 * h);
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {  // composite over black
      uint32_t a = pb.rgba[i * 4 + 3];
      bgr[i * 3 + 0] = static_cast<uint8_t>(pb.rgba[i * 4 + 2] * a / 255);
      bgr[i * 3 + 1] = static_cast<uint8_t>(pb.rgba[i * 4 + 1] * a / 255);
      bgr[i * 3 + 2] = static_cast<uint8_t>(pb.rgba[i * 4 + 0] * a / 255);
    }
    Com<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()));
    if (FAILED(hr)) return fail(error, "CoCreateInstance(ImagingFactory)", hr);
    Com<IStream> mem;
    if (FAILED(hr = CreateStreamOnHGlobal(nullptr, TRUE, mem.put()))) return fail(error, "CreateStreamOnHGlobal", hr);
    Com<IWICBitmapEncoder> enc;
    if (FAILED(hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, enc.put()))) return fail(error, "CreateEncoder", hr);
    if (FAILED(hr = enc->Initialize(mem.get(), WICBitmapEncoderNoCache))) return fail(error, "Encoder::Initialize", hr);
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> props;
    if (FAILED(hr = enc->CreateNewFrame(frame.put(), props.put()))) return fail(error, "CreateNewFrame", hr);
    PROPBAG2 opt{};
    opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_R4;
    v.fltVal = quality < 0 ? 0.0f : quality > 1 ? 1.0f : quality;
    if (FAILED(hr = props->Write(1, &opt, &v))) return fail(error, "ImageQuality", hr);
    if (FAILED(hr = frame->Initialize(props.get()))) return fail(error, "Frame::Initialize", hr);
    if (FAILED(hr = frame->SetSize(w, h))) return fail(error, "SetSize", hr);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(hr = frame->SetPixelFormat(&fmt))) return fail(error, "SetPixelFormat", hr);
    if (fmt != GUID_WICPixelFormat24bppBGR) {
      if (error) *error = "WIC: JPEG encoder refused 24bpp BGR";
      return false;
    }
    if (FAILED(hr = frame->WritePixels(h, w * 3, static_cast<UINT>(bgr.size()), bgr.data()))) return fail(error, "WritePixels", hr);
    if (FAILED(hr = frame->Commit())) return fail(error, "Frame::Commit", hr);
    if (FAILED(hr = enc->Commit())) return fail(error, "Encoder::Commit", hr);
    HGLOBAL g = nullptr;
    if (FAILED(hr = GetHGlobalFromStream(mem.get(), &g))) return fail(error, "GetHGlobalFromStream", hr);
    STATSTG stat{};
    if (FAILED(hr = mem->Stat(&stat, STATFLAG_NONAME))) return fail(error, "Stat", hr);
    const void* bytes = GlobalLock(g);
    if (!bytes) return fail(error, "GlobalLock", E_FAIL);
    out.assign(static_cast<const char*>(bytes), static_cast<size_t>(stat.cbSize.QuadPart));
    GlobalUnlock(g);
    return true;
  } catch (const std::bad_alloc&) {
    if (error) *error = "WIC: out of memory";
    return false;
  }
}

}  // namespace sq::ui

#else  // !_WIN32

namespace sq::ui {

const char* imageDecoderName() { return "portable"; }

ImagePtr decodeImageWic(const void*, size_t, float, std::string* error) {
  if (error) *error = "WIC is not available on this platform";
  return nullptr;
}

bool encodeJpegWic(const PixelBuffer&, float, std::string&, std::string* error) {
  if (error) *error = "WIC is not available on this platform";
  return false;
}

}  // namespace sq::ui

#endif
