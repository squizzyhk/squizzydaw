// cloud-client: local persistence.
//
// SecretStore: small named secrets (the device signing key, the refresh token). The file store
// protects them with DPAPI (current user) on Windows; elsewhere they are plain files in the
// data directory (documented gap). MemorySecretStore is for tests.
//
// Entitlement cache: {"format":1,"payload":"<json text>","sig":"<hex HMAC-SHA256>"}. The HMAC key
// is a random per-device key from the SecretStore, so a cache copied to another machine or edited
// by hand fails verification and is ignored (the account then reads as signed out). This protects
// integrity against casual edits and stale copies; it is not a DRM boundary against a user who
// can read their own DPAPI secrets.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sq::cloud {

class SecretStore {
 public:
  virtual ~SecretStore() = default;
  virtual bool get(const std::string& name, std::string& out) = 0;
  virtual bool put(const std::string& name, const std::string& data) = 0;
  virtual void erase(const std::string& name) = 0;
};

class MemorySecretStore final : public SecretStore {
 public:
  bool get(const std::string& name, std::string& out) override {
    auto it = m_.find(name);
    if (it == m_.end()) return false;
    out = it->second;
    return true;
  }
  bool put(const std::string& name, const std::string& data) override {
    m_[name] = data;
    return true;
  }
  void erase(const std::string& name) override { m_.erase(name); }

 private:
  std::map<std::string, std::string> m_;
};

// Files "<dir>/<name>.secret".
std::unique_ptr<SecretStore> makeFileSecretStore(const std::string& dir);

// 32 random bytes from the OS CSPRNG (BCryptGenRandom on Windows; GUID entropy elsewhere).
std::string randomBytes(size_t n);
// Loads the device signing key from the store or creates and stores a new one.
std::string deviceKey(SecretStore& store);

std::string hmacSha256Hex(const std::string& key, const std::string& message);
bool constantTimeEquals(const std::string& a, const std::string& b);

struct Entitlements {
  std::string displayName;
  std::vector<std::string> features;  // "sync.presets", or a prefix wildcard "sync.*"
  int64_t lastVerified = 0;           // unix seconds of the last successful online check
  int64_t validUntil = 0;             // unix seconds; 0 = no server-side end
};

std::string signEntitlements(const Entitlements& e, const std::string& key);
// False on a malformed document or a bad signature (err says which).
bool verifyEntitlements(const std::string& doc, const std::string& key, Entitlements& out, std::string* err);
// Parses the server's entitlement response {"display_name","features":[...],"valid_until"}.
bool parseEntitlementResponse(const std::string& body, Entitlements& out);
// "core.*" is always granted; otherwise exact match or a "prefix.*" entry.
bool featureGranted(const std::vector<std::string>& features, const char* feature);

}  // namespace sq::cloud
