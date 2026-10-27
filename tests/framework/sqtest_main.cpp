#include <cstring>
#include <exception>

#include "sqtest.hpp"

namespace sqtest {

std::vector<Case>& registry() {
  static std::vector<Case> r;
  return r;
}

static int gFailures = 0;

void reportFailure(const char* file, int line, const std::string& msg) {
  ++gFailures;
  std::fprintf(stdout, "    FAIL %s:%d: %s\n", file, line, msg.c_str());
  std::fflush(stdout);
}

}  // namespace sqtest

int main(int argc, char** argv) {
  const char* filter = nullptr;
  bool list = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--list") == 0) list = true;
    else filter = argv[i];
  }
  auto& cases = sqtest::registry();
  int ran = 0, failedCases = 0, skippedCases = 0;
  for (auto& c : cases) {
    if (filter && !std::strstr(c.name, filter)) continue;
    if (list) {
      std::printf("%s\n", c.name);
      continue;
    }
    ++ran;
    int before = sqtest::gFailures;
    std::printf("[ RUN  ] %s\n", c.name);
    std::fflush(stdout);
    bool skipped = false;
    try {
      c.fn();
    } catch (const sqtest::SkipCase& s) {
      skipped = true;
      std::printf("    SKIP %s\n", s.reason.c_str());
    } catch (const sqtest::AbortCase&) {
    } catch (const std::exception& e) {
      sqtest::reportFailure(c.file, c.line, std::string("unexpected exception: ") + e.what());
    } catch (...) {
      sqtest::reportFailure(c.file, c.line, "unexpected non-std exception");
    }
    bool ok = sqtest::gFailures == before;
    if (!ok) ++failedCases;
    else if (skipped) ++skippedCases;
    std::printf("[ %s ] %s\n", !ok ? "FAIL" : skipped ? "SKIP" : " OK ", c.name);
    std::fflush(stdout);
  }
  if (list) return 0;
  std::printf("== %d test case(s), %d failed, %d skipped, %d failed check(s)\n", ran, failedCases, skippedCases, sqtest::gFailures);
  if (ran == 0) {
    std::printf("== no test cases matched\n");
    return 2;
  }
  if (failedCases) return 1;
  return skippedCases ? sqtest::kSkipExitCode : 0;
}
