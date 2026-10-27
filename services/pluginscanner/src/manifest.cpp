// pluginscanner: manifest JSON (see manifest.hpp).
#include "manifest.hpp"

#include <cstring>

namespace sq::pscan {

void copyField(char* dst, size_t cap, const std::string& s) {
  if (!dst || cap == 0) return;
  size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
  // Never cut a UTF-8 sequence in half.
  while (n > 0 && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
  std::memcpy(dst, s.data(), n);
  dst[n] = 0;
}

namespace {
void putStr(sq_json* o, const char* k, const char* v) { sq_json_set(o, k, sq_json_new_string(v, static_cast<size_t>(-1))); }
void putNum(sq_json* o, const char* k, double v) { sq_json_set(o, k, sq_json_new_number(v)); }
std::string getStr(const sq_json* o, const char* k) {
  const sq_json* v = sq_json_get(o, k);
  if (!v || sq_json_type_of(v) != SQ_JSON_STRING) return {};
  size_t n = 0;
  const char* s = sq_json_get_string(v, &n);
  return std::string(s, n);
}
int64_t getInt(const sq_json* o, const char* k, int64_t def = 0) {
  const sq_json* v = sq_json_get(o, k);
  return v && sq_json_type_of(v) == SQ_JSON_NUMBER ? sq_json_get_int(v) : def;
}
}  // namespace

sq_json* descToJson(const sq_plugin_desc& d) {
  sq_json* o = sq_json_new_object();
  putStr(o, "format", d.format);
  putStr(o, "id", d.id);
  putStr(o, "name", d.name);
  putStr(o, "vendor", d.vendor);
  putStr(o, "version", d.version);
  putStr(o, "category", d.category);
  putStr(o, "path", d.path);
  putNum(o, "sub_index", d.sub_index);
  putNum(o, "flags", d.flags);
  putNum(o, "audio_inputs", d.audio_inputs);
  putNum(o, "audio_outputs", d.audio_outputs);
  putNum(o, "aux_inputs", d.aux_inputs);
  putNum(o, "midi_inputs", d.midi_inputs);
  putNum(o, "midi_outputs", d.midi_outputs);
  putNum(o, "status", d.status);
  putNum(o, "file_mtime", static_cast<double>(d.file_mtime));
  putNum(o, "file_size", static_cast<double>(d.file_size));
  return o;
}

bool descFromJson(const sq_json* j, sq_plugin_desc& d) {
  if (!j || sq_json_type_of(j) != SQ_JSON_OBJECT) return false;
  d = sq_plugin_desc{};
  d.struct_size = sizeof d;
  copyField(d.format, sizeof d.format, getStr(j, "format"));
  copyField(d.id, sizeof d.id, getStr(j, "id"));
  copyField(d.name, sizeof d.name, getStr(j, "name"));
  copyField(d.vendor, sizeof d.vendor, getStr(j, "vendor"));
  copyField(d.version, sizeof d.version, getStr(j, "version"));
  copyField(d.category, sizeof d.category, getStr(j, "category"));
  copyField(d.path, sizeof d.path, getStr(j, "path"));
  d.sub_index = static_cast<uint32_t>(getInt(j, "sub_index"));
  d.flags = static_cast<uint32_t>(getInt(j, "flags"));
  d.audio_inputs = static_cast<uint32_t>(getInt(j, "audio_inputs"));
  d.audio_outputs = static_cast<uint32_t>(getInt(j, "audio_outputs"));
  d.aux_inputs = static_cast<uint32_t>(getInt(j, "aux_inputs"));
  d.midi_inputs = static_cast<uint32_t>(getInt(j, "midi_inputs"));
  d.midi_outputs = static_cast<uint32_t>(getInt(j, "midi_outputs"));
  d.status = static_cast<int32_t>(getInt(j, "status", SQ_PLUGIN_STATUS_INVALID));
  d.file_mtime = getInt(j, "file_mtime");
  d.file_size = getInt(j, "file_size");
  return d.format[0] != 0 && d.path[0] != 0;
}

std::string writeManifest(const std::vector<sq_plugin_desc>& plugins, const std::vector<std::string>& blocked) {
  JsonPtr root(sq_json_new_object());
  putNum(root.get(), "version", kManifestVersion);
  sq_json* arr = sq_json_new_array();
  for (const auto& d : plugins) sq_json_append(arr, descToJson(d));
  sq_json_set(root.get(), "plugins", arr);
  sq_json* bl = sq_json_new_array();
  for (const auto& b : blocked) sq_json_append(bl, sq_json_new_string(b.c_str(), b.size()));
  sq_json_set(root.get(), "blocked", bl);
  return jsonWrite(root.get(), true);
}

bool readManifest(std::string_view json, std::vector<sq_plugin_desc>& plugins, std::vector<std::string>& blocked, std::string* error) {
  std::string err;
  JsonPtr root = jsonParse(json, &err);
  if (!root || sq_json_type_of(root.get()) != SQ_JSON_OBJECT) {
    if (error) *error = err.empty() ? "manifest is not a JSON object" : err;
    return false;
  }
  if (getInt(root.get(), "version", 0) != kManifestVersion) {
    if (error) *error = "unsupported manifest version";
    return false;
  }
  plugins.clear();
  blocked.clear();
  if (const sq_json* arr = sq_json_get(root.get(), "plugins"); arr && sq_json_type_of(arr) == SQ_JSON_ARRAY)
    for (size_t i = 0; i < sq_json_size(arr); ++i) {
      sq_plugin_desc d{};
      if (descFromJson(sq_json_at(arr, i), d)) plugins.push_back(d);
    }
  if (const sq_json* arr = sq_json_get(root.get(), "blocked"); arr && sq_json_type_of(arr) == SQ_JSON_ARRAY)
    for (size_t i = 0; i < sq_json_size(arr); ++i) {
      size_t n = 0;
      const char* s = sq_json_get_string(sq_json_at(arr, i), &n);
      if (n) blocked.emplace_back(s, n);
    }
  return true;
}

}  // namespace sq::pscan
