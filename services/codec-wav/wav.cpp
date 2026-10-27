// codec-wav: Squizzy Studio WAV / RF64 / AIFF / AIFF-C codec service (category "Codec").
#include "wav_internal.hpp"

namespace sq::wav {
namespace {

using namespace sq::cx;

constexpr uint32_t kRW = SQ_CODEC_READ | SQ_CODEC_WRITE | SQ_CODEC_METADATA | SQ_CODEC_MARKERS;
constexpr FormatDesc kFormats[] = {
    {"wav", "audio/wav", "WAVE audio (PCM / IEEE float, RF64 above 4 GB)", kRW},
    {"wave", "audio/x-wav", "WAVE audio", kRW},
    {"bwf", "audio/x-bwf", "Broadcast WAVE", kRW},
    {"rf64", "audio/x-rf64", "RF64 / BW64 WAVE", kRW},
    {"aif", "audio/aiff", "AIFF audio", kRW},
    {"aiff", "audio/x-aiff", "AIFF audio", kRW},
    {"aifc", "audio/x-aifc", "AIFF-C audio (uncompressed / float)", kRW},
};
constexpr size_t kFormatCount = sizeof kFormats / sizeof kFormats[0];

class Service final : public Object<Service, ICodecService> {
 public:
  sq_result SQ_CALL getCodecInfo(sq_codec_info* out) override {
    if (!out || out->struct_size < sizeof(sq_codec_info)) return SQ_E_INVALIDARG;
    std::snprintf(out->name, sizeof out->name, "WAV/AIFF");
    out->flags = kRW;
    return SQ_OK;
  }
  uint32_t SQ_CALL getFormatCount() override { return static_cast<uint32_t>(kFormatCount); }
  sq_result SQ_CALL getFormatInfo(uint32_t index, sq_codec_format_info* out) override {
    if (!out || out->struct_size < sizeof(sq_codec_format_info)) return SQ_E_INVALIDARG;
    if (index >= kFormatCount) return SQ_E_NOTFOUND;
    fillFormat(kFormats[index], ~0u, out);
    return SQ_OK;
  }
  sq_bool SQ_CALL supports(const char* extension, const char* mime, uint32_t flags) override {
    return formatSupports(kFormats, kFormatCount, ~0u, extension, mime, flags) ? SQ_TRUE : SQ_FALSE;
  }
  int32_t SQ_CALL probe(const void* header, uint32_t size) override {
    const uint8_t* p = static_cast<const uint8_t*>(header);
    if (!p || size < 12) return 0;
    if ((!std::memcmp(p, "RIFF", 4) || !std::memcmp(p, "RF64", 4) || !std::memcmp(p, "BW64", 4)) && !std::memcmp(p + 8, "WAVE", 4))
      return 100;
    if (!std::memcmp(p, "FORM", 4) && (!std::memcmp(p + 8, "AIFF", 4) || !std::memcmp(p + 8, "AIFC", 4))) return 100;
    return 0;
  }
  sq_result SQ_CALL createReader(IAudioFileReader** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = new (std::nothrow) Reader();
    return *out ? SQ_OK : SQ_E_OUTOFMEMORY;
  }
  sq_result SQ_CALL createWriter(const char* extension, IAudioFileWriter** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    const char* e = extension ? (*extension == '.' ? extension + 1 : extension) : "wav";
    Kind k;
    if (ieq(e, "wav") || ieq(e, "wave") || ieq(e, "bwf") || ieq(e, "rf64")) k = Kind::Wav;
    else if (ieq(e, "aif") || ieq(e, "aiff")) k = Kind::Aiff;
    else if (ieq(e, "aifc")) k = Kind::Aifc;
    else return SQ_E_UNSUPPORTED;
    *out = new (std::nothrow) Writer(k);
    return *out ? SQ_OK : SQ_E_OUTOFMEMORY;
  }
};

constexpr sq::ClassEntry kClasses[] = {
    sq::classEntry<Service>(sq::codec::kWavCodecClassId, sq::codec::kCategory, "WAV/AIFF Codec"),
};

constexpr sq::ModuleDesc kModule{
    .fourcc = SQ_FOURCC('C', 'W', 'A', 'V'),
    .name = "codec-wav",
    .description = "WAV / RF64 / AIFF / AIFF-C codec with full chunk metadata",
    .classes = kClasses,
};

}  // namespace
}  // namespace sq::wav

SQ_MODULE_EXPORTS(sq::wav::kModule)
