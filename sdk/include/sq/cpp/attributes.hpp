// Squizzy Studio SDK - header-only IAttributes implementation (sq/interfaces/common.h).
//   auto a = sq::makeAttributes();             // Ptr<IAttributes>, refcount 1
//   a->setString("name", "Kick"); editor->execute(&sq::doc::kAddTrack, a.get(), result.get());
// Not thread-safe, allocates: never use on the audio thread.
#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "sq/cpp/object.hpp"
#include "sq/cpp/ptr.hpp"
#include "sq/interfaces/common.h"

namespace sq {

class AttributeSet final : public Object<AttributeSet, IAttributes> {
 public:
  ~AttributeSet() override { clear(); }

  uint32_t SQ_CALL getCount() override { return static_cast<uint32_t>(items_.size()); }
  sq_result SQ_CALL getKey(uint32_t index, char* buf, uint32_t bufSize, uint32_t* needed) override {
    if (index >= items_.size()) return SQ_E_NOTFOUND;
    return copyOut(items_[index].key.data(), items_[index].key.size(), true, buf, bufSize, needed);
  }
  int32_t SQ_CALL getType(const char* key) override {
    const Item* it = find(key);
    return it ? it->type : SQ_VALUE_NONE;
  }
  int64_t SQ_CALL getInt(const char* key, int64_t def) override {
    const Item* it = find(key);
    if (!it) return def;
    switch (it->type) {
      case SQ_VALUE_INT: case SQ_VALUE_BOOL: return it->i;
      case SQ_VALUE_DOUBLE: return static_cast<int64_t>(it->d);
      default: return def;
    }
  }
  double SQ_CALL getDouble(const char* key, double def) override {
    const Item* it = find(key);
    if (!it) return def;
    switch (it->type) {
      case SQ_VALUE_DOUBLE: return it->d;
      case SQ_VALUE_INT: case SQ_VALUE_BOOL: return static_cast<double>(it->i);
      default: return def;
    }
  }
  sq_bool SQ_CALL getBool(const char* key, sq_bool def) override {
    const Item* it = find(key);
    if (!it) return def;
    switch (it->type) {
      case SQ_VALUE_BOOL: case SQ_VALUE_INT: return it->i != 0 ? SQ_TRUE : SQ_FALSE;
      case SQ_VALUE_DOUBLE: return it->d != 0.0 ? SQ_TRUE : SQ_FALSE;
      default: return def;
    }
  }
  sq_result SQ_CALL getString(const char* key, char* buf, uint32_t bufSize, uint32_t* needed) override {
    const Item* it = find(key);
    if (!it) return SQ_E_NOTFOUND;
    if (it->type != SQ_VALUE_STRING) return SQ_E_TYPE;
    return copyOut(it->bytes.data(), it->bytes.size(), true, buf, bufSize, needed);
  }
  sq_result SQ_CALL getGuid(const char* key, sq_guid* out) override {
    const Item* it = find(key);
    if (!it) return SQ_E_NOTFOUND;
    if (it->type != SQ_VALUE_GUID) return SQ_E_TYPE;
    if (!out) return SQ_E_INVALIDARG;
    *out = it->g;
    return SQ_OK;
  }
  sq_result SQ_CALL getBlob(const char* key, void* buf, uint32_t bufSize, uint32_t* needed) override {
    const Item* it = find(key);
    if (!it) return SQ_E_NOTFOUND;
    if (it->type != SQ_VALUE_BLOB && it->type != SQ_VALUE_STRING) return SQ_E_TYPE;
    return copyOut(it->bytes.data(), it->bytes.size(), false, buf, bufSize, needed);
  }
  sq_result SQ_CALL getObject(const char* key, const sq_guid* riid, void** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    const Item* it = find(key);
    if (!it) return SQ_E_NOTFOUND;
    if (it->type != SQ_VALUE_OBJECT || !it->obj) return SQ_E_TYPE;
    return it->obj->queryInterface(riid, out);
  }
  sq_result SQ_CALL setInt(const char* key, int64_t v) override { return setScalar(key, SQ_VALUE_INT, v, 0.0); }
  sq_result SQ_CALL setDouble(const char* key, double v) override { return setScalar(key, SQ_VALUE_DOUBLE, 0, v); }
  sq_result SQ_CALL setBool(const char* key, sq_bool v) override { return setScalar(key, SQ_VALUE_BOOL, v ? 1 : 0, 0.0); }
  sq_result SQ_CALL setString(const char* key, const char* v) override {
    Item* it = slot(key);
    if (!it) return SQ_E_INVALIDARG;
    it->type = SQ_VALUE_STRING;
    it->bytes.assign(v ? v : "", v ? std::strlen(v) : 0);
    return SQ_OK;
  }
  sq_result SQ_CALL setGuid(const char* key, const sq_guid* v) override {
    if (!v) return SQ_E_INVALIDARG;
    Item* it = slot(key);
    if (!it) return SQ_E_INVALIDARG;
    it->type = SQ_VALUE_GUID;
    it->g = *v;
    return SQ_OK;
  }
  sq_result SQ_CALL setBlob(const char* key, const void* data, uint32_t size) override {
    if (!data && size) return SQ_E_INVALIDARG;
    Item* it = slot(key);
    if (!it) return SQ_E_INVALIDARG;
    it->type = SQ_VALUE_BLOB;
    it->bytes.assign(static_cast<const char*>(data), size);
    return SQ_OK;
  }
  sq_result SQ_CALL setObject(const char* key, IUnknown* v) override {
    Item* it = slot(key);
    if (!it) return SQ_E_INVALIDARG;
    if (v) v->addRef();
    it->type = SQ_VALUE_OBJECT;
    it->obj = v;
    return SQ_OK;
  }
  sq_result SQ_CALL remove(const char* key) override {
    for (auto i = items_.begin(); i != items_.end(); ++i) {
      if (key && i->key == key) {
        if (i->obj) i->obj->release();
        items_.erase(i);
        return SQ_OK;
      }
    }
    return SQ_S_FALSE;
  }
  void SQ_CALL clear() override {
    for (auto& i : items_)
      if (i.obj) i.obj->release();
    items_.clear();
  }

 private:
  struct Item {
    std::string key;
    int32_t type = SQ_VALUE_NONE;
    int64_t i = 0;
    double d = 0.0;
    sq_guid g{};
    std::string bytes;
    IUnknown* obj = nullptr;
  };

  const Item* find(const char* key) const {
    if (!key) return nullptr;
    for (const auto& i : items_)
      if (i.key == key) return &i;
    return nullptr;
  }
  // Existing item reset to a clean value (keeps position), or a new one appended.
  Item* slot(const char* key) {
    if (!key || !*key) return nullptr;
    for (auto& i : items_) {
      if (i.key == key) {
        if (i.obj) i.obj->release();
        std::string k = std::move(i.key);
        i = Item{};
        i.key = std::move(k);
        return &i;
      }
    }
    items_.push_back(Item{});
    items_.back().key = key;
    return &items_.back();
  }
  sq_result setScalar(const char* key, int32_t type, int64_t iv, double dv) {
    Item* it = slot(key);
    if (!it) return SQ_E_INVALIDARG;
    it->type = type;
    it->i = iv;
    it->d = dv;
    return SQ_OK;
  }
  static sq_result copyOut(const char* data, size_t size, bool nul, void* buf, uint32_t bufSize, uint32_t* needed) {
    const size_t need = size + (nul ? 1 : 0);
    if (needed) *needed = static_cast<uint32_t>(need);
    if (!buf || bufSize < need) {
      if (buf && bufSize && nul) {
        std::memcpy(buf, data, bufSize - 1);
        static_cast<char*>(buf)[bufSize - 1] = '\0';
      }
      return SQ_E_BUFFER_TOO_SMALL;
    }
    if (size) std::memcpy(buf, data, size);
    if (nul) static_cast<char*>(buf)[size] = '\0';
    return SQ_OK;
  }

  std::vector<Item> items_;
};

inline Ptr<IAttributes> makeAttributes() { return Ptr<IAttributes>::adopt(new AttributeSet()); }

// Convenience: read a string attribute into std::string ("" if missing / wrong type).
inline std::string getAttrString(IAttributes* a, const char* key) {
  if (!a) return {};
  uint32_t need = 0;
  if (a->getString(key, nullptr, 0, &need) != SQ_E_BUFFER_TOO_SMALL || need == 0) return {};
  std::string s(need, '\0');
  if (a->getString(key, s.data(), need, &need) != SQ_OK) return {};
  s.resize(need - 1);
  return s;
}

}  // namespace sq
