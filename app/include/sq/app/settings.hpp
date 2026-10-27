// Squizzy Studio app - hierarchical dotted-key settings registry (implements sq::ISettings).
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "sq/sdk.hpp"

namespace sq::app {

class Settings final : public sq::StaticObject<Settings, sq::ISettings> {
 public:
  Settings() = default;
  ~Settings() override = default;

  // ---- C++ API (app-internal) ----
  static bool validKey(const char* key);
  std::string get(const std::string& key, const std::string& def = {}) const;
  // Sets the value only if the key is missing (for defaults). Returns true if it was set.
  bool setDefault(const std::string& key, int32_t type, const std::string& value);
  uint32_t observe(const std::string& prefix, std::function<void(const char*)> fn);
  size_t size() const;

  // XML persistence. Load merges into the current values (and notifies observers).
  sq_result loadFile(const std::string& path, std::string* err = nullptr);
  sq_result saveFile(const std::string& path) const;
  sq_result loadXml(const std::string& xml, std::string* err = nullptr);
  std::string toXml() const;

  // ---- sq::ISettings ----
  sq_bool SQ_CALL has(const char* key) override;
  int32_t SQ_CALL getType(const char* key) override;
  sq_result SQ_CALL getString(const char* key, char* buf, uint32_t bufSize, uint32_t* needed) override;
  int64_t SQ_CALL getInt(const char* key, int64_t def) override;
  double SQ_CALL getDouble(const char* key, double def) override;
  sq_bool SQ_CALL getBool(const char* key, sq_bool def) override;
  sq_result SQ_CALL setString(const char* key, const char* value) override;
  sq_result SQ_CALL setInt(const char* key, int64_t value) override;
  sq_result SQ_CALL setDouble(const char* key, double value) override;
  sq_result SQ_CALL setBool(const char* key, sq_bool value) override;
  sq_result SQ_CALL remove(const char* key) override;
  sq_result SQ_CALL enumerate(const char* prefix, sq_settings_enum_fn fn, void* user) override;
  sq_result SQ_CALL addObserver(const char* prefix, sq::ISettingsObserver* observer, uint32_t* cookie) override;
  sq_result SQ_CALL removeObserver(uint32_t cookie) override;

  static const char* typeName(int32_t type);
  static int32_t typeFromName(const char* name);

 private:
  struct Entry {
    int32_t type = SQ_SETTING_NONE;
    std::string value;
  };
  struct Observer {
    uint32_t cookie;
    std::string prefix;
    sq::Ptr<sq::ISettingsObserver> iface;
    std::function<void(const char*)> fn;
  };

  sq_result set(const char* key, int32_t type, std::string value);
  void notify(const std::vector<std::string>& keys);
  static bool underPrefix(const std::string& key, const std::string& prefix);

  mutable std::mutex m_;
  std::map<std::string, Entry> values_;
  std::vector<Observer> observers_;
  uint32_t nextCookie_ = 1;
};

}  // namespace sq::app
