// Squizzy Studio app - host context handed to every module (implements sq::IHostContext).
#pragma once

#include <map>
#include <mutex>

#include "sq/app/class_registry.hpp"
#include "sq/app/settings.hpp"

namespace sq::app {

class HostContext final : public sq::StaticObject<HostContext, sq::IHostContext> {
 public:
  // hostAbi is injectable so tests can simulate an older/newer host.
  HostContext(Settings& settings, ClassRegistry& registry, uint32_t hostAbi = SQ_PLATFORM_ABI)
      : settings_(settings), registry_(registry), hostAbi_(hostAbi) {}
  ~HostContext() override { clearServices(); }

  Settings& settings() { return settings_; }
  ClassRegistry& registry() { return registry_; }
  void clearServices();
  size_t serviceCount() const;

  // ---- sq::IHostContext ----
  uint32_t SQ_CALL getHostAbi() override { return hostAbi_; }
  uint32_t SQ_CALL getHostVersion() override { return SQ_VERSION; }
  void SQ_CALL log(int32_t level, const char* tag, const char* message) override;
  sq::ISettings* SQ_CALL getSettings() override { return &settings_; }
  sq::IClassRegistry* SQ_CALL getClassRegistry() override { return &registry_; }
  sq_result SQ_CALL queryService(const sq_guid* riid, void** out) override;
  sq_result SQ_CALL registerService(const sq_guid* riid, sq::IUnknown* service) override;
  sq_result SQ_CALL unregisterService(const sq_guid* riid, sq::IUnknown* service) override;

 private:
  Settings& settings_;
  ClassRegistry& registry_;
  uint32_t hostAbi_;
  mutable std::mutex m_;
  std::map<sq_guid, sq::Ptr<sq::IUnknown>> services_;
};

}  // namespace sq::app
