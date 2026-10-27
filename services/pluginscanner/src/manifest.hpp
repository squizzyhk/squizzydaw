// pluginscanner: manifest records (sq_plugin_desc <-> JSON with the same field names) and the
// scanner-process protocol shared by the service and the sq-pluginscan tool.
//
// Scanner protocol (one file per process, so a crashing plugin only kills that process):
//   sq-pluginscan --module <format module path> --class <format class id> --out <result.json> <plugin file>
// The tool loads ONLY the format module through the real module loader, calls
// IPluginFormat::scanFile and writes {"version":1,"plugins":[desc...]} atomically to --out.
// Exit code 0 = scanned (possibly zero plugins), anything else = failure. A missing output file
// after the process ended (crash) or a timeout is recorded by the service as CRASHED / TIMEOUT.
#pragma once

#include <string>
#include <vector>

#include "sq/interfaces/pluginhost.h"
#include "sq/platform.hpp"

namespace sq::pscan {

inline constexpr int kManifestVersion = 1;

sq_json* descToJson(const sq_plugin_desc& d);
bool descFromJson(const sq_json* j, sq_plugin_desc& out);

// {"version":1,"plugins":[...], "blocked":["fmt:id", ...]}
std::string writeManifest(const std::vector<sq_plugin_desc>& plugins, const std::vector<std::string>& blocked);
bool readManifest(std::string_view json, std::vector<sq_plugin_desc>& plugins, std::vector<std::string>& blocked,
                  std::string* error = nullptr);

void copyField(char* dst, size_t cap, const std::string& s);

}  // namespace sq::pscan
