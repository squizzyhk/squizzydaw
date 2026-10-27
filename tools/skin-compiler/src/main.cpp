// sq-skinc: validates theme folders and packs them for embedding (see skinc.hpp).
#include <cstdio>
#include <string>
#include <vector>

#include "sq/tools/skin_compiler/skinc.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
  // argv is in the ANSI code page; the platform layer wants UTF-8 paths.
  int wargc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  if (wargv) {
    std::vector<std::string> args;
    for (int i = 0; i < wargc; ++i) {
      int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
      std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
      if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), n, nullptr, nullptr);
      args.push_back(std::move(s));
    }
    LocalFree(wargv);
    std::vector<char*> ptrs;
    for (auto& a : args) ptrs.push_back(a.data());
    ptrs.push_back(nullptr);
    return sq::tools::skinc::runCli(static_cast<int>(args.size()), ptrs.data(), stdout, stderr);
  }
#endif
  return sq::tools::skinc::runCli(argc, argv, stdout, stderr);
}
