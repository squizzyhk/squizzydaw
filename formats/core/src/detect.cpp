#include "sq/formats/core/detect.hpp"

#include "sq/formats/core/extension.hpp"
#include "sq/formats/core/package.hpp"
#include "sq/formats/core/snapshots.hpp"
#include "sq/formats/core/zip.hpp"
#include "sq/platform.hpp"

namespace sq::formats {

const char* fileFormatName(FileFormat f) {
  switch (f) {
    case FileFormat::ScriptPackage: return "script-package";
    case FileFormat::SnapshotPackage: return "snapshot-package";
    case FileFormat::LangPack: return "langpack";
    case FileFormat::Preset: return "preset";
    case FileFormat::MultiPreset: return "multipreset";
    case FileFormat::Instrument: return "instrument";
    case FileFormat::TrackPreset: return "trackpreset";
    case FileFormat::DspPreset: return "dsppreset";
    case FileFormat::Chords: return "chords";
    case FileFormat::MusicalScale: return "musicalscale";
    case FileFormat::ColorScheme: return "colorscheme";
    case FileFormat::Extension: return "extension";
    default: return "unknown";
  }
}

FileFormat detectFormat(std::string_view path, std::string_view bytes) {
  if (packageProbe(bytes)) return FileFormat::ScriptPackage;
  std::string ext = fileExtension(path);
  if (ext == "package") {
    if (!zipProbe(bytes)) return FileFormat::Unknown;
    std::vector<std::string> names;  // detection reads the directory only: nothing is inflated
    if (zipListNames(bytes, names, ZipLimits::metadata().maxEntries))
      for (const auto& n : names)
        if (n == kSnapshotsManifest) return FileFormat::SnapshotPackage;
    return FileFormat::Unknown;
  }
  bool zip = zipProbe(bytes);
  if (ext == "langpack" && zip) return FileFormat::LangPack;
  if (ext == "preset" && zip) return FileFormat::Preset;
  if (ext == "multipreset" && zip) return FileFormat::MultiPreset;
  if (ext == "instrument" && zip) return FileFormat::Instrument;
  if (ext == "trackpreset" && zip) return FileFormat::TrackPreset;
  if (ext == "dsppreset") return FileFormat::DspPreset;
  if (ext == "chords") return FileFormat::Chords;
  if (ext == "musicalscale") return FileFormat::MusicalScale;
  if (ext == "colorscheme") return FileFormat::ColorScheme;
  return FileFormat::Unknown;
}

bool isExtensionDirectory(const std::string& dir) {
  return sq_fs_is_file(joinPath(dir, kMetaInfoFile).c_str()) && sq_fs_is_file(joinPath(dir, kInstallDataFile).c_str());
}

}  // namespace sq::formats
