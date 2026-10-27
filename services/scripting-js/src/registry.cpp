// scripting-js: IObjectRegistry (URL -> object), the settings mirror and the package:// resolver.
#include <cstring>

#include "engine.hpp"
#include "sq/cpp/attributes.hpp"
#include "sq/cpp/bytestream.hpp"

namespace sqjs {

namespace {

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// IController over one settings key (or a key prefix). getProperty("value") reads the key
// itself; getProperty("Child") reads "<key>.Child"; getChild walks down.
class SettingsNode final : public sq::Object<SettingsNode, sq::IController> {
 public:
  SettingsNode(sq::ISettings* s, std::string key) : s_(s), key_(std::move(key)) {}

  sq_result SQ_CALL getProperty(const char* name, sq::IAttributes* out) override {
    if (!out) return SQ_E_INVALIDARG;
    std::string k = keyFor(name);
    if (k.empty() || !s_->has(k.c_str())) return SQ_E_NOTFOUND;
    switch (s_->getType(k.c_str())) {
      case SQ_SETTING_INT: return out->setInt("value", s_->getInt(k.c_str(), 0));
      case SQ_SETTING_DOUBLE: return out->setDouble("value", s_->getDouble(k.c_str(), 0.0));
      case SQ_SETTING_BOOL: return out->setBool("value", s_->getBool(k.c_str(), SQ_FALSE));
      default: return out->setString("value", sq::getString(s_, k.c_str()).c_str());
    }
  }
  sq_result SQ_CALL setProperty(const char* name, sq::IAttributes* in) override {
    if (!in) return SQ_E_INVALIDARG;
    std::string k = keyFor(name);
    if (k.empty()) return SQ_E_INVALIDARG;
    switch (in->getType("value")) {
      case SQ_VALUE_INT: return s_->setInt(k.c_str(), in->getInt("value", 0));
      case SQ_VALUE_DOUBLE: return s_->setDouble(k.c_str(), in->getDouble("value", 0.0));
      case SQ_VALUE_BOOL: return s_->setBool(k.c_str(), in->getBool("value", SQ_FALSE));
      case SQ_VALUE_STRING: return s_->setString(k.c_str(), sq::getAttrString(in, "value").c_str());
      case SQ_VALUE_GUID: {
        sq_guid g{};
        in->getGuid("value", &g);
        return s_->setString(k.c_str(), guidToString(g).c_str());
      }
      case SQ_VALUE_NONE: return s_->remove(k.c_str());
      default: return SQ_E_TYPE;
    }
  }
  sq_result SQ_CALL getChild(const char* name, const sq_guid* riid, void** out) override {
    if (!out) return SQ_E_INVALIDARG;
    *out = nullptr;
    if (!name || !*name || !riid) return SQ_E_INVALIDARG;
    sq::Ptr<SettingsNode> n = sq::Ptr<SettingsNode>::adopt(new SettingsNode(s_, keyFor(name)));
    return n->queryInterface(riid, out);
  }

 private:
  std::string keyFor(const char* name) const {
    if (!name || !*name || std::strcmp(name, "value") == 0) return key_;
    std::string n = name;
    for (char& c : n)
      if (c == '/') c = '.';
    return key_.empty() ? n : key_ + "." + n;
  }

  sq::ISettings* s_;
  std::string key_;
};

}  // namespace

// ---- ObjectRegistry ----------------------------------------------------------------------
sq::Ptr<sq::IUrlResolver> ObjectRegistry::find(const std::string& url, std::string* effectiveUrl) {
  std::lock_guard<std::mutex> lk(m_);
  const std::string* best = nullptr;
  for (auto& [prefix, r] : resolvers_)
    if (startsWith(url, prefix.c_str()) && (!best || prefix.size() > best->size())) best = &prefix;
  if (best) {
    *effectiveUrl = url;
    return resolvers_[*best];
  }
  if (startsWith(url, kAppSettingsPrefix)) {
    auto it = resolvers_.find(kSettingsPrefix);
    if (it != resolvers_.end()) {
      *effectiveUrl = std::string(kSettingsPrefix) + url.substr(std::strlen(kAppSettingsPrefix));
      return it->second;
    }
  }
  return {};
}

sq_result SQ_CALL ObjectRegistry::registerResolver(const char* prefix, sq::IUrlResolver* resolver) {
  if (!prefix || !*prefix || !resolver) return SQ_E_INVALIDARG;
  try {
    std::lock_guard<std::mutex> lk(m_);
    if (resolvers_.count(prefix)) return SQ_E_EXISTS;
    resolvers_[prefix] = sq::Ptr<sq::IUrlResolver>(resolver);
    return SQ_OK;
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
}

sq_result SQ_CALL ObjectRegistry::unregisterResolver(const char* prefix, sq::IUrlResolver* resolver) {
  if (!prefix) return SQ_E_INVALIDARG;
  sq::Ptr<sq::IUrlResolver> gone;
  {
    std::lock_guard<std::mutex> lk(m_);
    auto it = resolvers_.find(prefix);
    if (it == resolvers_.end() || (resolver && it->second.get() != resolver)) return SQ_E_NOTFOUND;
    gone = std::move(it->second);
    resolvers_.erase(it);
  }
  return SQ_OK;  // `gone` released outside the lock
}

sq_result SQ_CALL ObjectRegistry::registerObject(const char* url, sq::IUnknown* object) {
  if (!url || !*url || !object) return SQ_E_INVALIDARG;
  try {
    std::lock_guard<std::mutex> lk(m_);
    if (objects_.count(url)) return SQ_E_EXISTS;
    objects_[url] = sq::Ptr<sq::IUnknown>(object);
    return SQ_OK;
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
}

sq_result SQ_CALL ObjectRegistry::unregisterObject(const char* url) {
  if (!url) return SQ_E_INVALIDARG;
  sq::Ptr<sq::IUnknown> gone;
  {
    std::lock_guard<std::mutex> lk(m_);
    auto it = objects_.find(url);
    if (it == objects_.end()) return SQ_E_NOTFOUND;
    gone = std::move(it->second);
    objects_.erase(it);
  }
  return SQ_OK;
}

sq_result SQ_CALL ObjectRegistry::resolve(const char* url, const sq_guid* riid, void** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  if (!url || !riid) return SQ_E_INVALIDARG;
  try {
    const std::string u = url;
    sq::Ptr<sq::IUnknown> obj;
    {
      std::lock_guard<std::mutex> lk(m_);
      auto it = objects_.find(u);
      if (it != objects_.end()) obj = it->second;
    }
    if (obj) return obj->queryInterface(riid, out);
    std::string eff;
    sq::Ptr<sq::IUrlResolver> r = find(u, &eff);
    if (!r) return SQ_E_NOTFOUND;
    return r->resolve(eff.c_str(), riid, out);
  } catch (...) {
    return SQ_E_FAIL;
  }
}

sq_result SQ_CALL ObjectRegistry::openStream(const char* url, sq::IByteStream** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  if (!url) return SQ_E_INVALIDARG;
  try {
    std::string eff;
    sq::Ptr<sq::IUrlResolver> r = find(url, &eff);
    if (!r) return SQ_E_NOTFOUND;
    return r->openStream(eff.c_str(), out);
  } catch (...) {
    return SQ_E_FAIL;
  }
}

void ObjectRegistry::clear() {
  std::map<std::string, sq::Ptr<sq::IUrlResolver>> r;
  std::map<std::string, sq::Ptr<sq::IUnknown>> o;
  {
    std::lock_guard<std::mutex> lk(m_);
    r.swap(resolvers_);
    o.swap(objects_);
  }
}

// ---- settings mirror ---------------------------------------------------------------------
sq_result SQ_CALL SettingsResolver::resolve(const char* url, const sq_guid* riid, void** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  if (!url || !riid || !settings_) return SQ_E_INVALIDARG;
  std::string u = url;
  std::string key;
  if (startsWith(u, kSettingsPrefix))
    key = u.substr(std::strlen(kSettingsPrefix));
  else if (startsWith(u, kAppSettingsPrefix))
    key = u.substr(std::strlen(kAppSettingsPrefix));
  else
    return SQ_E_NOTFOUND;
  for (char& c : key)
    if (c == '/') c = '.';
  while (!key.empty() && key.back() == '.') key.pop_back();
  for (char c : key) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (!ok) return SQ_E_INVALIDARG;
  }
  try {
    sq::Ptr<SettingsNode> n = sq::Ptr<SettingsNode>::adopt(new SettingsNode(settings_, key));
    return n->queryInterface(riid, out);
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
}

// ---- package:// ----------------------------------------------------------------------------
namespace {
bool splitPackageUrl(const char* url, std::string* id, std::string* path) {
  if (!url || std::strncmp(url, kPackagePrefix, std::strlen(kPackagePrefix)) != 0) return false;
  std::string rest = url + std::strlen(kPackagePrefix);
  size_t slash = rest.find('/');
  *id = rest.substr(0, slash);
  *path = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
  return !id->empty();
}
}  // namespace

sq_result SQ_CALL PackageResolver::resolve(const char* url, const sq_guid* riid, void** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  if (!riid) return SQ_E_INVALIDARG;
  std::string id, path;
  if (!splitPackageUrl(url, &id, &path)) return SQ_E_INVALIDARG;
  if (!path.empty()) {
    if (*riid == sq::IByteStream::iid || *riid == sq::IUnknown::iid) {
      sq::Ptr<sq::IByteStream> s;
      sq_result r = openStream(url, s.put());
      if (SQ_FAILED(r)) return r;
      return s->queryInterface(riid, out);
    }
    return SQ_E_NOINTERFACE;
  }
  std::lock_guard<std::mutex> lk(m_);
  if (!engine_) return SQ_E_CLOSED;
  sq::Ptr<Package> p = engine_->findPackage(id);
  if (!p) return SQ_E_NOTFOUND;
  return p->queryInterface(riid, out);
}

sq_result SQ_CALL PackageResolver::openStream(const char* url, sq::IByteStream** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  std::string id, path;
  if (!splitPackageUrl(url, &id, &path) || path.empty()) return SQ_E_INVALIDARG;
  if (!PackageSource::safeRelative(path)) return SQ_E_ACCESS;
  sq::Ptr<Package> p;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!engine_) return SQ_E_CLOSED;
    p = engine_->findPackage(id);
  }
  if (!p) return SQ_E_NOTFOUND;
  std::string data;
  if (!p->readFile(path, data)) return SQ_E_NOTFOUND;
  try {
    *out = sq::makeMemoryStream(data.data(), data.size(), false).detach();
    return SQ_OK;
  } catch (...) {
    return SQ_E_OUTOFMEMORY;
  }
}

}  // namespace sqjs
