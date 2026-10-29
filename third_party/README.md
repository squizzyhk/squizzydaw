# third_party/ policy

M0 uses **no** third-party code. The test framework (`tests/framework/sqtest.hpp`), the XML and JSON DOMs, the hashing code and the UTF codecs are all written for this project.

When you need a dependency:

1. **One subdirectory per library**: `third_party/<name>/` with a `VERSION` file. Keep it unmodified upstream source. If you must patch it, put the patches in `third_party/<name>/PATCHES.md` and explain each one. The root CMakeLists adds every `third_party/*/CMakeLists.txt` with `EXCLUDE_FROM_ALL`, so a library compiles only when something links it.
2. **License file required.** Copy the upstream `LICENSE`/`COPYING` into that folder, and add a row to the table below in the same change.
3. **Permissive licenses only for vendored source**: MIT, BSD-2/3, Apache-2.0, zlib, ISC, Boost. LGPL components (mpg123, LAME, an ffmpeg LGPL build) are never vendored as source into our targets. They are loaded dynamically by their own service DLL (plan §3.2). GPL/commercial items (VST3 proprietary path, élastique, Link) each sit behind their own service module and are added only after a license decision.
4. **Small only.** The dev disk is tight. No Skia, Chromium or full ffmpeg source trees in the repo. Large SDKs are fetched at configure time into `build/_deps` (CMake `FetchContent` with a pinned hash) or come from the system, behind an `option(SQ_WITH_<NAME>)` that defaults OFF until CI has the dependency cached.
5. **Wire it up locally.** Give the library its own `CMakeLists.txt` that defines a namespaced target (`tp::<name>`), and link it **only** from the service or component that needs it. Never link a third-party library into `sqplatform`, the SDK headers or the app shell without an architecture decision.
6. **No third-party product assets.** No names, GUIDs, presets, skins, scripts or text from other DAWs, ever (plan §0).

| Library | Version | License | Used by | Added |
|---|---|---|---|---|
| miniz (`third_party/miniz`, target `tp::miniz`) | 3.0.2 | MIT | shared: formats/core (.package/.preset/.langpack ZIP), formats/dawproject, engine/doc (XmlArchive), scripting-js, sqlite-index | 2026-10-27 |
| sqlite (`third_party/sqlite`, target `tp::sqlite`) | 3.50.4 | Public domain | services/sqlite-index (FTS5 enabled) | 2026-10-27 |
| quickjs-ng (`third_party/quickjs-ng`, target `tp::quickjs`) | 0.16.2 | MIT | services/scripting-js (unmodified amalgamation; quickjs-libc not compiled) | 2026-10-27 |
| dr_flac (`third_party/dr_flac`) | 0.13.4 (dr_libs a466dbe8) | Public domain / MIT-0 | services/codec-flac | 2026-10-27 |
| dr_mp3 (`third_party/dr_mp3`) | 0.7.4 (dr_libs a466dbe8) | Public domain / MIT-0 | services/codec-mp3 | 2026-10-27 |
| stb_vorbis (`third_party/stb_vorbis`, target `tp::stb_vorbis`) | 1.22 (nothings/stb 003501f6) | MIT / public domain | **nothing** (codec-ogg moved to libvorbisfile); kept for a possible lightweight decoder, can be removed | 2026-10-27 |
| libogg (`third_party/libogg`, target `tp::libogg`) | 1.3.5 (subset) | BSD-3-Clause | services/codec-ogg; `config_types.h` added (PATCHES.md) | 2026-10-28 |
| libvorbis (`third_party/libvorbis`, target `tp::libvorbis`) | 1.3.7 (libvorbis + vorbisenc + vorbisfile subset) | BSD-3-Clause | services/codec-ogg (Ogg Vorbis read + write); one local patch in `lib/block.c` (PATCHES.md) | 2026-10-28 |
| onnxruntime-headers (`third_party/onnxruntime-headers`) | 1.17.3 | MIT | services/analysis-stems (C API header only; the runtime is loaded dynamically if present, never linked) | 2026-10-27 |
| clap (`third_party/clap`, target `tp::clap`) | 1.2.10 | MIT | services/pluginhost-clap (headers only) | 2026-10-27 |
| vst3 pluginterfaces (`third_party/vst3`, target `tp::vst3`) | 3.8.1 build 84 (subset) | MIT (VST SDK 3.8+) | services/pluginhost-vst3 (pluginterfaces only; no SDK helper code) and its test plugin | 2026-10-27 |

LGPL / user-supplied, never vendored: LAME (`libmp3lame`, codec-mp3 encoder, path via `SQ_LAME_LIBRARY`), libopusfile (codec-ogg Opus), ONNX Runtime (analysis-stems), Media Foundation (OS, codec-aac, video-engine), ffmpeg LGPL shared build (video-engine, `SQ_FFMPEG_DIR`).

Licence-agreement SDKs, never vendored, seams only (each builds and loads without the SDK): Steinberg ASIO SDK (`services/audiodriver-asio`, `-DSQ_ASIO_SDK_DIR=` or the user-supplied ZIP in `asio sdk/`; without either the driver reports 0 devices and says why through `IAsioDriverStatus`), Ableton Link SDK (`services/link-ableton`, `-DSQ_LINK_SDK_DIR=`; without it a local-only tempo clock).
