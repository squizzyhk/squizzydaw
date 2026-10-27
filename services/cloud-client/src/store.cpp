#include "store.hpp"

#include <cstring>

#include "json_util.hpp"
#include "sq/platform.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>
#endif

namespace sq::cloud {

namespace {

std::string toHex(const uint8_t* d, size_t n) {
  std::string s(n * 2 + 1, '\0');
  sq_hex_encode(d, n, s.data());
  s.resize(n * 2);
  return s;
}

#if defined(_WIN32)
bool dpapi(const std::string& in, std::string& out, bool protect) {
  DATA_BLOB src{static_cast<DWORD>(in.size()), reinterpret_cast<BYTE*>(const_cast<char*>(in.data()))};
  DATA_BLOB dst{};
  BOOL ok = protect ? CryptProtectData(&src, L"SquizzyStudio cloud", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dst)
                    : CryptUnprotectData(&src, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dst);
  if (!ok) return false;
  out.assign(reinterpret_cast<const char*>(dst.pbData), dst.cbData);
  SecureZeroMemory(dst.pbData, dst.cbData);
  LocalFree(dst.pbData);
  return true;
}
#endif

class FileSecretStore final : public SecretStore {
 public:
  explicit FileSecretStore(std::string dir) : dir_(std::move(dir)) {}

  bool get(const std::string& name, std::string& out) override {
    auto data = sq::readFile(path(name));
    if (!data) return false;
#if defined(_WIN32)
    return dpapi(*data, out, false);
#else
    out = *data;
    return true;
#endif
  }

  bool put(const std::string& name, const std::string& data) override {
    if (sq_fs_create_dirs(dir_.c_str()) != SQ_OK) return false;
#if defined(_WIN32)
    std::string blob;
    if (!dpapi(data, blob, true)) return false;
    return sq::writeFile(path(name), blob) == SQ_OK;
#else
    return sq::writeFile(path(name), data) == SQ_OK;
#endif
  }

  void erase(const std::string& name) override { sq_fs_remove(path(name).c_str()); }

 private:
  std::string path(const std::string& name) const { return sq::joinPath(dir_, name + ".secret"); }
  std::string dir_;
};

}  // namespace

std::unique_ptr<SecretStore> makeFileSecretStore(const std::string& dir) { return std::make_unique<FileSecretStore>(dir); }

std::string randomBytes(size_t n) {
  std::string out(n, '\0');
#if defined(_WIN32)
  if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) ==
      0)
    return out;
#endif
  size_t at = 0;
  while (at < n) {
    sq_guid g{};
    sq_guid_generate(&g);
    uint8_t buf[sizeof g];
    std::memcpy(buf, &g, sizeof g);
    for (size_t i = 0; i < sizeof g && at < n; ++i) out[at++] = static_cast<char>(buf[i]);
  }
  return out;
}

std::string deviceKey(SecretStore& store) {
  std::string key;
  if (store.get("device-key", key) && key.size() == 32) return key;
  key = randomBytes(32);
  store.put("device-key", key);
  return key;
}

std::string hmacSha256Hex(const std::string& key, const std::string& message) {
  uint8_t k[64] = {};
  if (key.size() > 64) {
    sq_sha256(key.data(), key.size(), k);
  } else {
    std::memcpy(k, key.data(), key.size());
  }
  uint8_t ipad[64], opad[64];
  for (int i = 0; i < 64; ++i) {
    ipad[i] = static_cast<uint8_t>(k[i] ^ 0x36);
    opad[i] = static_cast<uint8_t>(k[i] ^ 0x5c);
  }
  uint8_t inner[32], outer[32];
  sq_sha256_ctx c;
  sq_sha256_init(&c);
  sq_sha256_update(&c, ipad, 64);
  sq_sha256_update(&c, message.data(), message.size());
  sq_sha256_final(&c, inner);
  sq_sha256_init(&c);
  sq_sha256_update(&c, opad, 64);
  sq_sha256_update(&c, inner, 32);
  sq_sha256_final(&c, outer);
  return toHex(outer, 32);
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < a.size(); ++i) diff = static_cast<unsigned char>(diff | (a[i] ^ b[i]));
  return diff == 0;
}

std::string signEntitlements(const Entitlements& e, const std::string& key) {
  JsonPtr payload(sq_json_new_object());
  sq_json_set(payload.get(), "display_name", jsonStr(e.displayName));
  sq_json* feats = sq_json_new_array();
  for (const auto& f : e.features) sq_json_append(feats, jsonStr(f));
  sq_json_set(payload.get(), "features", feats);
  sq_json_set(payload.get(), "last_verified", jsonNum(e.lastVerified));
  sq_json_set(payload.get(), "valid_until", jsonNum(e.validUntil));
  std::string text = jsonWrite(payload.get());
  JsonPtr doc(sq_json_new_object());
  sq_json_set(doc.get(), "format", jsonNum(1));
  sq_json_set(doc.get(), "payload", jsonStr(text));
  sq_json_set(doc.get(), "sig", jsonStr(hmacSha256Hex(key, text)));
  return jsonWrite(doc.get(), true);
}

bool verifyEntitlements(const std::string& doc, const std::string& key, Entitlements& out, std::string* err) {
  auto setErr = [&](const char* m) {
    if (err) *err = m;
    return false;
  };
  JsonPtr j = jsonParse(doc);
  if (!j || sq_json_type_of(j.get()) != SQ_JSON_OBJECT) return setErr("cache: not a JSON object");
  if (jsonInt(j.get(), "format", 0) != 1) return setErr("cache: unknown format");
  std::string payload = jsonString(j.get(), "payload");
  std::string sig = jsonString(j.get(), "sig");
  if (payload.empty() || sig.empty()) return setErr("cache: missing payload or signature");
  if (!constantTimeEquals(hmacSha256Hex(key, payload), sig)) return setErr("cache: bad signature");
  JsonPtr p = jsonParse(payload);
  if (!p || sq_json_type_of(p.get()) != SQ_JSON_OBJECT) return setErr("cache: bad payload");
  Entitlements e;
  e.displayName = jsonString(p.get(), "display_name");
  e.lastVerified = jsonInt(p.get(), "last_verified", 0);
  e.validUntil = jsonInt(p.get(), "valid_until", 0);
  if (const sq_json* f = sq_json_get(p.get(), "features"); f && sq_json_type_of(f) == SQ_JSON_ARRAY) {
    for (size_t i = 0; i < sq_json_size(f); ++i) {
      const sq_json* s = sq_json_at(f, i);
      if (sq_json_type_of(s) != SQ_JSON_STRING) continue;
      size_t n = 0;
      const char* c = sq_json_get_string(s, &n);
      e.features.emplace_back(c, n);
    }
  }
  out = std::move(e);
  return true;
}

bool parseEntitlementResponse(const std::string& body, Entitlements& out) {
  JsonPtr j = jsonParse(body);
  if (!j || sq_json_type_of(j.get()) != SQ_JSON_OBJECT) return false;
  const sq_json* f = sq_json_get(j.get(), "features");
  if (!f || sq_json_type_of(f) != SQ_JSON_ARRAY) return false;
  Entitlements e;
  e.displayName = jsonString(j.get(), "display_name");
  e.validUntil = jsonInt(j.get(), "valid_until", 0);
  for (size_t i = 0; i < sq_json_size(f) && i < 1024; ++i) {
    const sq_json* s = sq_json_at(f, i);
    if (sq_json_type_of(s) != SQ_JSON_STRING) continue;
    size_t n = 0;
    const char* c = sq_json_get_string(s, &n);
    if (n > 0 && n <= 128) e.features.emplace_back(c, n);
  }
  out = std::move(e);
  return true;
}

bool featureGranted(const std::vector<std::string>& features, const char* feature) {
  if (!feature || !*feature) return false;
  std::string f = feature;
  if (f.rfind("core.", 0) == 0) return true;
  for (const auto& g : features) {
    if (g == f) return true;
    if (g.size() >= 2 && g.compare(g.size() - 2, 2, ".*") == 0 && f.size() > g.size() - 1 &&
        f.compare(0, g.size() - 1, g, 0, g.size() - 1) == 0)
      return true;
  }
  return false;
}

}  // namespace sq::cloud
