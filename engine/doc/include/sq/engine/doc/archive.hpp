// Squizzy Studio engine/doc - XmlArchive: the document container.
//
// A ZIP (miniz) holding one XML part per editor plus a metainfo part. Entry order is fixed and
// every entry is stamped with the same DOS date (1980-01-01 00:00), so saving the same model twice
// yields identical bytes on any machine.
//
//   metainfo.xml          <MetaInfo format="squizzy.document" version="1" kind=... docId=... .../>
//   document.xml          document properties
//   <kind>/tempo.xml      tempo + signature map          (<kind> = song | project | show)
//   <kind>/tracks.xml     tracks (+ automation) with their clips (+ MIDI events)
//   <kind>/markers.xml    markers
//   uistate.xml           journals (UI state)
//   anything else         kept verbatim (parts written by other editors / newer versions)
//
// Doubles are written in shortest round-trip form, so reopen + save is byte-stable.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "sq/engine/doc/model.hpp"

namespace sq::edoc {

inline constexpr const char* kArchiveFormat = "squizzy.document";
inline constexpr int kArchiveVersion = 1;

const char* kindName(int32_t kind);       // "song" | "project" | "show"
int32_t kindFromName(const std::string&);  // 0 if unknown
const char* kindExtension(int32_t kind);  // ".sqsong" | ".sqproject" | ".sqshow"

struct ArchiveMeta {
  std::map<std::string, std::string> extra;  // extra <MetaInfo> attributes (e.g. "originalPath")
};

// Serialized parts in archive order (name -> XML text). metainfo excluded.
std::vector<std::pair<std::string, std::string>> serializeParts(const Store& s);
// Full container bytes.
std::string writeArchive(const Store& s, const ArchiveMeta& meta = {});
// Parses container bytes into `out` (replaces it). err (optional) gets a message.
sq_result readArchive(const std::string& bytes, Store& out, ArchiveMeta* meta = nullptr, std::string* err = nullptr);

// Crash-safe file write: temp file in the same folder, flush, verify, rename over the target.
sq_result safeWriteFile(const std::string& path, const std::string& data);

}  // namespace sq::edoc
