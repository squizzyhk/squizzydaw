#include "vst3_common.hpp"

#include <algorithm>
#include <vector>

namespace sqvst3 {

using namespace Steinberg;

std::string toUtf8(const char16* s, size_t maxLen) {
  if (!s) return {};
  size_t n = 0;
  while (n < maxLen && s[n]) ++n;
  if (!n) return {};
  static_assert(sizeof(char16) == sizeof(uint16_t), "UTF-16 unit size");
  const auto* u = reinterpret_cast<const uint16_t*>(s);
  size_t need = 0;
  sq_utf16_to_utf8(u, n, nullptr, 0, &need);
  std::string out(need + 1, '\0');
  size_t got = 0;
  if (sq_utf16_to_utf8(u, n, out.data(), out.size(), &got) != SQ_OK) return {};
  out.resize(got);
  return out;
}

void toUtf16(const char* s, char16* out, size_t cap) {
  if (!cap) return;
  out[0] = 0;
  if (!s || !*s) return;
  size_t len = std::strlen(s), need = 0;
  sq_utf8_to_utf16(s, len, nullptr, 0, &need);
  std::vector<uint16_t> tmp(need + 1, 0);
  size_t got = 0;
  if (sq_utf8_to_utf16(s, len, tmp.data(), tmp.size(), &got) != SQ_OK) return;
  size_t n = std::min(got, cap - 1);
  for (size_t i = 0; i < n; ++i) out[i] = static_cast<char16>(tmp[i]);
  out[n] = 0;
}

// ---- StreamAdapter ------------------------------------------------------------------------------
tresult PLUGIN_API StreamAdapter::read(void* buffer, int32 numBytes, int32* numBytesRead) {
  if (numBytesRead) *numBytesRead = 0;
  if (numBytes < 0 || (!buffer && numBytes)) return kInvalidArgument;
  uint32_t done = 0;
  while (done < static_cast<uint32_t>(numBytes)) {
    uint32_t got = 0;
    if (SQ_FAILED(s_->read(static_cast<uint8_t*>(buffer) + done, static_cast<uint32_t>(numBytes) - done, &got))) return kResultFalse;
    if (got == 0) break;  // end of stream
    done += got;
  }
  if (numBytesRead) *numBytesRead = static_cast<int32>(done);
  // Short reads at the end of the stream are not an error (callers check numBytesRead).
  return (done == 0 && numBytes > 0) ? kResultFalse : kResultOk;
}

tresult PLUGIN_API StreamAdapter::write(void* buffer, int32 numBytes, int32* numBytesWritten) {
  if (numBytesWritten) *numBytesWritten = 0;
  if (numBytes < 0 || (!buffer && numBytes)) return kInvalidArgument;
  uint32_t w = 0;
  if (SQ_FAILED(s_->write(buffer, static_cast<uint32_t>(numBytes), &w))) return kResultFalse;
  if (numBytesWritten) *numBytesWritten = static_cast<int32>(w);
  return w == static_cast<uint32_t>(numBytes) ? kResultOk : kResultFalse;
}

tresult PLUGIN_API StreamAdapter::seek(int64 pos, int32 mode, int64* result) {
  int32_t origin = mode == IBStream::kIBSeekSet ? SQ_SEEK_SET : mode == IBStream::kIBSeekCur ? SQ_SEEK_CUR : mode == IBStream::kIBSeekEnd ? SQ_SEEK_END : -1;
  if (origin < 0) return kInvalidArgument;
  int64_t np = 0;
  if (SQ_FAILED(s_->seek(pos, origin, &np))) return kResultFalse;
  if (result) *result = np;
  return kResultOk;
}

tresult PLUGIN_API StreamAdapter::tell(int64* pos) {
  if (!pos) return kInvalidArgument;
  int64_t p = 0;
  if (SQ_FAILED(s_->tell(&p))) return kResultFalse;
  *pos = p;
  return kResultOk;
}

tresult PLUGIN_API StreamAdapter::getStreamSize(int64& size) {
  int64_t s = 0;
  if (SQ_FAILED(s_->getSize(&s))) return kResultFalse;
  size = s;
  return kResultOk;
}

tresult PLUGIN_API StreamAdapter::setStreamSize(int64) { return kNotImplemented; }

}  // namespace sqvst3
