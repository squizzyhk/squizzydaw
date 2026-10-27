// Squizzy Studio sq-pack: build / unpack / inspect / verify every content container of
// formats/core (script + skin .package, snapshot .package, .langpack, the preset kinds,
// .dsppreset, plain-XML data) and manage extensions (installdata ABI gate).
//
// Determinism: every build sorts its inputs, uses the formats/core writers (fixed ZIP timestamps)
// and canonicalises through read -> write, so the same input tree gives the same bytes, and
// unpack -> build reproduces a container byte for byte.
//
// Directory layouts accepted by `build` (and produced by `unpack`):
//   script / skin  : any tree (bundle payload); a script tree needs package.xml (or one top folder
//                    holding it). A single .xml / .js file builds an Xml / JavaScript payload.
//   langpack       : metainfo.xml, catalogs/<module>.mo or .po, properties/<name>.properties
//   preset kinds   : metainfo.xml, preset.xml, slots/<i>.state, attachments/...
//   snapshot       : snapshots.json + the images it names; without snapshots.json every .png /
//                    .webp becomes a snapshot (id = path without extension; scale 2 for "@2x"
//                    names), with --info key=value entries.
// In-process C++ only; not thread-safe; never on the audio thread.
#pragma once

#include <string>
#include <vector>

#include "sq/formats/core/all.hpp"

namespace sq::packager {

using sq::formats::Status;

// Exit codes of the CLI (runCli / sq-pack).
inline constexpr int kExitOk = 0;
inline constexpr int kExitInvalid = 1;      // unreadable / malformed / failed verification
inline constexpr int kExitUsage = 2;
inline constexpr int kExitAbiMismatch = 3;  // installdata / package.xml ABI range excludes the host
inline constexpr int kExitUnsupported = 4;  // other application or platform

enum class BuildType { Script, Skin, Snapshot, LangPack, Preset, MultiPreset, Instrument, TrackPreset };
bool buildTypeFromName(const std::string& s, BuildType& out);
const char* buildTypeName(BuildType t);
const char* buildTypeExtension(BuildType t);  // "package", "langpack", "preset", ...

struct BuildOptions {
  sq::formats::KeyValues info;  // snapshot: package info entries (--info key=value)
};

// Builds a container from a directory (or, for script/skin, a single .xml/.js file).
Status build(BuildType type, const std::string& src, std::string& bytes, const BuildOptions& opt = {});
// Writes the container's members into outDir (layout accepted by build). poCatalogs: langpack
// catalogs as .po text instead of .mo. *written: relative paths written (sorted).
Status unpack(const std::string& file, const std::string& outDir, bool poCatalogs = false,
              std::vector<std::string>* written = nullptr);

struct VerifyOptions {
  bool canonical = false;  // also require read -> write to reproduce the exact bytes
  bool gate = true;        // apply the ABI / app / platform gate (extensions, package.xml abi)
  sq::formats::HostIdentity host = sq::formats::HostIdentity::current();
};
// Human-readable report (stable "key: value" lines). verify fills it too.
Status inspect(const std::string& path, std::string& report);
// Full parse + structural checks (+ gate, + canonical round trip). The Status code tells why:
// SQ_E_ABI_MISMATCH / SQ_E_UNSUPPORTED for gate failures, SQ_E_PARSE etc. for broken content.
Status verify(const std::string& path, const VerifyOptions& opt, std::string& report);

// Writes metainfo.xml + installdata.xml for an extension directory (creates it).
struct ExtensionInit {
  sq::formats::MetaInfo meta;  // Package.Id and Package.Version required
  sq::formats::InstallData install;
};
Status extensionInit(const std::string& dir, const ExtensionInit& init);

// The CLI. args excludes the program name. Output goes to out/err (the tool prints them).
int runCli(const std::vector<std::string>& args, std::string& out, std::string& err);
std::string usage();

}  // namespace sq::packager
