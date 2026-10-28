# Squizzy Studio

A cross-platform, recording-first DAW built from scratch. Windows comes first. The architecture is in [`REBUILD-PLAN.md`](REBUILD-PLAN.md), the rules for contributors and agents are in [`docs/CONTRACTS.md`](docs/CONTRACTS.md), and what is actually done (per plan section and milestone) is in [`STATUS.md`](STATUS.md).

<<<<<<< HEAD
**Status:** integrated on Windows (second integration pass): a thin shell EXE over versioned service DLLs (two-export, GUID-addressed, COM-style factory ABI with no COM dependency), an in-process engine (document model with transactions/undo, edit tools, realtime audio graph with PDC, recording, offline render), plugin hosting (CLAP + VST3 + out-of-process scanner), WASAPI audio and an ASIO seam, codecs (incl. Ogg Vorbis write), a SQLite content index, JavaScript scripting with binary packages, a skin-XML GUI with compiled themes (`theme://`, @2x, dark/light) and a UI Automation bridge, MIDI control surfaces, OSC/mDNS, and the optional cloud, tempo-sync and video services. Skia/Yoga are not used (own layout + software renderer with stb_truetype text, presented through Win32 or X11), and ASIO streaming needs the separately licensed SDK. CI builds and tests Windows, Linux and macOS on every push, and the Windows job ships a self-contained `SquizzyStudio-windows-x64` package (app-local VC++ runtime) that is run headless and as a real window; see `STATUS.md`.
=======
**Status:** integrated on Windows (second integration pass): a thin shell EXE over versioned service DLLs (two-export, GUID-addressed, COM-style factory ABI with no COM dependency), an in-process engine (document model with transactions/undo, edit tools, realtime audio graph with PDC, recording, offline render), plugin hosting (CLAP + VST3 + out-of-process scanner), WASAPI audio and an ASIO seam, codecs (incl. Ogg Vorbis write), a SQLite content index, JavaScript scripting with binary packages, a skin-XML GUI with compiled themes (`theme://`, @2x, dark/light) and a UI Automation bridge, MIDI control surfaces, OSC/mDNS, and the optional cloud, tempo-sync and video services. Skia/Yoga are not used (own layout + GDI backend), ASIO streaming needs the separately licensed SDK, and macOS is unverified; the Linux build and the full non-GUI test suite are verified (see `STATUS.md`).
>>>>>>> origin/feature/core-session-import

## Build (Windows)

You need Visual Studio 2022 with the C++ workload. The wrapper loads `vcvars64`, so nothing has to be on PATH.

```powershell
.\build.ps1                                   # Debug -> build\windows-debug, build + run all tests
.\build.ps1 -Preset windows-release           # RelWithDebInfo
.\build.ps1 -BuildDir build\mine -Jobs 4      # private build dir, capped parallelism
.\build.ps1 -CMakeArgs "-DSQ_WARNINGS_AS_ERRORS=ON"          # what CI uses
.\build.ps1 -CMakeArgs "-DSQ_ONLY=engine/doc;services/codec-wav"  # build only some components (+ core)
.\build.ps1 -ExcludeLabel gui                 # skip the window smoke test (no desktop session)
build.bat -NoTest                             # same wrapper from cmd
```

Several `-CMakeArgs` may go in one string (`"-DA=1 -DB=2"`). From bash, quote Windows paths (`-BuildDir 'build\x'`).

Optional, user-supplied components (never vendored, loaded at runtime): LAME for MP3 encoding (setting `Services.codec-mp3.LameLibrary` or env `SQ_LAME_LIBRARY`), libopusfile for Opus decoding, ONNX Runtime for stem separation, an LGPL ffmpeg shared build for the video engine (`-DSQ_FFMPEG_DIR=`).

SDKs that need a licence agreement are seams: the tree builds and runs without them. ASIO: `-DSQ_ASIO_SDK_DIR=<Steinberg ASIO SDK>` compiles the ASIO host into `services/audiodriver-asio` (without it the driver loads, lists 0 devices and `Audio.ListDevices` says why). Ableton Link: `-DSQ_LINK_SDK_DIR=<Link SDK>` (without it `services/link-ableton` runs a local-only tempo clock). Neither SDK path has been compiled in this tree's CI.

<<<<<<< HEAD
Linux and macOS: `cmake --preset linux` (or `macos`), then `cmake --build --preset linux` and `ctest --preset linux`. Both build and pass the test suite in CI; on Linux the window uses X11 and audio goes through ALSA or PulseAudio. macOS has no native window or audio driver yet (headless and batch modes only).
=======
Linux (verified on Ubuntu 24.04 with GCC 13 + Ninja):

```bash
cmake --preset linux
cmake --build --preset linux --parallel
ctest --preset linux -LE gui --output-on-failure
```

The Linux build has no window backend and no hardware audio driver yet: the app runs headless and offline rendering works, while the GUI and live device I/O remain Windows-only. macOS has the same `#ifdef` seams but its build is **unverified**.
>>>>>>> origin/feature/core-session-import

## Run

```text
build\windows-debug\bin\SquizzyStudio.exe       # the window (arrange + mixer from the built-in skin)
SquizzyStudio.exe --headless                    # boot, load services, print a summary
SquizzyStudio.exe --list-commands               # everything the command system can do
SquizzyStudio.exe --audio-driver none --run File.New --run "Track.AddAudio name=Vox" --run "File.Save path=C:/tmp/a.song"
SquizzyStudio.exe --selftest | --list-classes | --reload-test 25 | --new-guid 3 | --version
```

Options: `--audio-driver none|null|offline|wasapi|asio|alsa|pulse` (default: WASAPI for the window on Windows, ALSA then Pulse then null on Linux, none for batch modes; also `Devices.Audio.Driver` / `Devices.Audio.Device`), `--settings FILE`, `--services-dir DIR`, `--log-level LEVEL`, `--quit-after MS`.

GUI: menu bar, transport, browser (files + built-in devices), arrange view (clips, automation lanes, loop/locators, drag/resize/split), piano-roll and audio clip editors, mixer (faders, pan, mute/solo/arm/monitor, inserts, sends, buses), device chain with 18 built-in instruments and effects, file/folder choosers and prompts. Per-monitor DPI aware on Windows. Headless rendering for tests: `--screenshot out.png [--size WxH --scale N]` and `--ui-script file.uiscript`.

Window keys: Space play/stop, R record, S split at cursor, L loop, Q quantize, G glue, Delete delete, arrows cursor/track, Ctrl+arrows nudge, Tab next clip, Ctrl+N/S/Z/Y/D/A new/save/undo/redo/duplicate/select all, Home go to start, F5 reload every service module, Esc quit. Every button, key and fader goes through the same command system (`Shell::execute`) the CLI and scripts use.

Commands (excerpt; `--list-commands` prints all, including script commands): `File.New/Open/Save/Close/ImportAudio/ImportMidi/ExportAudio/ExportStems/ExportMidi/ExportDawProject/ImportDawProject`, `Transport.Play/Stop/Record/Rewind/ToggleLoop`, `Edit.Undo/Redo` + the engine/edit tools (`Edit.Split`, `Edit.Quantize`, `Edit.StripSilence`, ... `Navigate.*`), `Track.AddAudio/AddInstrument/ToggleArm/SetInput/SetVolume/SetPan/ToggleMute/ToggleSolo/Rename/InsertPlugin`, `Plugins.Rescan`, `Browser.IndexFolder/Search`, `Script.Evaluate/LoadPackage`, `Audio.ListDevices/SelectDevice`, `UI.SetTheme`, `Surface.List/LoadMap/LoadRemotePages/SelectPage`, `Osc.Listen/Stop/Send`, `Sync.Link`, `Account.Status/SignIn/SignOut`, `Video.Probe`.

A complete session can be built from the command line, e.g. `File.New`, `Track.AddAudio`, `File.ImportAudio path=take.wav`, `Track.SetVolume value=-6`, `File.ExportAudio path=mix.wav`. `File.ImportAudio` reads the real media duration, converts it to the song's tempo map, and is one undo step; media at any sample rate is resampled to the graph rate. `File.ImportMidi` turns each Standard MIDI File track into an instrument track with a note clip.

Themes: the stock themes (`ui/themes`: base, dark, light) are validated and packed by `sq-skinc` (`tools/skin-compiler`) and embedded in the EXE; `UI.Theme` (or `UI.SetTheme name=light`) switches them. OSC: `Osc.Listen port=9000` accepts `/sq/<Category>/<Name> [value]` for transport, navigation and undo/redo only, bound to 127.0.0.1 unless `Services.hardware-remote.OscBindAddress` says otherwise. MIDI control surfaces: `Surface.LoadMap path=<map>.surface.xml` (examples in `services/hardware-surface/maps/`), then `Surface.LoadRemotePages surface=<id>`.

Tools: `sq-pluginscan` (out-of-process scanner child), `sq-skinc` (theme/skin compiler), `sq-pack` (build/verify/unpack `.package` files).

## Layout

| Dir | What |
|---|---|
| `sdk/` | Public C ABI: GUIDs, refcounted interfaces, the module/factory ABI, shared media PODs, area interface headers (`sq/interfaces/*.h`) and header-only C++ helpers |
| `platform/` | `sqplatform` DLL: threads and pool, IPC, filesystem, UTF-8/16, XML and JSON DOMs, CRC32/SHA-256, logging |
| `app/` | `SquizzyStudio.exe`; `sqappcore` (module loader with ABI gate and hot reload, class registry, settings, host context); `sqshell` (engine services + the command system, shared with the end-to-end tests); the built-in skin |
| `engine/` | In-process units: `doc` (document model, XmlArchive, transactions/undo, side effects, autosave), `edit` (edit tools + keyboard commands), `audio` (graph, render thread, fast lane, PDC, recording, renders, plugin inserts), `simd` (vector ops, FFT) |
| `formats/` | `core` (packages, presets, langpacks, extensions, ...), `dawproject` (DAWproject export/import) |
| `services/` | One DLL per subsystem: drivers (WASAPI, ASIO seam, MIDI), codecs (WAV/AIFF, FLAC, MP3, OGG, AAC, MIDI file), CLAP + VST3 hosts, plugin scanner, SQLite index, JS scripting, analysis (beat/tuner, chords, stems), time-stretch, control surfaces (`hardware-surface`), OSC/mDNS (`hardware-remote`), tempo sync (`link-ableton`), `cloud-client`, `video-engine`, `dummy` |
| `ui/` | Backend-neutral window/renderer API, own flexbox layout, retained widgets, skin XML, themes (`theme://`, packs, @2x), PNG preflight, accessibility tree + UI Automation bridge, remote pages, Win32/GDI backend, portable raster backend |
| `tools/` | `packager` (`sq-pack`), `skin-compiler` (`sq-skinc`) |
| `tests/` | `sqtest` framework, core tests, `e2e/` milestone tests |
| `third_party/` | Vendored permissive libraries (see `third_party/README.md`) |
| `ci/` | ABI stamping and guard (`stamp-abi.cmake`); workflow in `.github/workflows/ci.yml` |

## Tests

`ctest` in a build dir runs every unit's tests plus `e2e_milestones`, which drives the real shell with all service modules loaded to prove the plan exit criteria together (save/reopen, 2-track record at 64 frames, keyboard-only editing with exact undo, 1000-item browser search, crash-proof plugin scan + a gain plugin on a track, a user script menu command with undo, WAV/FLAC/MIDI/DAWproject export and reimport, a VST3 insert with exact PDC, theme switching, remote pages, and OSC driving the command system). Cases that need absent hardware or SDKs exit 77 and are reported **Skipped** by CTest, never passed.

## License and provenance

All code here is original. Third-party components and their licenses are listed in `third_party/README.md`.
