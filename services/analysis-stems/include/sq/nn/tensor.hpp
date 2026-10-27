// Squizzy Studio - plain C++ tensor for the neural-inference seam (interfaces/dsp.h INNModel).
// Owns row-major storage of one element type and converts to/from the C POD sq_tensor.
// Header-only; allocation happens on construction/reshape only. Worker threads only.
#pragma once

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "sq/interfaces/dsp.h"

namespace sq::nn {

inline uint32_t dtypeSize(int32_t dtype) {
  switch (dtype) {
    case SQ_TENSOR_F32: return 4;
    case SQ_TENSOR_I64: return 8;
    case SQ_TENSOR_I32: return 4;
    case SQ_TENSOR_U8: return 1;
    default: return 0;
  }
}

template <class T> constexpr int32_t dtypeOf();
template <> constexpr int32_t dtypeOf<float>() { return SQ_TENSOR_F32; }
template <> constexpr int32_t dtypeOf<int64_t>() { return SQ_TENSOR_I64; }
template <> constexpr int32_t dtypeOf<int32_t>() { return SQ_TENSOR_I32; }
template <> constexpr int32_t dtypeOf<uint8_t>() { return SQ_TENSOR_U8; }

class Tensor {
 public:
  Tensor() = default;
  Tensor(int32_t dtype, std::vector<int64_t> shape) { reset(dtype, std::move(shape)); }
  Tensor(int32_t dtype, std::initializer_list<int64_t> shape) { reset(dtype, std::vector<int64_t>(shape)); }

  template <class T>
  static Tensor of(std::initializer_list<int64_t> shape) {
    return Tensor(dtypeOf<T>(), shape);
  }

  // Resizes (zero-filled). Dynamic (-1) or negative dims are treated as 0.
  void reset(int32_t dtype, std::vector<int64_t> shape) {
    dtype_ = dtype;
    shape_ = std::move(shape);
    if (shape_.size() > SQ_TENSOR_MAX_RANK) shape_.resize(SQ_TENSOR_MAX_RANK);
    for (auto& d : shape_)
      if (d < 0) d = 0;
    storage_.assign((byteSize() + 7) / 8, 0);
  }

  int32_t dtype() const { return dtype_; }
  const std::vector<int64_t>& shape() const { return shape_; }
  uint32_t rank() const { return static_cast<uint32_t>(shape_.size()); }
  int64_t dim(uint32_t i) const { return i < shape_.size() ? shape_[i] : 0; }
  uint64_t elementCount() const {
    if (shape_.empty()) return dtype_ ? 1 : 0;  // scalar
    uint64_t n = 1;
    for (int64_t d : shape_) n *= static_cast<uint64_t>(d);
    return n;
  }
  uint64_t byteSize() const { return elementCount() * dtypeSize(dtype_); }

  void* raw() { return storage_.data(); }
  const void* raw() const { return storage_.data(); }
  template <class T>
  T* data() {
    return dtypeOf<T>() == dtype_ ? reinterpret_cast<T*>(storage_.data()) : nullptr;
  }
  template <class T>
  const T* data() const {
    return dtypeOf<T>() == dtype_ ? reinterpret_cast<const T*>(storage_.data()) : nullptr;
  }
  // Row-major flat index of a full coordinate.
  uint64_t index(std::initializer_list<int64_t> coord) const {
    uint64_t idx = 0;
    size_t i = 0;
    for (int64_t c : coord) {
      idx = idx * static_cast<uint64_t>(i < shape_.size() ? shape_[i] : 1) + static_cast<uint64_t>(c);
      ++i;
    }
    return idx;
  }
  float& at(std::initializer_list<int64_t> coord) { return data<float>()[index(coord)]; }

  // Non-owning C view (valid while this tensor is alive and not reset).
  sq_tensor view() {
    sq_tensor t{};
    t.struct_size = sizeof t;
    t.dtype = dtype_;
    t.rank = rank();
    for (uint32_t i = 0; i < t.rank; ++i) t.shape[i] = shape_[i];
    t.data = storage_.data();
    t.byte_size = storage_.size() * 8;
    return t;
  }

  // Adopts the shape a callee wrote into `t` (after INNModel::run) — data is already in place
  // when t was produced by view() of this tensor.
  bool adoptShape(const sq_tensor& t) {
    std::vector<int64_t> s(t.shape, t.shape + (t.rank > SQ_TENSOR_MAX_RANK ? SQ_TENSOR_MAX_RANK : t.rank));
    uint64_t n = 1;
    for (int64_t d : s) n *= static_cast<uint64_t>(d < 0 ? 0 : d);
    if (n * dtypeSize(t.dtype) > storage_.size() * 8) return false;
    dtype_ = t.dtype;
    shape_ = std::move(s);
    return true;
  }

  std::string shapeString() const {
    std::string s = "[";
    for (size_t i = 0; i < shape_.size(); ++i) {
      if (i) s += ",";
      s += std::to_string(shape_[i]);
    }
    return s + "]";
  }

 private:
  int32_t dtype_ = 0;
  std::vector<int64_t> shape_;
  std::vector<uint64_t> storage_;  // 8-byte aligned
};

}  // namespace sq::nn
