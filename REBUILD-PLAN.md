# Squizzy Studio — DAW Architecture & Build Plan

**Product:** a cross-platform (Windows first) recording-first digital audio workstation
**Status:** planning · 2026-10-27

---

## 0. Scope and ground rules

This document specifies the architecture and build plan for an original DAW. Ground rules:

- All product code is written from scratch; third-party components are used under their proper licenses (permissive, LGPL dynamic-linked, or commercial where a license is purchased).
- No code, DSP models, presets, skins, images, or text resources are copied from any existing product.
- Architecture lessons from established DAW engineering practice are fair game; this plan favors a modular, scriptable, service-oriented design of the kind that has proven to scale in this product category.

---

## 1. Product target

A recording-first DAW with: multitrack audio/MIDI editing, realtime effect/instrument graph, VST3 + CLAP hosting, ASIO/WASAPI/MME drivers, JavaScript macro scripting, a skinnable vector UI, an SQLite-indexed content browser, and open interchange (DAWproject, AAF, OpenTL, standard media formats). Cloud sync and hardware-control integration are optional, post-1.0 packages — never core dependencies.

Target profile:

| Property | Value |
|---|---|
| Platforms | Windows x64 first; macOS & Linux from the same CMake tree |
| Toolchain | C++20, MSVC/clang, CMake + CI matrix (Windows/macOS/Linux) |
| Shape | Thin shell EXE + versioned service DLLs + out-of-process plugin scanner |
| Scripting | Embedded JS engine as a service module |
| Persistence | XML-archive document containers, SQLite indexes, ZIP-wrapped presets |

---

## 2. Architecture

### 2.1 Platform layer

A self-owned cross-platform foundation library (the `platform/` module), exposed through a stable C ABI and implemented in C++ inside:

- Atomics, locks, thread-local storage, worker/thread pools.
- **IPC primitives:** pipes, shared memory, semaphores (used by the out-of-process scanner and any future sandboxed hosts).
- Filesystem abstraction, unicode/text services, XML + JSON, crypto/hashing.
- Versioned: every module and package declares the platform ABI range it requires, and the loader enforces it.

### 2.2 Module system and plugin ABI

The app is a microkernel-style shell over the platform layer. Every subsystem is a separately versioned DLL exporting exactly two symbols:

- `GetClassFactory` — returns a lazily constructed, refcounted, vtable-dispatched singleton factory (COM-style, no COM dependency); registers module metadata (fourcc tag + module name) and serves class objects by GUID `classID`.
- `ModuleMain` — module lifecycle entry (init/shutdown/version handshake).

Classes are addressed by GUID across the whole app — and **script classes use the same registry**: script packages contain `<ClassFactory>` XML with `<ScriptClass classID="{GUID}" category="..." name="..." sourceFile="X.js" functionName="factoryFn">` entries. One registration mechanism covers native modules, menu commands, edit tasks, and user extensions.

Service DLL roster (one subsystem each): ASIO device service, WASAPI device service, Windows MIDI service, VST3 host adapter, CLAP host adapter, ARA adapter, time-stretch, codec services (WAV/AIFF, FLAC, MP3, AAC, OGG/Opus, REX-class if licensed), MIDI file, SQLite index, JS engine, cloud client, stem/chord/beat/pitch analysis, tuner, spatial audio, video engine, Link service, hardware-control service, session/import-export handlers (DAWproject, AAF, OpenTL, foreign-project readers), and the instrument/effect bundles.

**Rationale:** driver crashes, plugin-format bugs, and codec changes stay isolated from the sequencer core; the shell loads only what a document needs.

### 2.3 Sequencer core

Lives in the main EXE (no DLL boundary for the hot path):

- **Graph:** an audio process graph of `AudioProcessor` variants (FX, synth, parallel, track, sub-mix) and node types (file, clip, modification, event, media). A dedicated **fast-lane low-latency input-monitoring path** runs beside the main graph so overdub monitoring never waits on the full render graph.
- **Rendering:** render engine + dedicated realtime render thread (`AudioRenderThread`) with explicit thread-priority setup; stem/ selective-mixdown/ click renderers as task classes; "transform to realtime audio" (bounce-in-place) as a render task.
- **Latency bookkeeping is first-class:** `processLatency`, `processInputLatency`, `processOutputLatency`, `latencyTotal`, block-size and sample-rate negotiation, `lowLatencyMonitoring` mode — the compensation data model is designed before the mixer, not after.
- **Documents:** typed document trees — Song (arrange), Project (mastering-style), Show (live-performance) — under a DocumentManager, persisted through an XmlArchive (a container holding per-editor XML parts); WAV/AIFF I/O supports the full RIFF chunk vocabulary (`bext`, `smpl`, `CHAN`, cue/marker chunks).
- **Edit model:** every user edit is a command object (`Action`) executed inside explicit transactions, recorded by an UndoKeeper, followed by declarative `SideEffect` propagators (e.g. automation-follow, time-map, remove-overlaps, chord-follow) and optional journals for UI state. This is the backbone — editing features are added by writing new Action + SideEffect classes, nothing else.
- **Settings:** hierarchical dotted-key registry (`Engine.*`, `Devices.*`, `Media.*`, `Platform.*`…) mirrored into the scripting layer as `object://` URLs.

### 2.4 Scripting

- JS engine service module: **SpiderMonkey** (MPL-2.0) or **QuickJS** (MIT, much smaller binary) — decide in M6 by binary-size and API needs.
- Bootstrap scripts define the host API: `Host.Classes.createInstance`, `Host.Objects.getObjectByUrl("object://app/...")`, `Host.Interfaces.*` (ICommandHandler, IObserver, IController, IComponent), URL schemes `object://`, `package://`, `theme://`.
- User-facing macros ship as JS packages registered via the `<ClassFactory><ScriptClass>` GUID registry — **scripts are first-class plugins**. Bundled command packages: audioedit, musicedit, projectedit, trackedit, toolset, macrohandler.

### 2.5 GUI

- Custom retained-mode UI on **Skia** (BSD) with the **Yoga** (MIT) flexbox layout engine; webp/PNG asset pipeline; embedded OFL-licensed fonts; icon fonts for tooling.
- **Skinning is declarative XML:** `<Skin>` documents with `<Forms>`, `<View>`, `<Knob>`, `<Target>` widgets, `<define paramName=…>` parameter bindings, `<if>/<switch>` preprocessor conditionals, `@2x`/zoom asset variants. The same skin language drives hardware remote pages (controller knob/page maps) so hardware UIs are data, not code.
- Theme assets are compiled into the EXE's resource section at build time and addressed via `theme://` URLs; a skin-compiler tool validates and packs them.

### 2.6 Data & content formats

| Format | Encoding | Purpose |
|---|---|---|
| `.package` (scripts/skins) | `PACKAGE` magic + version byte + zlib → XML or JavaScript | script/skin registration bundles |
| `.package` (snapshots) | ZIP + `snapshots.json` manifest | plugin thumbnail bundles |
| `.langpack` | ZIP: gettext `.mo` catalogs per module + `metainfo.xml` + properties maps | localization |
| `*.tags.db` | SQLite (`TaggedItem`/`DataItem`/`StoreInfo` class tables, semicolon `_tags`) | content browser index |
| `.preset` / `.multipreset` / `.instrument` / `.trackpreset` | ZIP: preset XML + `metainfo.xml` | plugin presets |
| `.dsppreset` | JSON | hardware DSP presets |
| `.chords`, `.musicalscale`, `.colorscheme`… | plain XML | song-adjacent data |
| Extensions | directory + `metainfo.xml` + `installdata.xml` (gates on app id + platform ABI version) | installable packages |
| MIDI control surfaces | `.device` + `.surface.xml` (`<ControlSurface><Controls><Control><MidiMessage status address>`) | hardware maps |

Schemas are our own, following these container patterns (small, versioned, metadata-included).

### 2.7 ML / analysis stack

- Neural inference for chord/beat/pitch/drum detection and stem separation via **ONNX Runtime** (MIT) — or **frugally-deep** (MIT) for Keras-trained models — wrapped in a `NNPredictionDispatcher`-style service with tensors expressed in plain C++.
- A music-theory helper library (own implementation) for chord naming/quantization; models ship as data packages so they update independently of code.

### 2.8 Cloud (optional package)

Account/entitlement service behind a `cloud/` service seam: registration, entitlement checks, subscription states, preset/tone sync, telemetry, AI assistant hook. Build on standard managed services (e.g., OAuth2/OIDC identity, an analytics collector, an LLM API for the assistant) — but the core app must be fully functional offline.

### 2.9 Hardware integration (optional package)

A hardware-control network service (MIDI over network/USB/BLE) with mDNS/DNS-SD discovery, remote-page skins, and control-surface XML maps; Ableton Link via its SDK (free license from Ableton); OSC support where useful.

### 2.10 SIMD

A runtime-dispatched vector op table (`vectorLibraryBindFunction`-style): one generic build plus SSE4.2/AVX2/AVX-512 kernel sets selected at load, behind a small op-table header. Implementation via **Highway** or **xsimd** (both permissive) — no proprietary IPP dependency.

---

## 3. Build plan

### 3.1 Core decisions

1. Microkernel shell + service modules, two-export factory ABI, **ABI version-gated packages**.
2. GUID class registry spanning native and script classes.
3. Own platform layer with a C ABI boundary.
4. Engine = document-graph + realtime render thread + separate fast-lane monitoring path.
5. Driver and plugin-format isolation; plugin scanning always out-of-process.
6. SIMD dispatch table, runtime-selected.
7. Declarative skinning; same language for on-screen and hardware UIs.
8. Scriptable via JS exposing a URL-addressed object graph + observer interfaces.
9. SQLite for browser indexes, caches, settings side-data.
10. CMake + CI on three OSes from day one, strict ABI stamping.

### 3.2 Third-party component selection

| Need | Choice | License |
|---|---|---|
| Scripting | SpiderMonkey **or** QuickJS | MPL-2.0 / MIT |
| Time-stretch | **élastique Pro (zplane)** license — industry standard — or SBSMS/RubberBand as placeholder | commercial / GPL-dual |
| Neural inference | ONNX Runtime (or frugally-deep) | MIT |
| Vector math | Google Highway or xsimd | Apache-2.0 / BSD |
| 2D rendering | Skia | BSD |
| UI layout | Yoga | MIT |
| Plugin ABIs | CLAP (MIT), VST3 SDK (GPL3/proprietary dual) | per LICENSE |
| Audio codecs | libogg/vorbis/FLAC/Opus (BSD), mpg123 (LGPL2.1, dynamic), LAME (LGPL2, dynamic), mp4v2 (BSD), ALAC (Apache-2), AAC via fdk-aac where licensed | mixed |
| Media/decode glue | ffmpeg (LGPL build, dynamic) | LGPL |
| CD burning | IMAPI2 (Windows) / libburn | OS / GPL |
| Databases / serialization | SQLite, nlohmann/json, pugixml or Expat, cmark | permissive |
| Unicode / text | ICU | ICU license |
| Compression | zlib, LZ4 | permissive |
| Sync | Ableton Link SDK | free proprietary license |
| Fonts | 2 OFL-licensed families | OFL |

Any component that can't be satisfied permissively (élastique, Link, VST3 proprietary path) sits behind its own service-module seam so it can be swapped or dropped.

### 3.3 Repo & module layout

```
squizzystudio/
  platform/        # foundation: c-api headers + system, text, net, crypto, ipc
  engine/          # document model, transactions/undo, graph, render thread, fast-lane
  services/        # one DLL per subsystem
    audiodriver-asio/ audiodriver-wasapi/ midi-win/
    pluginhost-vst3/ pluginhost-clap/ pluginscanner/   # scanner = separate exe
    codec-wav/ codec-flac/ codec-mp3/ codec-aac/ codec-ogg/ time-stretch/
    analysis-stems/ analysis-chords/ analysis-beat/
    scripting-js/ sqlite-index/ link-ableton/ hardware-remote/
  app/             # shell EXE: document manager, command system, settings registry
  ui/              # renderer (Skia), layout (Yoga), skin XML, widgets, theme system
  sdk/             # public headers: class factory ABI, GUIDs, scripting API, skin spec
  formats/         # .package readers/writers, presets, DAWproject, AAF/OpenTL import
  tools/           # packager, plugin scanner manifest, skin compiler (assets -> .rsrc)
  ci/              # CMake presets, CI pipelines, ABI-version stamping
```

### 3.4 Milestones

**M0 — Walking skeleton (4–6 wks).** EXE + platform/ + module loader with two-export ABI, GUID registry, settings registry, empty window via ui/ (Skia+Yoga), CMake+CI on 3 OSes. *Exit:* app boots, loads/unloads a dummy service DLL without restart; ABI version stamped.

**M1 — Document & undo core (6–8 wks).** Song document tree, XmlArchive save/load (container with per-editor XML parts), transaction/action/undo-keeper with side-effect hooks, autosave. *Exit:* create/save/reopen a session; 100-action undo stress test passes.

**M2 — Audio engine (8–12 wks).** WASAPI then ASIO services; realtime render thread with priority setup; track → fader → master graph; fast-lane input monitoring; 32/64-bit float pipeline; latency-compensation bookkeeping from §2.3. *Exit:* record 2 tracks @64 samples roundtrip; glitch-free playback CI harness on test hardware.

**M3 — Editing & content (8 wks).** Arrange edit tools as command objects (split/trim/slip/fade/quantize/silence/tempo-map edits…), content browser backed by SQLite tag DB, presets as ZIP(preset XML + `metainfo.xml`). *Exit:* a full editing workflow without touching the mouse-only paths; browser search over 1k+ indexed items.

**M4 — Plugin hosting (6–10 wks).** VST3 + CLAP adapters in-proc; **out-of-process scanner** with quarantine/crash-backoff; plugin delay-compensation & sidechain plumbing; ARA later. *Exit:* host top-20 commercial plugins; scanner survives a deliberately crashing plugin.

**M5 — Skin & platform UI (6–8 wks).** Skin XML language (forms/views/knobs/defines/conditionals), `theme://` resolver, @2x assets, dark/light themes, accessibility hooks; hardware remote pages driven by the same skin spec.

**M6 — Scripting (4–6 wks).** JS engine service (SpiderMonkey or QuickJS), `Host.Objects` URL graph + observers, `<ClassFactory><ScriptClass>` package registration, bundled command scripts per §2.4. *Exit:* a user script adds a menu command that edits the document with full undo.

**M7 — Formats & interchange (4 wks).** WAV/AIFF/FLAC/MP3/OGG/Opus read+write (full RIFF chunk coverage incl. `bext`/`smpl`), MIDI file, DAWproject export (open format), stem export, video track decode (ffmpeg) — one handler DLL per format.

**M8 — Optional packages (post-1.0).** Cloud/account seam, preset sync, AI assistant, Link, hardware-control network.

### 3.5 Team & reality check

Mature commercial DAWs represent 15+ years of large-team investment. Scope the rebuild honestly: **MVP = M0–M3** (a stable recording/editing DAW) with 3–5 engineers for ~12–18 months; M4–M7 bring parity-class features by year 2. The service-module architecture is the amortization strategy: small independently-testable DLLs, one registration mechanism, scripted long-tail features.

### 3.6 Risks

| Risk | Mitigation |
|---|---|
| Realtime stability | fast-lane separation; RT-thread rules enforced in CI (lock-free containers, no allocations in process path) |
| Plugin ecosystem chaos | out-of-proc scanner, blocklist, per-plugin sandbox option later |
| Time-stretch quality | budget a commercial license; isolate behind the `time-stretch` service so the backend swaps |
| ABI rot | version-gate every module/package at load time |
| Scope creep (cloud/AI/hardware) | service seams exist from M0; features stay optional packages |
| Legal | original code only; permissive/LGPL/commercial components per §3.2; no third-party assets, presets, or text copied |
