#include "sq/app/host_context.hpp"

#include "sq/platform.h"

namespace sq::app {

void HostContext::clearServices() {
  std::map<sq_guid, sq::Ptr<sq::IUnknown>> drop;
  {
    std::lock_guard<std::mutex> lk(m_);
    drop.swap(services_);
  }
}

size_t HostContext::serviceCount() const {
  std::lock_guard<std::mutex> lk(m_);
  return services_.size();
}

void SQ_CALL HostContext::log(int32_t level, const char* tag, const char* message) { sq_log_write(level, tag, message); }

sq_result SQ_CALL HostContext::queryService(const sq_guid* riid, void** out) {
  if (!out) return SQ_E_INVALIDARG;
  *out = nullptr;
  if (!riid) return SQ_E_INVALIDARG;
  sq::Ptr<sq::IUnknown> svc;
  {
    std::lock_guard<std::mutex> lk(m_);
    auto it = services_.find(*riid);
    if (it == services_.end()) return SQ_E_NOTFOUND;
    svc = it->second;
  }
  return svc->queryInterface(riid, out);
}

sq_result SQ_CALL HostContext::registerService(const sq_guid* riid, sq::IUnknown* service) {
  if (!riid || !service) return SQ_E_INVALIDARG;
  std::lock_guard<std::mutex> lk(m_);
  if (services_.count(*riid)) return SQ_E_EXISTS;
  services_[*riid] = sq::Ptr<sq::IUnknown>(service);
  return SQ_OK;
}

sq_result SQ_CALL HostContext::unregisterService(const sq_guid* riid, sq::IUnknown* service) {
  if (!riid) return SQ_E_INVALIDARG;
  sq::Ptr<sq::IUnknown> drop;
  {
    std::lock_guard<std::mutex> lk(m_);
    auto it = services_.find(*riid);
    if (it == services_.end()) return SQ_E_NOTFOUND;
    if (service && it->second.get() != service) return SQ_E_ACCESS;
    drop = std::move(it->second);
    services_.erase(it);
  }
  return SQ_OK;
}

}  // namespace sq::app
