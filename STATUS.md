# Squizzy Studio - status matrix

## Third pass: complete DAW (2026-10-28) - read this first

This pass turned the service platform into a usable DAW. The tables further down are from the second pass; where they disagree with this section, this section wins.

- **GUI:** software renderer (`ui/src/soft_renderer.cpp`, stb_truetype + Inter) presented by Win32 (`SetDIBitsToDevice`, per-monitor-v2 DPI, `WM_DPICHANGED`) and X11. Panels (`app/src/gui_*.cpp`): menu bar, transport, browser + device browser, arrange (clips, automation lanes, loop/locators), MIDI and audio editors, mixer (inserts, sends, buses, master), device chain. Overlays: menus with submenus, prompts, confirmations, file/folder choosers. Headless `--screenshot` / `--ui-script`; CTests `app_gui_smoke` and `app_gui_smoke_hidpi`. Two QA rounds (52 GUI defects, 31 engine findings) fixed.
- **Engine:** 18 built-in devices (instruments, drums, effects) via `builtin|<id>|<path>|i=v` specs; sample-rate conversion of media (`engine/audio/src/resample.cpp`); master mute/pan, bus solo, tempo-synced delay; automation of volume, pan, sends and insert parameters; loop recording (new pass detection by counter, held notes split at the wrap).
- **Persistence:** relative media paths with a missing-media warning, recorded takes copied out of temp into `<song> Media/` on save, loop range + on/off saved, gain automation clamped to -144..+24 dB on load, default export tail rendered until the output falls below -90 dBFS (max 10 s). DAWproject export now carries buses, sends, routing, devices and automation.
- **Commands:** ~40 new (`Track.*` inserts/sends/instruments, `Clip.*`, `Automation.*`, `Song.*`, `Midi.*`, `Transport.Locate/SetLoop/Wait`); the GUI edits inserts and sends through them so automation lanes follow their slots.
- **Linux:** ALSA + PulseAudio audio, ALSA sequencer MIDI, X11 window.
- **CI:** Windows, Linux, macOS build + full ctest. The Windows job packages `SquizzyStudio-windows-x64`, runs it headless (screenshot + non-silent mixdown export) and opens the real window.
- **Still open:** hands-on testing on a physical Windows 11 machine (WASAPI devices, 125-200% scaling, monitor moves); macOS native window/audio; the gaps listed under "Known gaps" that this pass did not touch (ASIO/Link SDKs, plugin editors, ARA, video tracks).

## Second pass

Snapshot after the second integration pass (wave 2: ASIO seam, VST3, themes/skin compiler/UI Automation, MIDI control surfaces, optional M8 services, sq-pack + Ogg write), each unit adversarially verified before integration (2026-10-28). Legend: **done** = implemented and covered by tests that ran green; **partial** = works with documented gaps; **seam** = interface/hook exists, the real implementation needs something not in the tree (an SDK or a user-supplied library); **not done**.

Verified platforms:
- **Windows 11 x64, MSVC 2022, Debug, `/W4 /WX`** (unfiltered build, full `ctest`: see the end).
- **Linux x64, Ubuntu 24.04, GCC 13.3, RelWithDebInfo** (unfiltered Ninja build, `ctest -LE gui`: 99 passed, 16 skipped, 0 failed, 2026-10-28). The Linux build is headless: there is still no window backend or hardware audio driver.

macOS is **unverified**: the tree has `#ifdef` seams and null backends, but nothing was built or run there.

## Plan section 2 - architecture

| Plan item | Status | Where / evidence | Notes |
|---|---|---|---|
| 2.1 Platform layer (threads, pool, IPC, fs, text, XML/JSON, hash, log, C ABI, ABI stamp) | done | `platform/`, tests `platform`, `ipc`, `xml_json`, `hash`, `abi_lock` | No process-spawn API in the platform layer yet (the plugin scanner has its own) |
| 2.2 Module system: two exports, GUID registry, ABI gate, hot reload | done | `app/`, tests `module_reload`, `module_lifecycle`, `service_exports`, `app_reload_test` | 24 modules load in the unfiltered build |
| 2.2 Script classes in the same registry | done | `services/scripting-js`, test `scripting_js`, e2e M6 | |
| 2.3 Graph, AudioProcessor variants, render thread with priority/MMCSS | done | `engine/audio`, tests `engine_audio_*` | Single-threaded graph; instrument tracks use a built-in sine synth (no plugin instruments yet). Mixer-only and cosmetic edits are RT commands on the installed graph; **structural edits (clips, inserts, routing, tempo) still recompile the graph**, which restarts PDC delay lines and plugin state (no state transfer / crossfade yet). Media at a sample rate different from the graph is linearly resampled in `MediaNode` (fixes imported 44.1 kHz files in a 48 kHz song). |
| 2.3 Device loss / reset recovery | done | `engine_audio_service` (simulated loss with and without driver notification, during recording) | Not exercised with a physically unplugged endpoint |
| 2.3 Fast-lane monitoring | partial | `engine/audio` (`processFastLane`) | Bypasses buses and master inserts by design |
| 2.3 Latency bookkeeping + PDC | done | `engine_audio_pdc`, `engine_audio_plugin_fx`, e2e M4 (CLAP) and **M4 (VST3)**: 64-frame plugin latency compensated exactly (max error 0) | |
| 2.3 Stem / mixdown / click / bounce-in-place render tasks | done | `engine_audio_render`, `engine_audio_plugin_fx`, e2e M4/M7 | Stem export command not covered end-to-end. No sample-rate conversion; media fully decoded (no disk streaming) |
| 2.3 Documents: Song / Project / Show, DocumentManager, XmlArchive | partial | `engine/doc`, tests `engine_doc_*`, e2e M1 | Project and Show share the Song model |
| 2.3 RIFF chunk vocabulary (bext, smpl, CHAN, cue) | done | `services/codec-wav`, test `codec_wav` | WAVE_FORMAT_EXTENSIBLE mask only round-trips with `chunk:CHAN` |
| 2.3 Edit model: actions, transactions, undo, side effects, journals | done | `engine/doc`, `engine/edit`, 100-action and 10k-action undo stress | |
| 2.3 Settings registry mirrored as `object://` | partial | `app/`, scripting-js `object://settings/` | The app does not register `object://app/settings/` itself (scripting-js fallback) |
| 2.4 Scripting (QuickJS), Host API, packages, bundled command packages | done | `services/scripting-js` (`scripting_js`, `scripting_js_binary_package`), e2e M6 | Loads directories, ZIPs and binary `.package` containers (Bundle payloads; Xml payloads only when they declare no classes). A package whose `ScriptClass sourceFile` is missing is refused at load (`SQ_E_PARSE`), not at first use |
| 2.5 GUI: retained widgets, flexbox layout, skin XML (forms, defines, if/switch, include) | partial | `ui/`, `ui_core`, `ui_m5_hardening`, `app_window_smoke` | **Skia and Yoga deferred**: own flexbox engine + Win32/GDI backend + portable raster backend. Skin builds are bounded (100k widgets, 1M nodes, 32 MB text, include depth 16, **widget nesting 256 levels across includes**); the skin validator is non-recursive (no stack overflow on 20000-form include chains or deep nesting) |
| 2.5 theme:// resolver, @2x assets, skin compiler, light theme | done | `ui/` ThemeResolver + ThemePack, `tools/skin-compiler` (`sq-skinc`), tests `ui_*`, `tools_skin-compiler_*`, `app_integration_commands`, e2e M5/M8 | Stock themes (base, dark, light) are validated and packed deterministically by `sq-skinc`, **embedded in `SquizzyStudio.exe`** (RCDATA) and copied to `bin/themes/`; the shell follows `UI.Theme` (`UI.SetTheme`), the GUI builds the skin with the theme's defines, uses the theme image provider and rebuilds on a switch. PNG decoding is preflighted (bounded memory, truncated PNGs rejected); **other image formats still go to WIC unchecked** (WIC may allocate the header-claimed frame size, up to ~256 MB). The arrange view still paints with hard-coded colors. The `.rc` sq-skinc writes contains an absolute path (the pack itself is byte-identical across machines) |
| 2.5 Accessibility | partial | `ui/src/uia_win32.cpp`, `ui_uia_window`, `ui_m5_hardening_uia` (gui label) | UI Automation bridge: names, roles, help text, bounds, RangeValue/Toggle/Invoke; provider calls are **marshalled to the UI thread** (message-only window, 10 s timeout). **Wired into the app GUI** (`setAccessibilityRoot`, `notifyAccessibilityChanged` on value/structure changes). Not tested with a real screen reader; the arrange view is one opaque node |
| 2.6 .package (scripts/skins, snapshots), .langpack, presets, .dsppreset, plain XML data, extensions with install gating | done | `formats/core`, `formats_core_roundtrip`, `formats_core_extension`; `tools/packager` (`sq-pack`, `tools_packager_*`) | `sq-pack build/verify/unpack`: refuses script packages the engine cannot load, enforces the engine's id rule, and `unpack` refuses case-colliding / device-name / trailing-dot members and non-empty output dirs before writing. `sf::isSafeRelativePath` itself still accepts Windows device names (other extractors must check). **Open (needs a product decision):** document/preset extensions and the scripting vocabulary follow REBUILD-PLAN verbatim and match another DAW's public vocabulary |
| 2.6 `*.tags.db` content index | done | `services/sqlite-index`, e2e M3 (1000 items, all queries < 200 ms) | |
| 2.6 MIDI control surfaces (.device / .surface.xml) | partial | `services/hardware-surface` (`hardware_surface_map`, `hardware_surface_service`), `Surface.*` commands, e2e M5/M8 | Map parser/validator, lock-free input, coalesced main-thread writes, LED/motor feedback (buttons/pads light on press; toggles follow their target without an LED), hot-plug reconnect, remote pages from the skin. **The shell pumps it** (`ISurfacePump` in `Shell::idle`). **No real hardware tested**; 14-bit CC: an MSB-only move after an LSB is dropped; whether `object://doc/active/track/N` targets resolve against the real document resolver is not covered by a test |
| 2.7 Analysis: beat, tuner, chords, stems, music theory, NN dispatcher | partial | `services/analysis-*`, tests `analysis_*` | Stems need a user-installed ONNX Runtime + model; no models ship |
| 2.8 Cloud (optional) | partial | `services/cloud-client` (`cloud_client_core`, `cloud_client_winhttp`), `Account.*` commands | OAuth device flow, entitlement cache (HMAC with a device-local key), telemetry seam; sign-out races, loopback-URL userinfo bypass and redirect following fixed by the verifier. **Server endpoints and client id are placeholders**; no real backend exists. Shutdown can wait up to the WinHTTP timeouts on a black-holed network |
| 2.9 Hardware integration, OSC, mDNS, Link (optional) | partial | `services/hardware-remote` (OSC, DNS-SD: system + built-in mDNS), `services/link-ableton`, `services/hardware-surface`; `Osc.*`, `Sync.Link` commands; e2e M5/M8 (OSC message -> command over UDP loopback) | OSC pattern matching is bounded (stack/time). Inbound OSC may only run transport/navigation/undo commands; the app binds OSC to 127.0.0.1 by default. Link: **local-only clock** unless built with `SQ_LINK_SDK_DIR` (SDK adapter not compiled by the verifier). Open: built-in mDNS has no response rate limit; the system DNS-SD backend can leak a context whose cancellation the OS never confirms (late callback after unload) |
| 2.10 SIMD dispatch table (generic / SSE4.2 / AVX2 / AVX-512) | done | `engine/simd`, tests `engine_simd_*` | Own kernels instead of Highway/xsimd; AVX-512 kernels have never executed (no AVX-512 CPU here) |

## Service roster (plan 2.2 / 3.3)

| Service | Status | Notes |
|---|---|---|
| audiodriver-wasapi | done | Shared + exclusive + capture, event driven, MMCSS. Hardware cases `audiodriver_wasapi_shared/_exclusive/_capture` each assert >= 3 callbacks and are **Skipped** (exit 77) without the hardware. Full-duplex record through WASAPI into the engine is not tested |
| audiodriver-asio | partial / seam | Without `-DSQ_ASIO_SDK_DIR` (this build) the module loads, lists **0 devices** and explains why through `IAsioDriverStatus` (`Audio.ListDevices` prints it: "3 ASIO drivers installed, support not compiled in" on this machine). Selectable via `Devices.Audio.Driver=asio` / `Audio.SelectDevice driver=asio`. Device logic, sample conversion (24 hand-written byte vectors), listener teardown safety and buffer-size sanitising are tested against a fake backend (`audiodriver_asio_*`); `audiodriver_asio_stream` is **Skipped** here. **The SDK path has never been compiled against the real SDK in this tree** |
| midi-win | partial | WinMM in/out + loopback cables, lock-free scheduled output, sysex buffers re-queued by a worker, **hot-plug watcher** (device notification + polling, simulated plug/unplug test `midi_win_hotplug`; refresh serialised). Tests use loopback only (no MIDI hardware here) |
| audiodriver-alsa (Linux) | partial | ALSA PCM via runtime `dlopen("libasound.so.2")` (no -dev headers needed; without the library: 0 devices + logged reason). Driver name "ALSA" (`--audio-driver alsa`, the Linux GUI default, then pulse, then null). Hint enumeration (default/sysdefault/pipewire/pulse/hw/plughw/hdmi/null; any other PCM name opens by id), FLOAT/S32/S24_3/S24/S16 LE conversion, playback + capture + linked full duplex on one SCHED_FIFO thread (normal priority if refused), xrun counting + restart, device-lost notification, `Services.audiodriver-alsa.Periods`. Tests: conversion vectors/round trips + streaming on ALSA's `null` PCM (playback, capture, duplex, default config: ~93 callbacks of 256 frames per 0.5 s, clock-paced). **No real sound card tested** (container has none) |
| audiodriver-pulse (Linux) | partial | PulseAudio simple API via `dlopen("libpulse-simple.so.0")`, driver name "Pulse" (`--audio-driver pulse`): `pulse:default` (or `pulse:<sink>`), float32 stereo, playback/capture/duplex. No enumeration, no xrun reporting (simple API limits). `audiodriver_pulse_stream` Skipped without a server |
| midi-alsa (Linux) | partial | ALSA sequencer via dlopen; the Linux platform MIDI driver (also registered as the host `IMidiDriver` service, which hardware-surface prefers over midi-win's loopback-only Linux build). Ports = every subscribable port of other clients (`alsa:in:/alsa:out:<client>:<port>#n`), input decoded on a reader thread (same gate/threading contract as midi-win, sysex reassembled), lock-free scheduled output, hot-plug via System Announce + polling. Codec tested with synthetic events; `midi_alsa_loopback` (Midi Through) **Skipped** here: no `/dev/snd/seq` in the container |
| pluginhost-clap | done | Test gain plugin: latency, sidechain bus, params, state |
| pluginhost-vst3 | partial | Adapter + original test plugin (latency, sidechain bus, events, MIDI-CC mapping, params incl. non-linear, component/controller connection, state). Verifier fixed 7 defects (destructor callbacks, >512-param bursts lost, NaN automation, wrapped frame offsets, stale buses after kIoChanged, malformed descs, unknown param ids). Scanned out of process and inserted on a track by e2e **M4 (VST3)** with exact PDC. Opt-in `pluginhost_vst3_installed` (`SQ_VST3_SMOKE_DIR`) is Skipped by default; the builder's 8-plugin smoke run was not re-run. Gaps: no IUnitInfo groups, no 64-bit processing, no note expression, no Linux run loop, editors not in the GUI, macOS/Linux unverified |
| pluginscanner | done | `sq-pluginscan` child per file, crash/timeout quarantine, blocklist, manifest; scans CLAP and VST3 (every `PluginFormat` class) |
| ARA | not done | |
| time-stretch | done | Own phase vocoder; no commercial backend (élastique seam not built) |
| codec-wav / flac / mp3 / ogg / aac | partial | Ogg **Vorbis read + write** (libogg/libvorbis, BSD-3); Opus decode-only via user-supplied libopusfile (`codec_ogg_opus` Skipped here), **no Opus encoder**; MP3 encode needs user LAME; AAC via Media Foundation (Windows) |
| codec-midifile (MIDI file) | done | SMF 0/1 read/write |
| sqlite-index | done | |
| scripting-js | done | See 2.4 |
| analysis-beat (+ tuner), analysis-chords, analysis-stems | partial | see 2.7 |
| hardware-surface | partial | See 2.6 MIDI control surfaces. Sole `IControlSurfaceService` (decision of this pass) |
| hardware-remote | partial | OSC + DNS-SD/mDNS only (see 2.9) |
| link-ableton (tempo sync) | partial / seam | Local timeline (seqlock, RT-safe `captureState`); network sync only with the licensed SDK (`link_ableton_sdk` Skipped here) |
| cloud-client | partial | See 2.8 |
| video-engine | partial | Media Foundation backend done (`video_engine_module`); ffmpeg backend is a seam (`SQ_FFMPEG_DIR`, LGPL shared build; `video_engine_ffmpeg` Skipped here, not compiled by the verifier). Not wired into tracks or the GUI (only `Video.Probe`) |
| DAWproject handler | partial | `formats/dawproject` static library (not a DLL). Subset: tracks, audio/note clips, markers, first tempo + signature; no automation, devices, fades |
| AAF / OpenTL / foreign project readers, spatial audio | not done | |

## Tools

| Tool | Status | Notes |
|---|---|---|
| `sq-pluginscan` (tools via pluginscanner) | done | Child process per plugin file |
| `sq-skinc` (`tools/skin-compiler`) | done | Validates (`--strict`) and packs themes; refuses folder/theme-name mismatches and the reserved name `current`; emits pack, `.rc`, header, manifest |
| `sq-pack` (`tools/packager`) | done | build / verify / unpack `.package` containers (see 2.6) |

## Milestones (plan 3.4)

| Milestone | Exit criterion | Status | Evidence |
|---|---|---|---|
| M0 walking skeleton | app boots, loads/unloads a service without restart; ABI stamped; CI on 3 OSes | partial | `app_selftest`, `app_reload_test`, `abi_lock`; CI workflow exists but only Windows was run |
| M1 document & undo | create/save/reopen; 100-action undo stress | done | `engine_doc_undo_stress`, e2e "M1"; autosave implemented (`Engine.Autosave.Dir`), no crash-recovery prompt in the GUI |
| M2 audio engine | record 2 tracks @64 samples round trip; glitch-free playback harness on test hardware | partial | e2e "M2" (NullDevice at 16x, 64-frame blocks, bit-exact takes) + `engine_audio_record` (60 s at 64 frames: no ring overflow, sample-exact; realtime-paced 1024-frame case: zero late callbacks). Zero dropouts at 64 frames in real time is **not** proven. WASAPI streams; **ASIO is a seam** (no streaming without the licensed SDK, never compiled against it here); no hardware glitch harness |
| M3 editing & content | full editing workflow without mouse-only paths; browser search over 1k+ items | done | e2e "M3", `engine_edit_workflow`; browser has no GUI panel yet |
| M4 plugin hosting | host top-20 commercial plugins; scanner survives a crashing plugin | partial | e2e "M4" (CLAP) + **"M4 (VST3)"** + `pluginscanner_quarantine`. CLAP and VST3 adapters in-process with exact PDC. Commercial plugins: only the VST3 builder's opt-in smoke run (8 installed plugins, not re-run, not in CI); no top-20 evidence. No plugin editor windows in the GUI; sidechain inputs fed silence |
| M5 skin & platform UI | skin language, theme://, @2x, dark/light, accessibility, remote pages | partial | Skin language + GUI (`ui_core`, `app_window_smoke`), themes + compiler + embedding (`tools_skin-compiler_*`, `app_integration_commands`, e2e M5/M8), UIA bridge wired into the GUI (`ui_uia_window`), remote pages from the theme skin (e2e M5/M8). Remaining: Skia/Yoga not used, arrange view not themed, no screen-reader test, non-PNG images unchecked |
| M6 scripting | a user script adds a menu command that edits the document with full undo | done | e2e "M6" |
| M7 formats & interchange | WAV/AIFF/FLAC/MP3/OGG/Opus read+write, MIDI file, DAWproject export, stem export, video | partial | e2e "M7" (WAV/FLAC/MIDI/DAWproject). **Ogg Vorbis write done**; Opus write missing; MP3 write needs user LAME; video decode via Media Foundation (`video_engine_module`) but no video track |
| M8 optional packages | cloud, preset sync, AI assistant, Link, hardware network | partial | cloud-client, link-ableton (local clock), hardware-remote (OSC/mDNS), hardware-surface; all reachable through shell commands (e2e M5/M8). No preset sync, no AI assistant, placeholder cloud backend |

## Linux baseline pass (this pass)

- Build fixes: `<cstdlib>` in the CLAP test plugin and `<cstring>` in `tests/test_platform.cpp` (GCC 13 does not get these transitively).
- `ipc` (POSIX): duplicate `sq_pipe_listen` is now detected with a sidecar `flock`, not a `connect()` probe. The old probe enqueued a phantom connection that `sq_pipe_accept` returned instead of the real client.
- `scripting-js`: QuickJS stack budget is capped to 8 MB even when the host reports an enormous/implicit stack (Linux with `RLIMIT_STACK=unlimited` used to allow a 1 GB budget and segfault before QuickJS' guard fired).
- `platform/thread`: a kernel/container with an empty `SCHED_FIFO` priority range now reports `SQ_E_ACCESS` instead of calling `pthread_setschedparam` with an invalid priority.
- `analysis-stems` test finds the POSIX `libsqplatform.so` name; `engine_audio_record` only requires RT priority when the process can actually set it.
- Core workflow: `File.ImportAudio`, `File.ImportMidi`, and mixer commands (`Track.SetVolume/SetPan/ToggleMute/ToggleSolo/Rename`); media sample-rate conversion in the graph. Covered by e2e "M7: import audio (44.1 kHz into a 48 kHz song) + MIDI, mixer commands, save/reopen" and `engine_audio_graph`.

## Second integration pass (this pass)

- **Verified units:** asio (3 defects fixed: notifier use-after-free, unbounded driver buffer sizes, stale latency/rate after notifications), vst3 (7 defects), ui-m5 (UIA threading, PNG decompression bomb, three stack overflows in the skin validator/builder, theme-name and BMP fixes), surfaces (button LEDs, toggle sync, validator gaps, midi-win refresh race), optional (OSC pattern DoS, sign-out race, loopback-URL bypass, redirect following), packager (unloadable script packages, unsafe unpack). Each fix has a regression test that failed first.
- **Decision:** `hardware-surface` is the only `IControlSurfaceService`; `hardware-remote` keeps OSC + discovery.
- **Interfaces:** `ISurfacePump` -> `sdk/include/sq/interfaces/surface.h`, `IAsioDriverStatus` -> `asio.h` (same IIDs); `video.h`, `surface.h`, `asio.h` added to `tests/test_interfaces.cpp` and `tests/c_interfaces_probe.c`; `remote.h` implementer comment corrected (link-ableton, hardware-surface).
- **Build:** `sq_add_test` now sets `SKIP_RETURN_CODE 77` centrally (CONTRACTS section 10 was wrong before).
- **App wiring:** stock themes embedded in the EXE + `UI.Theme`; `IThemeResolver`/`IRemotePageProvider`/`theme://` registered via `UiServices`; GUI themed and connected to UI Automation; `ISurfacePump::pump()` in `Shell::idle`; new commands `Audio.ListDevices/SelectDevice`, `UI.SetTheme`, `Surface.*`, `Osc.*`, `Sync.Link`, `Account.*`, `Video.Probe`; OSC bound to loopback by default, inbound OSC restricted to transport/navigation/undo.
- **New integration tests:** e2e "M4 (VST3)", e2e "M5/M8", CTest `app_integration_commands`.
- **Docs:** CONTRACTS 5.5 table + integration notes, `third_party/README.md` (libogg, libvorbis; stb_vorbis now unused), README.

## Known gaps (prioritised)

1. ASIO streaming and network Link need licensed SDKs; neither SDK path has been compiled in this tree (a licence holder should do one build of each).
2. M2 real-time glitch-free proof at 64 frames on hardware; full-duplex WASAPI record into the engine untested.
3. M4 commercial top-20 plugin evidence; plugin editors in the GUI; VST3 IUnitInfo / 64-bit / note expression; sidechain feeds silence; no plugin instruments in the graph.
4. Structural edits recompile the graph (plugin state / PDC lines restart).
5. Skia/Yoga not adopted; arrange view not themed; non-PNG images decoded by WIC without a preflight.
6. Opus encoder; video tracks; AAF/OpenTL; ARA; spatial audio.
7. Control surfaces untested with hardware; remote-page targets against the real document resolver untested.
8. Cloud backend is placeholder; mDNS rate limiting; DNS-SD late-callback risk; cloud shutdown wait on black-holed networks.
9. macOS never built; Linux builds and the non-GUI suite passes, but has no window backend or hardware audio driver.
10. `codec_aac` timed out once under parallel CTest (intermittent Media Foundation hang, not reproduced); codec-aac and video-engine tests use fixed temp paths, so two instances of the same test collide (CTest never does that, but it hides isolation bugs).
11. Product decision pending on spec-mandated file extensions and scripting vocabulary (2.6).

## Final test run

Fresh (`-Clean`) unfiltered build in `build\final` (`build.ps1 -Clean -CMakeArgs "-DSQ_WARNINGS_AS_ERRORS=ON"`, Debug, MSVC 2022, Windows 11, `/W4 /WX` clean). 118 CTest entries; the 5 SDK/hardware-gated cases are reported **Skipped** (exit 77), not passed: `audiodriver_asio_stream` (no ASIO SDK), `codec_ogg_opus` (no libopusfile), `link_ableton_sdk` (no Link SDK), `pluginhost_vst3_installed` (opt-in), `video_engine_ffmpeg` (no ffmpeg). All three WASAPI hardware cases streamed on this machine.

- First run after the clean build: 117/118; **`codec_aac` hit the 300 s CTest timeout once** (it normally finishes in ~1 s; 4 serial reruns passed in ~1 s each; no code in codec-aac changed in this pass). Treat it as an **intermittent Media Foundation hang, cause not found** (open item).
- Rerun of the full suite in the same build dir: **`100% tests passed, 0 tests failed out of 118`** (5 skipped as above), including `app_window_smoke` (desktop session), `app_integration_commands` and `e2e_milestones` (8 cases incl. M4 VST3 and M5/M8).
- `SquizzyStudio --headless`: 24 modules, 117 classes, 111 commands, themes base/dark/light (active dark). `--selftest`: PASS. `--list-classes`: 117 classes.

### Linux run (2026-10-28)

`cmake --preset linux && cmake --build --preset linux --parallel 18 && ctest --preset linux -LE gui --output-on-failure -j 8` -> **100% tests passed, 0 failed out of 115** (99 passed, 16 hardware/SDK/Windows-only cases Skipped). `SquizzyStudio --headless`: 24 modules, 117 classes, 111 commands. The added e2e case imports a 44.1 kHz WAV into a 48 kHz song, renders 96,000 frames, imports a MIDI file, exercises the mixer commands, and saves/reopens the session.
