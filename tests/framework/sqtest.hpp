// sqtest - tiny self-contained test framework (own code, no dependencies).
//
//   #include "sqtest.hpp"
//   SQ_TEST("guid parses braced form") {
//     CHECK(x == 1);                 // records failure, continues
//     CHECK_EQ(a, b);                // prints both values on failure
//     REQUIRE(ptr != nullptr);       // records failure, aborts this test case
//     SQ_SKIP("no audio device");   // case reported SKIPPED; exe exits 77 (CTest: Skipped)
//   }
//
// Link with sq::test (provides main). Run: sqtest_<name> [substring-filter] [--list]
#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace sqtest {

struct Case {
  const char* name;
  const char* file;
  int line;
  void (*fn)();
};

std::vector<Case>& registry();
void reportFailure(const char* file, int line, const std::string& msg);

struct AbortCase {};
// Thrown by SQ_SKIP: the case is reported as SKIPPED (not passed). A run whose cases all passed
// or skipped, with at least one skip, exits with kSkipExitCode, which sq_add_test registers as
// CTest's SKIP_RETURN_CODE - so a hardware check that could not run is visible, never green.
struct SkipCase {
  std::string reason;
};
inline constexpr int kSkipExitCode = 77;

struct Registrar {
  Registrar(const char* name, const char* file, int line, void (*fn)()) { registry().push_back(Case{name, file, line, fn}); }
};

template <class T>
std::string show(const T& v) {
  if constexpr (std::is_same_v<T, std::nullptr_t>) {
    return "nullptr";
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(v));
  } else if constexpr (requires(std::ostream& o) { o << v; }) {
    std::ostringstream o;
    o << v;
    return o.str();
  } else {
    return "<unprintable>";
  }
}

}  // namespace sqtest

#define SQTEST_CAT2(a, b) a##b
#define SQTEST_CAT(a, b) SQTEST_CAT2(a, b)
#define SQ_TEST(name)                                                                                        \
  static void SQTEST_CAT(sqtest_fn_, __LINE__)();                                                            \
  static ::sqtest::Registrar SQTEST_CAT(sqtest_reg_, __LINE__)(name, __FILE__, __LINE__, &SQTEST_CAT(sqtest_fn_, __LINE__)); \
  static void SQTEST_CAT(sqtest_fn_, __LINE__)()

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) ::sqtest::reportFailure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define REQUIRE(cond)                                                       \
  do {                                                                      \
    if (!(cond)) {                                                          \
      ::sqtest::reportFailure(__FILE__, __LINE__, "REQUIRE(" #cond ")");    \
      throw ::sqtest::AbortCase{};                                          \
    }                                                                       \
  } while (0)

#define CHECK_EQ(a, b)                                                                                     \
  do {                                                                                                     \
    const auto& sqtest_a_ = (a);                                                                           \
    const auto& sqtest_b_ = (b);                                                                           \
    if (!(sqtest_a_ == sqtest_b_))                                                                         \
      ::sqtest::reportFailure(__FILE__, __LINE__,                                                          \
                              "CHECK_EQ(" #a ", " #b "): " + ::sqtest::show(sqtest_a_) + " != " + ::sqtest::show(sqtest_b_)); \
  } while (0)

#define REQUIRE_EQ(a, b)                                                                                   \
  do {                                                                                                     \
    const auto& sqtest_a_ = (a);                                                                           \
    const auto& sqtest_b_ = (b);                                                                           \
    if (!(sqtest_a_ == sqtest_b_)) {                                                                       \
      ::sqtest::reportFailure(__FILE__, __LINE__,                                                          \
                              "REQUIRE_EQ(" #a ", " #b "): " + ::sqtest::show(sqtest_a_) + " != " + ::sqtest::show(sqtest_b_)); \
      throw ::sqtest::AbortCase{};                                                                         \
    }                                                                                                      \
  } while (0)

// Skips the rest of the current case (e.g. no audio hardware). See SkipCase.
#define SQ_SKIP(reason) throw ::sqtest::SkipCase{std::string(reason)}

// For sq_result calls: CHECK_OK(sq_fs_remove(p));
#define CHECK_OK(expr) CHECK_EQ((expr), 0)
#define REQUIRE_OK(expr) REQUIRE_EQ((expr), 0)
