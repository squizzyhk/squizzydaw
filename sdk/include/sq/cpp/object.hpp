// Squizzy Studio SDK - C++ helpers: base implementations of IUnknown. Header-only.
//
//   class MyThing final : public sq::Object<MyThing, IFoo, IBar> {
//    public:
//     sq_result SQ_CALL fooMethod() override { ... }
//   };
//
// Object<> provides refcounting (starts at 1), queryInterface over the listed interfaces and
// their bases (via each interface's sq_base/iid), and counts live objects per binary so the
// module factory can answer canUnload().
#pragma once

#include <atomic>
#include <type_traits>

#include "sq/module.h"

namespace sq {

namespace detail {

// One counter per module binary (headers are compiled into each DLL; nothing here is exported).
inline std::atomic<int32_t>& liveObjects() noexcept {
  static std::atomic<int32_t> count{0};
  return count;
}

template <class I>
constexpr bool interfaceMatches(const sq_guid& riid) noexcept {
  if (I::iid == riid) return true;
  if constexpr (!std::is_void_v<typename I::sq_base>) {
    return interfaceMatches<typename I::sq_base>(riid);
  } else {
    return false;
  }
}

}  // namespace detail

template <class Derived, class... Interfaces>
class Object : public Interfaces... {
  static_assert(sizeof...(Interfaces) >= 1, "Object needs at least one interface");

 public:
  Object() noexcept { detail::liveObjects().fetch_add(1, std::memory_order_relaxed); }
  Object(const Object&) = delete;
  Object& operator=(const Object&) = delete;

  sq_result SQ_CALL queryInterface(const sq_guid* riid, void** out) override { return queryInterfaceImpl(riid, out); }
  uint32_t SQ_CALL addRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
  uint32_t SQ_CALL release() override {
    uint32_t r = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (r == 0) delete static_cast<Derived*>(this);
    return r;
  }

 protected:
  virtual ~Object() { detail::liveObjects().fetch_sub(1, std::memory_order_relaxed); }

  // Derived classes overriding queryInterface can fall back to this.
  sq_result queryInterfaceImpl(const sq_guid* riid, void** out) noexcept {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (!riid) return SQ_E_INVALIDARG;
    void* p = nullptr;
    Derived* self = static_cast<Derived*>(this);
    ((p == nullptr && detail::interfaceMatches<Interfaces>(*riid) ? (p = static_cast<Interfaces*>(self), true) : false), ...);
    if (!p) return SQ_E_NOINTERFACE;
    addRef();
    *out = p;
    return SQ_OK;
  }

 private:
  std::atomic<uint32_t> refs_{1};
};

// For host-owned objects with static/owner-managed lifetime: refcount calls are no-ops.
template <class Derived, class... Interfaces>
class StaticObject : public Interfaces... {
 public:
  sq_result SQ_CALL queryInterface(const sq_guid* riid, void** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (!riid) return SQ_E_INVALIDARG;
    void* p = nullptr;
    Derived* self = static_cast<Derived*>(this);
    ((p == nullptr && detail::interfaceMatches<Interfaces>(*riid) ? (p = static_cast<Interfaces*>(self), true) : false), ...);
    if (!p) return SQ_E_NOINTERFACE;
    *out = p;
    return SQ_OK;
  }
  uint32_t SQ_CALL addRef() override { return 1; }
  uint32_t SQ_CALL release() override { return 1; }

 protected:
  StaticObject() = default;
  virtual ~StaticObject() = default;
};

}  // namespace sq
