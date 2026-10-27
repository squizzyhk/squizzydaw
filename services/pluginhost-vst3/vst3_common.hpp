// pluginhost-vst3: shared helpers (FUnknown object base, RT ring, UTF-16 strings, IBStream adapter).
#pragma once

#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/vsttypes.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "sq/interfaces/common.h"
#include "sq/platform.hpp"
#include "sq/sdk.hpp"

namespace sqvst3 {

using Steinberg::FUnknown;
using Steinberg::IPtr;
using Steinberg::tresult;
using Steinberg::TUID;

inline bool iidIs(const TUID a, const Steinberg::FUID& b) { return Steinberg::FUnknownPrivate::iidEqual(a, b.toTUID()); }

// Refcounted host-side FUnknown object. Starts at refcount 1 (adopt with Steinberg::owned()).
// Counted in this module's live-object counter, so the adapter DLL cannot be unloaded while a
// plugin still holds one of our host objects.
template <class Derived, class... Ifaces>
class FObject : public Ifaces... {
 public:
  FObject() noexcept { sq::detail::liveObjects().fetch_add(1, std::memory_order_relaxed); }
  FObject(const FObject&) = delete;
  FObject& operator=(const FObject&) = delete;

  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    if (!obj) return Steinberg::kInvalidArgument;
    *obj = nullptr;
    void* p = nullptr;
    Derived* self = static_cast<Derived*>(this);
    ((p == nullptr && iidIs(iid, Ifaces::iid) ? (p = static_cast<Ifaces*>(self), true) : false), ...);
    if (!p && iidIs(iid, FUnknown::iid)) p = static_cast<FUnknown*>(static_cast<FirstIface*>(self));
    if (!p) p = self->queryExtra(iid);
    if (!p) return Steinberg::kNoInterface;
    addRef();
    *obj = p;
    return Steinberg::kResultOk;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
  Steinberg::uint32 PLUGIN_API release() override {
    uint32_t r = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (r == 0) delete static_cast<Derived*>(this);
    return r;
  }
  // Extra interfaces answered by QI (e.g. bases of an interface). Override in Derived.
  void* queryExtra(const TUID) { return nullptr; }

 protected:
  virtual ~FObject() { sq::detail::liveObjects().fetch_sub(1, std::memory_order_relaxed); }

 private:
  template <class F, class...>
  struct First {
    using type = F;
  };
  using FirstIface = typename First<Ifaces...>::type;
  std::atomic<uint32_t> refs_{1};
};

// Owner-managed FUnknown object (preallocated process structures): refcounting is a no-op.
template <class Derived, class Iface>
class FStatic : public Iface {
 public:
  tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
    if (!obj) return Steinberg::kInvalidArgument;
    if (iidIs(iid, Iface::iid) || iidIs(iid, FUnknown::iid)) {
      *obj = static_cast<Iface*>(this);
      return Steinberg::kResultOk;
    }
    *obj = nullptr;
    return Steinberg::kNoInterface;
  }
  Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
  Steinberg::uint32 PLUGIN_API release() override { return 1; }
};

// QI helper: returns an owned IPtr<I> or null.
template <class I>
IPtr<I> queryAs(FUnknown* u) {
  I* p = nullptr;
  if (u && u->queryInterface(I::iid, reinterpret_cast<void**>(&p)) == Steinberg::kResultOk && p) return Steinberg::owned(p);
  return nullptr;
}

// Fixed-capacity single-producer / single-consumer ring (no allocation after construction).
template <class T>
class SpscRing {
 public:
  explicit SpscRing(uint32_t capacityPow2) : mask_(capacityPow2 - 1), buf_(new T[capacityPow2]) {}
  bool push(const T& v) noexcept {
    uint32_t w = w_.load(std::memory_order_relaxed);
    if (w - r_.load(std::memory_order_acquire) > mask_) return false;
    buf_[w & mask_] = v;
    w_.store(w + 1, std::memory_order_release);
    return true;
  }
  bool pop(T& out) noexcept {
    uint32_t r = r_.load(std::memory_order_relaxed);
    if (r == w_.load(std::memory_order_acquire)) return false;
    out = buf_[r & mask_];
    r_.store(r + 1, std::memory_order_release);
    return true;
  }
  // Consumer side: look at the oldest element without removing it; skip() then removes it.
  bool peek(T& out) const noexcept {
    uint32_t r = r_.load(std::memory_order_relaxed);
    if (r == w_.load(std::memory_order_acquire)) return false;
    out = buf_[r & mask_];
    return true;
  }
  void skip() noexcept { r_.store(r_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

 private:
  const uint32_t mask_;
  std::unique_ptr<T[]> buf_;
  std::atomic<uint32_t> w_{0};
  std::atomic<uint32_t> r_{0};
};

// ---- strings ----------------------------------------------------------------------------------
inline void copyStr(char* dst, size_t cap, const char* src) {
  if (!cap) return;
  size_t n = src ? std::strlen(src) : 0;
  if (n >= cap) n = cap - 1;
  if (n) std::memcpy(dst, src, n);
  dst[n] = 0;
}
// Bounded char8 field -> std::string (fields are not guaranteed NUL-terminated).
template <size_t N>
std::string fieldStr(const char (&f)[N]) {
  size_t n = 0;
  while (n < N && f[n]) ++n;
  return std::string(f, n);
}
// UTF-16 (bounded, NUL-terminated or full) -> UTF-8.
std::string toUtf8(const Steinberg::char16* s, size_t maxLen);
template <size_t N>
std::string toUtf8(const Steinberg::char16 (&s)[N]) {
  return toUtf8(s, N);
}
// UTF-8 -> UTF-16 into a String128-style buffer (always NUL-terminated, truncated).
void toUtf16(const char* s, Steinberg::char16* out, size_t cap);

// ---- IBStream over sq::IByteStream ----------------------------------------------------------------
class StreamAdapter final : public FObject<StreamAdapter, Steinberg::IBStream, Steinberg::ISizeableStream> {
 public:
  explicit StreamAdapter(sq::IByteStream* s) : s_(s) {}
  tresult PLUGIN_API read(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesRead) override;
  tresult PLUGIN_API write(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesWritten) override;
  tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode, Steinberg::int64* result) override;
  tresult PLUGIN_API tell(Steinberg::int64* pos) override;
  tresult PLUGIN_API getStreamSize(Steinberg::int64& size) override;
  tresult PLUGIN_API setStreamSize(Steinberg::int64 size) override;

 private:
  sq::Ptr<sq::IByteStream> s_;
};

}  // namespace sqvst3
