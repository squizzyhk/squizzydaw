#include <set>
#include <unordered_set>

#include "sq/platform.hpp"
#include "sq/sdk.hpp"
#include "sqtest.hpp"

extern "C" {
int sq_c_abi_probe(void);
size_t sq_c_sizeof_module_info(void);
size_t sq_c_sizeof_class_info(void);
size_t sq_c_factory_vtbl_slots(void);
size_t sq_c_host_vtbl_slots(void);
}

SQ_TEST("guid: parse braced and bare, format canonical") {
  sq_guid a{}, b{};
  REQUIRE(sq_guid_parse("{9C63D502-ED61-4EE2-8E62-41DB33A4689D}", &a));
  REQUIRE(sq_guid_parse("9c63d502-ed61-4ee2-8e62-41db33a4689d", &b));
  CHECK(a == b);
  CHECK_EQ(a.data1, 0x9C63D502u);
  CHECK_EQ(a.data2, 0xED61);
  CHECK_EQ(a.data3, 0x4EE2);
  CHECK_EQ(int(a.data4[0]), 0x8E);
  CHECK_EQ(int(a.data4[7]), 0x9D);
  CHECK_EQ(sq::toString(a), std::string("{9C63D502-ED61-4EE2-8E62-41DB33A4689D}"));
}

SQ_TEST("guid: rejects malformed input") {
  sq_guid g{};
  const char* bad[] = {"",
                       "{9C63D502-ED61-4EE2-8E62-41DB33A4689D",
                       "9C63D502-ED61-4EE2-8E62-41DB33A4689D}",
                       "{9C63D502ED61-4EE2-8E62-41DB33A4689D}",
                       "{9C63D502-ED61-4EE2-8E62-41DB33A4689G}",
                       "{9C63D502-ED61-4EE2-8E62-41DB33A4689D}x",
                       "{9C63D502-ED61-4EE2-8E62-41DB33A468}"};
  for (const char* s : bad) CHECK(!sq_guid_parse(s, &g));
  CHECK(!sq_guid_parse(nullptr, &g));
  // Edge cases: whitespace, doubled braces, misplaced dashes, truncation, out-param untouched.
  const char* edge[] = {" {9C63D502-ED61-4EE2-8E62-41DB33A4689D}",
                        "{9C63D502-ED61-4EE2-8E62-41DB33A4689D} ",
                        "9C63D502-ED61-4EE2-8E62-41DB33A4689D ",
                        "{{9C63D502-ED61-4EE2-8E62-41DB33A4689D}}",
                        "{9C63D502-ED614-EE2-8E62-41DB33A4689D}",
                        "{9C63D502-ED61-4EE2-8E62-41DB33A4689D0}",
                        "9C63D502-ED61-4EE2-8E62-41DB33A4689D00",
                        "{9C63D502-ED61-4EE2-8E62-41DB33A4689-}",
                        "{0x63D502-ED61-4EE2-8E62-41DB33A4689D}",
                        "{9C63D502",
                        "{"};
  sq_guid sentinel = sq::guid("{11111111-2222-3333-4444-555555555555}");
  for (const char* s : edge) {
    sq_guid out = sentinel;
    CHECK(!sq_guid_parse(s, &out));
    CHECK(out == sentinel);
  }
  sq_guid mixed{};
  CHECK(sq_guid_parse("{9c63D502-eD61-4ee2-8E62-41db33A4689d}", &mixed));
  CHECK(mixed == sq::guid("{9C63D502-ED61-4EE2-8E62-41DB33A4689D}"));
  CHECK(!sq_guid_parse("{9C63D502-ED61-4EE2-8E62-41DB33A4689D}", nullptr));
}

SQ_TEST("guid: constexpr literal matches runtime parse") {
  constexpr sq_guid c = sq::guid("{42812941-1F61-4F13-8D50-F7A905B737C7}");
  static_assert(c.data1 == 0x42812941u, "constexpr parse");
  sq_guid r{};
  REQUIRE(sq::parseGuid("{42812941-1F61-4F13-8D50-F7A905B737C7}", r));
  CHECK(c == r);
  CHECK(!(c < r) && !(r < c));
}

SQ_TEST("guid: generated GUIDs are v4, unique, roundtrip") {
  std::set<sq_guid> seen;
  std::unordered_set<sq_guid> hashed;
  for (int i = 0; i < 200; ++i) {
    sq_guid g{};
    REQUIRE_OK(sq_guid_generate(&g));
    CHECK_EQ((g.data3 >> 12), 4);
    CHECK_EQ((g.data4[0] & 0xC0), 0x80);
    sq_guid back{};
    CHECK(sq::parseGuid(sq::toString(g), back) && back == g);
    seen.insert(g);
    hashed.insert(g);
  }
  CHECK_EQ(seen.size(), size_t(200));
  CHECK_EQ(hashed.size(), size_t(200));
  sq_guid zero{};
  CHECK(sq_guid_is_null(&zero) != 0);
}

SQ_TEST("guid: C headers compile as C and agree with C++ layout") {
  CHECK_EQ(sq_c_abi_probe(), 1);
  CHECK_EQ(sq_c_sizeof_module_info(), sizeof(sq_module_info));
  CHECK_EQ(sq_c_sizeof_class_info(), sizeof(sq_class_info));
  CHECK_EQ(sq_c_factory_vtbl_slots(), size_t(8));  // 3 IUnknown + 5 factory methods
  CHECK_EQ(sq_c_host_vtbl_slots(), size_t(11));    // 3 IUnknown + 8 host methods
  CHECK(sq::IUnknown::iid == sq_guid SQ_IID_IUNKNOWN_INIT);
}
