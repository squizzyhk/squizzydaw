// Squizzy Studio formats - identify a content file from its name and leading bytes.
#pragma once

#include <string>
#include <string_view>

#include "sq/formats/core/common.hpp"

namespace sq::formats {

enum class FileFormat {
  Unknown,
  ScriptPackage,    // .package with the binary header (script/skin)
  SnapshotPackage,  // .package ZIP with snapshots.json
  LangPack,
  Preset,
  MultiPreset,
  Instrument,
  TrackPreset,
  DspPreset,
  Chords,
  MusicalScale,
  ColorScheme,
  Extension,        // a directory (detectDirectory)
};

const char* fileFormatName(FileFormat f);
// path: used for the extension; bytes: the whole file (or at least the head for non-.package).
FileFormat detectFormat(std::string_view path, std::string_view bytes);
// A directory is an extension if it holds metainfo.xml and installdata.xml.
bool isExtensionDirectory(const std::string& dir);

}  // namespace sq::formats
