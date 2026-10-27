// sq-pack: command-line front end (see sq/tools/packager/packager.hpp and README.md).
#include <cstdio>
#include <string>
#include <vector>

#include "sq/platform.h"
#include "sq/tools/packager/packager.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

std::vector<std::string> utf8Args(int argc, char** argv) {
  std::vector<std::string> args;
#if defined(_WIN32)
  (void)argc;
  (void)argv;
  int n = 0;
  LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
  for (int i = 1; w && i < n; ++i) {
    const size_t len = wcslen(w[i]);
    std::string s(len * 3 + 1, '\0');
    size_t outLen = 0;
    if (sq_utf16_to_utf8(reinterpret_cast<const uint16_t*>(w[i]), len, s.data(), s.size(), &outLen) == SQ_OK) s.resize(outLen);
    else s.clear();
    args.push_back(std::move(s));
  }
  if (w) LocalFree(w);
#else
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
#endif
  return args;
}

}  // namespace

int main(int argc, char** argv) {
  std::string out, err;
  const int code = sq::packager::runCli(utf8Args(argc, argv), out, err);
  if (!out.empty()) std::fwrite(out.data(), 1, out.size(), stdout);
  if (!err.empty()) std::fwrite(err.data(), 1, err.size(), stderr);
  return code;
}
