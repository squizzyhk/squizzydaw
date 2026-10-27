// Squizzy Studio SDK - header-only in-memory IByteStream (sq/interfaces/common.h).
//   auto s = sq::makeMemoryStream();                      // empty, writable, growable
//   auto r = sq::makeMemoryStream(bytes, size, false);    // copy of data, read-only
// Not thread-safe, allocates: never use on the audio thread.
#pragma once

#include <cstring>
#include <vector>

#include "sq/cpp/object.hpp"
#include "sq/cpp/ptr.hpp"
#include "sq/interfaces/common.h"

namespace sq {

class MemoryStream final : public Object<MemoryStream, IByteStream> {
 public:
  MemoryStream() = default;
  MemoryStream(const void* data, size_t size, bool writable)
      : data_(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size), writable_(writable) {}

  sq_result SQ_CALL read(void* buf, uint32_t size, uint32_t* got) override {
    if (!buf && size) return SQ_E_INVALIDARG;
    const size_t at = static_cast<size_t>(pos_);
    size_t avail = at < data_.size() ? data_.size() - at : 0;
    size_t n = size < avail ? size : avail;
    if (n) std::memcpy(buf, data_.data() + at, n);
    pos_ += static_cast<int64_t>(n);
    if (got) *got = static_cast<uint32_t>(n);
    return SQ_OK;
  }
  sq_result SQ_CALL write(const void* src, uint32_t size, uint32_t* written) override {
    if (written) *written = 0;
    if (!writable_) return SQ_E_ACCESS;
    if (!src && size) return SQ_E_INVALIDARG;
    size_t end = static_cast<size_t>(pos_) + size;
    if (end > data_.size()) data_.resize(end);
    if (size) std::memcpy(data_.data() + static_cast<size_t>(pos_), src, size);
    pos_ += size;
    if (written) *written = size;
    return SQ_OK;
  }
  sq_result SQ_CALL seek(int64_t offset, int32_t origin, int64_t* newPos) override {
    int64_t base = origin == SQ_SEEK_SET ? 0 : origin == SQ_SEEK_CUR ? pos_ : origin == SQ_SEEK_END ? static_cast<int64_t>(data_.size()) : -1;
    if (base < 0 || base + offset < 0) return SQ_E_INVALIDARG;
    pos_ = base + offset;
    if (newPos) *newPos = pos_;
    return SQ_OK;
  }
  sq_result SQ_CALL tell(int64_t* pos) override {
    if (!pos) return SQ_E_INVALIDARG;
    *pos = pos_;
    return SQ_OK;
  }
  sq_result SQ_CALL getSize(int64_t* size) override {
    if (!size) return SQ_E_INVALIDARG;
    *size = static_cast<int64_t>(data_.size());
    return SQ_OK;
  }
  sq_result SQ_CALL flush() override { return SQ_OK; }

  const std::vector<uint8_t>& bytes() const { return data_; }

 private:
  std::vector<uint8_t> data_;
  int64_t pos_ = 0;
  bool writable_ = true;
};

inline Ptr<IByteStream> makeMemoryStream() { return Ptr<IByteStream>::adopt(new MemoryStream()); }
inline Ptr<IByteStream> makeMemoryStream(const void* data, size_t size, bool writable) {
  return Ptr<IByteStream>::adopt(new MemoryStream(data, size, writable));
}

}  // namespace sq
