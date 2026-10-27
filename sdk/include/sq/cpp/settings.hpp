// Squizzy Studio SDK - C++ conveniences over sq::ISettings / sq::IClassRegistry. Header-only.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "sq/cpp/object.hpp"
#include "sq/cpp/ptr.hpp"

namespace sq {

inline std::string getString(ISettings* s, const char* key, const std::string& def = {}) {
  if (!s) return def;
  char small[256];
  uint32_t needed = 0;
  sq_result r = s->getString(key, small, sizeof small, &needed);
  if (r == SQ_OK) return std::string(small);
  if (r == SQ_E_BUFFER_TOO_SMALL && needed > 0) {
    std::string big(needed, '\0');
    if (s->getString(key, big.data(), needed, &needed) == SQ_OK) {
      big.resize(needed - 1);
      return big;
    }
  }
  return def;
}

inline std::vector<std::string> listKeys(ISettings* s, const char* prefix) {
  std::vector<std::string> keys;
  if (!s) return keys;
  s->enumerate(
      prefix, [](void* user, const char* key) { static_cast<std::vector<std::string>*>(user)->emplace_back(key); }, &keys);
  return keys;
}

// Adapts a std::function to ISettingsObserver. Keep the returned Ptr alive while observing.
class SettingsObserverFn final : public Object<SettingsObserverFn, ISettingsObserver> {
 public:
  explicit SettingsObserverFn(std::function<void(const char*)> fn) : fn_(std::move(fn)) {}
  void SQ_CALL onSettingChanged(const char* key) override {
    if (fn_) fn_(key);
  }

 private:
  std::function<void(const char*)> fn_;
};

template <class T>
Ptr<T> createInstance(IClassRegistry* reg, const sq_guid& classId) {
  Ptr<T> r;
  if (reg) reg->createInstance(&classId, &T::iid, r.putVoid());
  return r;
}

}  // namespace sq
