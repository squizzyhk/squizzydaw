// Squizzy Studio skin compiler (sq-skinc) as a library: validates theme folders (theme.xml,
// skin XML incl. defines / if / switch / include / remote pages, images and their @Nx variants),
// resolves every theme:// asset reference, and packs everything into a deterministic theme pack
// (sq/ui/theme.hpp, "SQTHMPK1") plus a Windows .rc that embeds it as RCDATA, a C header and a
// JSON manifest.
//
// Inputs are theme folders (<dir>/theme.xml) or folders of themes (<dir>/<name>/theme.xml).
// Determinism: the pack depends only on the set of (path, bytes) pairs - not on input order,
// file system enumeration order, timestamps or the machine. The .rc/.h/.json are derived from it.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace sq::tools::skinc {

struct Options {
  std::vector<std::string> inputs;
  std::string out;       // pack file (optional with validateOnly)
  std::string rc;        // Windows resource script embedding `out`
  std::string header;    // C header: resource name, size, sha256
  std::string manifest;  // JSON listing
  std::string resourceName = "SQTHEMES";
  std::vector<std::string> customElements;  // extra skin elements the host registers
  bool validateOnly = false;
  bool strict = false;  // warnings fail the run
};

struct Diagnostic {
  bool error = true;
  std::string file;  // pack path ("dark/remote/mixer.skin.xml") or input path
  std::string message;
};

struct Result {
  bool ok = false;
  std::vector<Diagnostic> diagnostics;
  std::vector<std::string> themes;  // sorted
  size_t files = 0;
  std::string pack;    // the pack bytes (empty when validation failed)
  std::string sha256;  // hex of `pack`
  size_t errors() const;
  size_t warnings() const;
};

// Validates and packs; writes the requested outputs only when the run succeeds.
Result compile(const Options& options);

// Command line front end (sq-skinc). Returns the process exit code: 0 ok, 1 validation failed,
// 2 usage / IO error. Diagnostics go to `err`, the summary to `out`.
int runCli(int argc, char** argv, std::FILE* out, std::FILE* err);

}  // namespace sq::tools::skinc
