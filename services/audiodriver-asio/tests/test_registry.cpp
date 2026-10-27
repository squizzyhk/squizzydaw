// audiodriver-asio: installed-driver enumeration on a fake registry view.
#include <map>
#include <string>
#include <vector>

#include "asio_registry.hpp"
#include "sqtest.hpp"

namespace {

class FakeRegistry final : public sq::asio::RegistryView {
 public:
  bool present = true;
  std::vector<std::string> order;                                    // subkey order as enumerated
  std::map<std::string, std::map<std::string, std::string>> values;  // key -> value name -> string

  void add(const std::string& key, const char* clsid, const char* desc = nullptr) {
    order.push_back(key);
    auto& v = values[key];
    if (clsid) v["CLSID"] = clsid;
    if (desc) v["Description"] = desc;
  }
  bool listSubkeys(std::vector<std::string>& out) override {
    out = present ? order : std::vector<std::string>{};
    return present;
  }
  bool readString(const std::string& subkey, const char* value, std::string& out) override {
    auto k = values.find(subkey);
    if (k == values.end()) return false;
    auto v = k->second.find(value);
    if (v == k->second.end()) return false;
    out = v->second;
    return true;
  }
};

}  // namespace

SQ_TEST("asio registry: CLSID normalisation") {
  std::string out;
  CHECK(sq::asio::normalizeClsid("{0a1b2c3d-4e5f-6789-abcd-ef0123456789}", out));
  CHECK_EQ(out, std::string("{0A1B2C3D-4E5F-6789-ABCD-EF0123456789}"));
  CHECK(sq::asio::normalizeClsid("  0A1B2C3D-4E5F-6789-ABCD-EF0123456789 ", out));
  CHECK_EQ(out, std::string("{0A1B2C3D-4E5F-6789-ABCD-EF0123456789}"));
  CHECK(!sq::asio::normalizeClsid("", out));
  CHECK(!sq::asio::normalizeClsid("{0A1B2C3D-4E5F-6789-ABCD-EF012345678}", out));    // short
  CHECK(!sq::asio::normalizeClsid("{0A1B2C3D-4E5F-6789-ABCD-EF012345678G}", out));   // non-hex
  CHECK(!sq::asio::normalizeClsid("{0A1B2C3D04E5F-6789-ABCD-EF0123456789}", out));   // dash missing
  CHECK(!sq::asio::normalizeClsid("0A1B2C3D-4E5F-6789-ABCD-EF0123456789}", out));    // unbalanced
  CHECK(!sq::asio::normalizeClsid("{{0A1B2C3D-4E5F-6789-ABCD-EF0123456789}}", out));
}

SQ_TEST("asio registry: missing key -> no drivers, keyPresent false") {
  FakeRegistry reg;
  reg.present = false;
  const auto r = sq::asio::enumerateDrivers(reg);
  CHECK(!r.keyPresent);
  CHECK(r.drivers.empty());
  CHECK(r.skipped.empty());
}

SQ_TEST("asio registry: valid, nameless, malformed, missing and duplicate entries") {
  FakeRegistry reg;
  reg.add("Zeta Interface", "{11111111-2222-3333-4444-555555555555}", "  Zeta Pro Interface  ");
  reg.add("alpha box", "{aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee}");                   // no Description
  reg.add("Broken", "not-a-guid", "Broken driver");
  reg.add("NoClsid", nullptr, "Has no class");
  reg.add("Alias of Zeta", "{11111111-2222-3333-4444-555555555555}", "Zeta again");  // duplicate CLSID
  reg.add("Mid", "{99999999-8888-7777-6666-555555555555}", "");                     // empty Description
  const auto r = sq::asio::enumerateDrivers(reg);
  CHECK(r.keyPresent);
  REQUIRE_EQ(r.drivers.size(), size_t{3});
  // sorted by display name, case-insensitive
  CHECK_EQ(r.drivers[0].name, std::string("alpha box"));
  CHECK_EQ(r.drivers[0].clsid, std::string("{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}"));
  CHECK_EQ(r.drivers[0].id, std::string("asio:{AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE}"));
  CHECK_EQ(r.drivers[0].keyName, std::string("alpha box"));
  CHECK_EQ(r.drivers[1].name, std::string("Mid"));
  CHECK_EQ(r.drivers[2].name, std::string("Zeta Pro Interface"));  // trimmed description wins over key name
  CHECK_EQ(r.drivers[2].keyName, std::string("Zeta Interface"));    // first key kept for the duplicate
  REQUIRE_EQ(r.skipped.size(), size_t{3});
  CHECK(r.skipped[0].find("Broken") == 0 && r.skipped[0].find("malformed") != std::string::npos);
  CHECK(r.skipped[1].find("NoClsid") == 0 && r.skipped[1].find("no CLSID") != std::string::npos);
  CHECK(r.skipped[2].find("Alias of Zeta") == 0 && r.skipped[2].find("duplicate") != std::string::npos);
}

SQ_TEST("asio registry: ids are stable across renames and key order") {
  FakeRegistry a, b;
  a.add("One", "{00000000-0000-0000-0000-000000000001}", "First");
  a.add("Two", "{00000000-0000-0000-0000-000000000002}", "Second");
  b.add("Two renamed", "{00000000-0000-0000-0000-000000000002}", "Second (v2)");
  b.add("One", "{00000000-0000-0000-0000-000000000001}", "First");
  const auto ra = sq::asio::enumerateDrivers(a), rb = sq::asio::enumerateDrivers(b);
  REQUIRE_EQ(ra.drivers.size(), size_t{2});
  REQUIRE_EQ(rb.drivers.size(), size_t{2});
  CHECK_EQ(ra.drivers[0].id, rb.drivers[0].id);
  CHECK_EQ(ra.drivers[1].id, rb.drivers[1].id);
}

SQ_TEST("asio registry: entry count is bounded") {
  FakeRegistry reg;
  for (int i = 0; i < 300; ++i) {
    char clsid[64];
    std::snprintf(clsid, sizeof clsid, "{00000000-0000-0000-0000-%012d}", i);
    reg.add("drv" + std::to_string(i), clsid);
  }
  const auto r = sq::asio::enumerateDrivers(reg);
  CHECK_EQ(r.drivers.size(), sq::asio::kMaxDrivers);
  CHECK(!r.skipped.empty());
}

SQ_TEST("asio registry: the system view opens without crashing") {
  auto view = sq::asio::openSystemRegistry();
  REQUIRE(view != nullptr);
  const auto r = sq::asio::enumerateDrivers(*view);
  std::printf("asio registry: key %s, %zu valid driver entr%s, %zu skipped\n", r.keyPresent ? "present" : "absent",
              r.drivers.size(), r.drivers.size() == 1 ? "y" : "ies", r.skipped.size());
  for (const auto& d : r.drivers) CHECK(d.id.rfind("asio:{", 0) == 0);
}
