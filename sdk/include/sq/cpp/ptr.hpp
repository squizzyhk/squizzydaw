// Squizzy Studio SDK - C++ helpers: intrusive smart pointer for sq interfaces. Header-only.
#pragma once

#include <cstddef>
#include <utility>

#include "sq/unknown.h"

namespace sq {

// Owning reference to an sq interface. Ptr<T>::adopt() takes an already add-ref'd pointer
// (what createInstance/queryInterface return); the constructor from T* adds a reference.
template <class T>
class Ptr {
 public:
  Ptr() noexcept = default;
  Ptr(std::nullptr_t) noexcept {}
  explicit Ptr(T* p) noexcept : p_(p) {
    if (p_) p_->addRef();
  }
  Ptr(const Ptr& o) noexcept : p_(o.p_) {
    if (p_) p_->addRef();
  }
  Ptr(Ptr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  ~Ptr() { reset(); }

  Ptr& operator=(const Ptr& o) noexcept {
    if (this != &o) {
      if (o.p_) o.p_->addRef();
      reset();
      p_ = o.p_;
    }
    return *this;
  }
  Ptr& operator=(Ptr&& o) noexcept {
    if (this != &o) {
      reset();
      p_ = o.p_;
      o.p_ = nullptr;
    }
    return *this;
  }
  Ptr& operator=(std::nullptr_t) noexcept {
    reset();
    return *this;
  }

  static Ptr adopt(T* p) noexcept {
    Ptr r;
    r.p_ = p;
    return r;
  }

  T* get() const noexcept { return p_; }
  T* operator->() const noexcept { return p_; }
  T& operator*() const noexcept { return *p_; }
  explicit operator bool() const noexcept { return p_ != nullptr; }

  // Out-parameter helpers: release current, return address for the callee to fill.
  T** put() noexcept {
    reset();
    return &p_;
  }
  void** putVoid() noexcept {
    reset();
    return reinterpret_cast<void**>(&p_);
  }
  T* detach() noexcept {
    T* p = p_;
    p_ = nullptr;
    return p;
  }
  void reset() noexcept {
    if (p_) {
      T* p = p_;
      p_ = nullptr;
      p->release();
    }
  }

  // queryInterface to U (U must declare SQ_INTERFACE / static iid). Empty Ptr on failure.
  template <class U>
  Ptr<U> as() const noexcept {
    Ptr<U> r;
    if (p_) p_->queryInterface(&U::iid, r.putVoid());
    return r;
  }

 private:
  T* p_ = nullptr;
};

template <class U, class T>
Ptr<U> queryInterface(T* obj) noexcept {
  Ptr<U> r;
  if (obj) obj->queryInterface(&U::iid, r.putVoid());
  return r;
}

}  // namespace sq
