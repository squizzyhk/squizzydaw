#include "sq/app/settings.hpp"

#include <charconv>
#include <cmath>
#include <cstring>
#include <locale>
#include <sstream>

#include "sq/platform.hpp"

namespace sq::app {
namespace {

bool parseInt(const std::string& s, int64_t& out) {
  if (s.empty()) return false;
  auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc() && r.ptr == s.data() + s.size();
}
bool parseDouble(const std::string& s, double& out) {
  if (s.empty()) return false;
#if defined(__APPLE__)  // Apple libc++ lacks floating-point from_chars
  std::istringstream ss(s);
  ss.imbue(std::locale::classic());
  ss >> out;
  return !ss.fail() && ss.peek() == std::char_traits<char>::eof();
#else
  auto r = std::from_chars(s.data(), s.data() + s.size(), out);
  return r.ec == std::errc() && r.ptr == s.data() + s.size();
#endif
}
std::string fmtDouble(double d) {
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof buf, d);
  return std::string(buf, r.ptr);
}

}  // namespace

const char* Settings::typeName(int32_t type) {
  switch (type) {
    case SQ_SETTING_STRING: return "string";
    case SQ_SETTING_INT: return "int";
    case SQ_SETTING_DOUBLE: return "double";
    case SQ_SETTING_BOOL: return "bool";
    default: return "none";
  }
}
int32_t Settings::typeFromName(const char* name) {
  if (!name) return SQ_SETTING_STRING;
  if (std::strcmp(name, "int") == 0) return SQ_SETTING_INT;
  if (std::strcmp(name, "double") == 0) return SQ_SETTING_DOUBLE;
  if (std::strcmp(name, "bool") == 0) return SQ_SETTING_BOOL;
  return SQ_SETTING_STRING;
}

bool Settings::validKey(const char* key) {
  if (!key || !*key) return false;
  bool segStart = true;
  for (const char* p = key; *p; ++p) {
    char c = *p;
    if (c == '.') {
      if (segStart) return false;  // empty segment
      segStart = true;
      continue;
    }
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
    segStart = false;
  }
  return !segStart;
}

bool Settings::underPrefix(const std::string& key, const std::string& prefix) {
  if (prefix.empty()) return true;
  if (key.size() < prefix.size() || key.compare(0, prefix.size(), prefix) != 0) return false;
  return key.size() == prefix.size() || key[prefix.size()] == '.';
}

std::string Settings::get(const std::string& key, const std::string& def) const {
  std::lock_guard<std::mutex> lk(m_);
  auto it = values_.find(key);
  return it == values_.end() ? def : it->second.value;
}

size_t Settings::size() const {
  std::lock_guard<std::mutex> lk(m_);
  return values_.size();
}

bool Settings::setDefault(const std::string& key, int32_t type, const std::string& value) {
  if (!validKey(key.c_str())) return false;
  {
    std::lock_guard<std::mutex> lk(m_);
    if (values_.count(key)) return false;
  }
  return set(key.c_str(), type, value) == SQ_OK;
}

sq_result Settings::set(const char* key, int32_t type, std::string value) {
  if (!validKey(key)) return SQ_E_INVALIDARG;
  {
    std::lock_guard<std::mutex> lk(m_);
    Entry& e = values_[key];
    if (e.type == type && e.value == value) return SQ_S_FALSE;  // unchanged: no notification
    e.type = type;
    e.value = std::move(value);
  }
  notify({key});
  return SQ_OK;
}

void Settings::notify(const std::vector<std::string>& keys) {
  if (keys.empty()) return;
  std::vector<Observer> obs;
  {
    std::lock_guard<std::mutex> lk(m_);
    obs = observers_;  // snapshot: observers may (un)register from the callback
  }
  for (const auto& k : keys)
    for (const auto& o : obs)
      if (underPrefix(k, o.prefix)) {
        if (o.iface) o.iface->onSettingChanged(k.c_str());
        if (o.fn) o.fn(k.c_str());
      }
}

uint32_t Settings::observe(const std::string& prefix, std::function<void(const char*)> fn) {
  std::lock_guard<std::mutex> lk(m_);
  uint32_t c = nextCookie_++;
  observers_.push_back(Observer{c, prefix, nullptr, std::move(fn)});
  return c;
}

// ---- ISettings ----
sq_bool SQ_CALL Settings::has(const char* key) {
  if (!key) return SQ_FALSE;
  std::lock_guard<std::mutex> lk(m_);
  return values_.count(key) ? SQ_TRUE : SQ_FALSE;
}

int32_t SQ_CALL Settings::getType(const char* key) {
  if (!key) return SQ_SETTING_NONE;
  std::lock_guard<std::mutex> lk(m_);
  auto it = values_.find(key);
  return it == values_.end() ? SQ_SETTING_NONE : it->second.type;
}

sq_result SQ_CALL Settings::getString(const char* key, char* buf, uint32_t bufSize, uint32_t* needed) {
  if (!key) return SQ_E_INVALIDARG;
  std::string v;
  {
    std::lock_guard<std::mutex> lk(m_);
    auto it = values_.find(key);
    if (it == values_.end()) return SQ_E_NOTFOUND;
    v = it->second.value;
  }
  uint32_t need = static_cast<uint32_t>(v.size() + 1);
  if (needed) *needed = need;
  if (!buf || bufSize == 0) return SQ_E_BUFFER_TOO_SMALL;
  size_t n = v.size() < bufSize - 1 ? v.size() : bufSize - 1;
  std::memcpy(buf, v.data(), n);
  buf[n] = '\0';
  return need <= bufSize ? SQ_OK : SQ_E_BUFFER_TOO_SMALL;
}

int64_t SQ_CALL Settings::getInt(const char* key, int64_t def) {
  if (!key) return def;
  std::lock_guard<std::mutex> lk(m_);
  auto it = values_.find(key);
  if (it == values_.end()) return def;
  const Entry& e = it->second;
  int64_t i;
  double d;
  if (e.type == SQ_SETTING_BOOL) return e.value == "true" ? 1 : 0;
  if (parseInt(e.value, i)) return i;
  if (parseDouble(e.value, d) && std::isfinite(d)) return static_cast<int64_t>(d);
  return def;
}

double SQ_CALL Settings::getDouble(const char* key, double def) {
  if (!key) return def;
  std::lock_guard<std::mutex> lk(m_);
  auto it = values_.find(key);
  if (it == values_.end()) return def;
  double d;
  if (it->second.type == SQ_SETTING_BOOL) return it->second.value == "true" ? 1.0 : 0.0;
  return parseDouble(it->second.value, d) ? d : def;
}

sq_bool SQ_CALL Settings::getBool(const char* key, sq_bool def) {
  if (!key) return def;
  std::lock_guard<std::mutex> lk(m_);
  auto it = values_.find(key);
  if (it == values_.end()) return def;
  const std::string& v = it->second.value;
  if (v == "true" || v == "1" || v == "yes" || v == "on") return SQ_TRUE;
  if (v == "false" || v == "0" || v == "no" || v == "off") return SQ_FALSE;
  int64_t i;
  if (parseInt(v, i)) return i != 0 ? SQ_TRUE : SQ_FALSE;
  return def;
}

sq_result SQ_CALL Settings::setString(const char* key, const char* value) { return set(key, SQ_SETTING_STRING, value ? value : ""); }
sq_result SQ_CALL Settings::setInt(const char* key, int64_t value) { return set(key, SQ_SETTING_INT, std::to_string(value)); }
sq_result SQ_CALL Settings::setDouble(const char* key, double value) {
  if (!std::isfinite(value)) return SQ_E_INVALIDARG;
  return set(key, SQ_SETTING_DOUBLE, fmtDouble(value));
}
sq_result SQ_CALL Settings::setBool(const char* key, sq_bool value) { return set(key, SQ_SETTING_BOOL, value ? "true" : "false"); }

sq_result SQ_CALL Settings::remove(const char* key) {
  if (!validKey(key)) return SQ_E_INVALIDARG;
  std::vector<std::string> removed;
  {
    std::lock_guard<std::mutex> lk(m_);
    std::string k = key;
    for (auto it = values_.lower_bound(k); it != values_.end() && it->first.compare(0, k.size(), k) == 0;) {
      if (underPrefix(it->first, k)) {
        removed.push_back(it->first);
        it = values_.erase(it);
      } else {
        ++it;
      }
    }
  }
  notify(removed);
  return removed.empty() ? SQ_E_NOTFOUND : SQ_OK;
}

sq_result SQ_CALL Settings::enumerate(const char* prefix, sq_settings_enum_fn fn, void* user) {
  if (!fn) return SQ_E_INVALIDARG;
  std::string p = prefix ? prefix : "";
  std::vector<std::string> keys;
  {
    std::lock_guard<std::mutex> lk(m_);
    for (auto& kv : values_)
      if (underPrefix(kv.first, p)) keys.push_back(kv.first);
  }
  for (auto& k : keys) fn(user, k.c_str());
  return SQ_OK;
}

sq_result SQ_CALL Settings::addObserver(const char* prefix, sq::ISettingsObserver* observer, uint32_t* cookie) {
  if (!observer || !cookie) return SQ_E_INVALIDARG;
  std::string p = prefix ? prefix : "";
  if (!p.empty() && !validKey(p.c_str())) return SQ_E_INVALIDARG;
  std::lock_guard<std::mutex> lk(m_);
  uint32_t c = nextCookie_++;
  observers_.push_back(Observer{c, p, sq::Ptr<sq::ISettingsObserver>(observer), nullptr});
  *cookie = c;
  return SQ_OK;
}

sq_result SQ_CALL Settings::removeObserver(uint32_t cookie) {
  sq::Ptr<sq::ISettingsObserver> dropped;  // released outside the lock
  {
    std::lock_guard<std::mutex> lk(m_);
    for (auto it = observers_.begin(); it != observers_.end(); ++it)
      if (it->cookie == cookie) {
        dropped = std::move(it->iface);
        observers_.erase(it);
        return SQ_OK;
      }
  }
  return SQ_E_NOTFOUND;
}

// ---- XML persistence ----
//   <Settings format="1">
//     <Section name="Engine"><Value name="BlockSize" type="int">256</Value></Section>
//   </Settings>
std::string Settings::toXml() const {
  auto doc = sq::xmlCreate("Settings");
  sq::XmlNode root = sq::xmlRoot(doc);
  root.setAttr("format", "1");
  std::map<std::string, Entry> copy;
  {
    std::lock_guard<std::mutex> lk(m_);
    copy = values_;
  }
  for (auto& [key, e] : copy) {
    sq::XmlNode parent = root;
    size_t start = 0;
    for (;;) {
      size_t dot = key.find('.', start);
      if (dot == std::string::npos) break;
      std::string seg = key.substr(start, dot - start);
      sq::XmlNode found;
      for (sq::XmlNode c : parent.children("Section"))
        if (c.attr("name") == seg) {
          found = c;
          break;
        }
      if (!found) {
        found = parent.append("Section");
        found.setAttr("name", seg);
      }
      parent = found;
      start = dot + 1;
    }
    sq::XmlNode v = parent.append("Value");
    v.setAttr("name", key.substr(start));
    v.setAttr("type", typeName(e.type));
    v.setText(e.value);
  }
  return sq::xmlWrite(doc, true);
}

sq_result Settings::loadXml(const std::string& xml, std::string* err) {
  auto doc = sq::xmlParse(xml, err);
  if (!doc) return SQ_E_PARSE;
  sq::XmlNode root = sq::xmlRoot(doc);
  if (root.name() != "Settings") {
    if (err) *err = "root element is not <Settings>";
    return SQ_E_PARSE;
  }
  struct Item {
    std::string key;
    int32_t type;
    std::string value;
  };
  std::vector<Item> items;
  std::function<void(sq::XmlNode, const std::string&)> walk = [&](sq::XmlNode n, const std::string& prefix) {
    for (sq::XmlNode c : n.children()) {
      std::string name = c.attr("name");
      if (name.empty()) continue;
      std::string key = prefix.empty() ? name : prefix + "." + name;
      if (c.name() == "Section") walk(c, key);
      else if (c.name() == "Value" && validKey(key.c_str()))
        items.push_back(Item{key, typeFromName(c.attr("type", "string").c_str()), std::string(c.text())});
    }
  };
  walk(root, "");
  for (auto& it : items) set(it.key.c_str(), it.type, it.value);
  return SQ_OK;
}

sq_result Settings::loadFile(const std::string& path, std::string* err) {
  auto data = sq::readFile(path);
  if (!data) {
    if (err) *err = "cannot read " + path;
    return SQ_E_NOTFOUND;
  }
  return loadXml(*data, err);
}

sq_result Settings::saveFile(const std::string& path) const { return sq::writeFile(path, toXml()); }

}  // namespace sq::app
