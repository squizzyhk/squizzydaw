# Squizzy Studio: contracts for parallel agents

This is the handoff document for everyone building on the M0 walking skeleton. Read it fully before writing code. The architecture spec is `REBUILD-PLAN.md`. This file covers the **mechanics**: how to add things without stepping on anyone else.

---

## 0. Hard rules

1. **Clean room.** Never open, read, grep, list or reference anything under `re/`. Write every line yourself. Never reuse third-party product names, GUIDs, strings, presets, skins or file contents. The product is **Squizzy Studio**, the C prefix is `sq_`, the C++ namespace is `sq`.
2. **One owner per directory** (see §2). Write only inside the directory you own (`services/<x>/`, `engine/<x>/`, `formats/<x>/`, `tools/<x>/`, `ui/`), plus an interface header `sdk/include/sq/interfaces/<area>.h` **only if the orchestrator assigned it to you**. The area headers listed in §5.5 are frozen and owned by the orchestrator.
3. **Never edit the root `CMakeLists.txt`** or the parent `engine/ formats/ tools/` CMakeLists. Components are auto-discovered.
4. **Never change a published interface.** To extend one, add a new interface with a new IID (§5.3).
5. **Core ABI headers are frozen.** These are `sdk/include/sq/*.h` and `platform/include/sq/platform*`. Changing them needs an ABI bump, and the `abi_lock` test enforces it (§9). If you need something there, ask the M0 owner or orchestrator; do not edit it yourself.
6. **No link-time dependencies between services.** Services talk only through interfaces, the class registry and the host service locator.
7. **Build in your own build dir** (§3), never `build/windows-debug` when other agents are running.
8. **Your tests must be green** before you hand off, and the whole suite must still pass.

---

## 1. Layout

```
squizzystudio/                      (= repo root)
  VERSION                           product version + platform ABI (edit via ci/stamp-abi.cmake only)
  CMakeLists.txt                    root: auto-discovers components. DO NOT EDIT.
  CMakePresets.json                 windows-debug / windows-release / linux / macos
  build.ps1, build.bat              Windows wrapper: vcvars64 -> CMake+Ninja -> build -> ctest
  cmake/SqHelpers.cmake             sq_add_service / sq_add_library / sq_add_tool / sq_add_test
  sdk/include/sq/                   PUBLIC C ABI (frozen core) + C++ helpers
     config.h types.h guid.h unknown.h module.h media.h sq.h
     cpp/ptr.hpp object.hpp module_def.hpp settings.hpp   (header-only helpers)
     sdk.hpp                        umbrella for C++ modules
     interfaces/<area>.h            one header per area, one owner each
  platform/                         sqplatform DLL (C ABI): threads, IPC, fs, text, XML, JSON, hash, log
     include/sq/platform.h          umbrella; include/sq/platform/*.h per area; platform.hpp = C++ sugar
  app/                              SquizzyStudio.exe + sqappcore (settings, registry, host, loader)
  ui/                               squi: backend-neutral window/renderer (GDI stand-in, Skia planned)
  services/<name>/                  one module DLL per subsystem -> bin/services/<name>.dll
  engine/ formats/ tools/           parent dirs (orchestrator); each child <dir>/<x>/ has one owner, auto-discovered
  third_party/<name>/               vendored libs (tp::<name> targets, EXCLUDE_FROM_ALL); miniz is shared
  tests/                            core tests + tests/framework (sqtest)
  ci/                               stamp-abi.cmake, CI notes (workflow in .github/workflows/ci.yml)
  docs/CONTRACTS.md                 this file
  third_party/                      vendored permissive libs, one subdir each (see third_party/README.md)
```

Build output (per build dir):

```
build/<dir>/bin/SquizzyStudio.exe      shell
build/<dir>/bin/sqplatform.dll         platform layer
build/<dir>/bin/services/<name>.dll    service modules (auto-loaded by the app)
build/<dir>/bin/testmodules/*.dll      test-only modules (never auto-loaded)
build/<dir>/bin/sqtest_<name>.exe      test executables
build/<dir>/bin/<tool>.exe             tools
```

---

## 2. Directory ownership

| Path | Owner | Notes |
|---|---|---|
| `sdk/include/sq/*.h`, `sdk/include/sq/cpp/`, `platform/`, `app/`, `cmake/`, `ci/`, `tests/` (core), root files | M0 skeleton / orchestrator | Changes go through the orchestrator |
| `sdk/include/sq/interfaces/<area>.h` | the agent of that area | **Only your own file.** `dummy.h` belongs to M0 |
| `engine/CMakeLists.txt`, `formats/CMakeLists.txt`, `tools/CMakeLists.txt` | orchestrator | parent dirs: they only call `sq_add_child_components()` |
| `engine/<x>/` | that engine unit's agent | `engine/doc` (document model, undo), `engine/edit` (edit actions + commands), `engine/audio` (graph, render, PDC, record), `engine/simd` (sq::dsp vector ops) |
| `ui/` | ui agent | single component, always built (the shell links `sq::ui`); keep `include/sq/ui/ui.hpp` backend-neutral |
| `formats/<x>/` | that format unit's agent | e.g. `formats/package`, `formats/preset`, `formats/dawproject` |
| `tools/<x>/` | that tool's agent | e.g. `tools/packager`, `tools/skinc`, `tools/indexer` |
| `third_party/<name>/` | whoever vendors it (shared libs: orchestrator) | see §12 |

**Subdir conventions** for `engine/<x>`, `formats/<x>`, `tools/<x>`:

- Library target `sqengine_<x>` / `sqformats_<x>` (via `sq_add_library`) with alias `sq::engine_<x>` / `sq::formats_<x>`; tools use `sq_add_tool(NAME sq-<x> ...)`.
- Public in-process headers in `<dir>/<x>/include/sq/<dir>/<x>/*.hpp` (e.g. `#include "sq/engine/doc/document.hpp"`).
- Test names prefixed `engine_<x>_`, `formats_<x>_`, `tools_<x>_`.
- If you link a sibling's target, put `sq_require_components(engine/<y>)` at the top of your CMakeLists: configure then fails with a clear message when it is filtered out. Prefer coding against the `sdk/include/sq/interfaces/` headers and mocks over linking siblings.
| `services/<name>/` | that service's agent | e.g. `services/audiodriver-wasapi/` |

Service directory names follow plan §3.3: `audiodriver-asio`, `audiodriver-wasapi`, `midi-win`, `pluginhost-vst3`, `pluginhost-clap`, `pluginscanner`, `codec-wav`, `codec-flac`, `codec-mp3`, `codec-aac`, `codec-ogg`, `time-stretch`, `analysis-stems`, `analysis-chords`, `analysis-beat`, `scripting-js`, `sqlite-index`, `link-ableton`, `hardware-remote`.

If you need a change in a directory you do not own, write the request into your handoff report. Do not make the change.

---

## 3. Building in your own directory

Windows. Nothing is on PATH; the wrapper calls `vcvars64.bat` for you:

```powershell
# full build + all tests in your private dir (replace <you>):
powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\user\Documents\squizzystudiopro\build.ps1 -BuildDir build\agent-<you>

# same thing from cmd:
C:\Users\user\Documents\squizzystudiopro\build.bat -BuildDir build\agent-<you>

# useful flags
-Preset windows-release          # RelWithDebInfo instead of Debug
-Target svc_<name>               # build one target (services are svc_<name>, tests sqtest_<name>)
-TestFilter "<regex>"            # ctest -R
-ExcludeLabel gui                # skip the window smoke test (headless sessions)
-NoTest / -Clean / -ConfigureOnly
-CMakeArgs "-DSQ_WARNINGS_AS_ERRORS=ON"   # what CI uses on Windows; keep your code warning-free at /W4
-Jobs 4                          # many agents share this machine: always cap parallelism
-CMakeArgs "-DSQ_ONLY=engine/doc;services/codec-wav"   # build only these components (+ core)
-CMakeArgs "-DSQ_SKIP=services/foo;engine/bar"         # exclude broken neighbours
```

Set `$env:CMAKE_BUILD_PARALLEL_LEVEL=4` as well (or pass `-Jobs 4`).

### 3.1 Component filter (`SQ_ONLY` / `SQ_SKIP`)

Component ids are repo-relative dirs: `engine`, `engine/<x>`, `formats`, `formats/<x>`, `tools`, `tools/<x>`, `services/<x>`, `third_party/<x>` (SQ_SKIP only).

- `SQ_ONLY` (empty = everything): only the listed components are configured. Listing a parent (`engine`) enables all its children; listing a child (`engine/doc`) enters `engine/` but adds only that child.
- `SQ_SKIP`: excludes ids and their children; wins over `SQ_ONLY`. `SQ_SKIP_SERVICES=foo;bar` still works (= `SQ_SKIP=services/foo;services/bar`).
- **Core is always built:** `sdk`, `platform`, `app` (+ `SquizzyStudio.exe`), `ui` (the shell links it), `tests/` + `tests/framework`, `services/dummy`.
- Both are **CMake cache variables, so they stick** in your build dir. Reset with `-CMakeArgs "-USQ_ONLY"` (or `-DSQ_ONLY=`).
- List your own dependencies: `SQ_ONLY=engine/doc` does not pull in `engine/graph`. `sq_require_components()` (§2) turns a missing dependency into a clear configure error.
- `third_party/*` libs are always configured but `EXCLUDE_FROM_ALL`: they compile only when something you build links them.
- Filtered-out services are removed from `bin/services` at configure time, so `service_exports` and the app stay consistent in a filtered build dir.
- Typical builder loop: `build.ps1 -BuildDir build\agent-<you> -Jobs 4 -CMakeArgs "-DSQ_ONLY=services/codec-wav"` (builds and runs the core tests plus yours). Before handoff, run once without a filter (or with `SQ_SKIP` for neighbours that are currently broken, and say so in your report).
- The matching rules are unit-tested by the CTest `component_filter` (`tests/cmake/test_component_filter.cmake`).

Linux/macOS: `cmake --preset linux && cmake --build --preset linux && ctest --preset linux`. Re-run the configure step after adding a new component directory (the wrapper always reconfigures; a bare `cmake --build` does not see new dirs).

Configure also deletes `bin/services/<x>` modules whose `services/<x>/` no longer exists (or is skipped), because the app auto-loads everything in that folder.

Disk is tight (about 26 GB free). Delete your `build/agent-<you>` when you are done, and do not vendor large SDKs (§10).

---

## 4. The module ABI

### 4.1 Exports

Every module DLL exports **exactly two** C symbols. `SQ_MODULE_EXPORTS` writes both for you, and `sqtest_factory_abi` checks the export table:

```c
sq_IClassFactory* SQ_CALL GetClassFactory(void);            // lazily-built singleton, ADD-REF'D
sq_result        SQ_CALL ModuleMain(uint32_t reason, sq_IHostContext* host);  // SQ_MODULE_INIT / SQ_MODULE_SHUTDOWN
```

Load sequence in `app/src/module_loader.cpp`:

1. `LoadLibrary`, then resolve both exports.
2. `GetClassFactory()`, then `getModuleInfo()`.
3. **ABI gate:** the host's `SQ_PLATFORM_ABI` must lie in `[abi_min, abi_max]`. Otherwise the module is rejected and unloaded, and INIT never runs.
4. Duplicate name or fourcc: rejected.
5. `ModuleMain(INIT, host)`. Any failure rejects the module.
6. Classes are registered in the GUID registry.

Unload runs in reverse:

1. Unregister the module's classes (no new instances can be created).
2. `ModuleMain(SHUTDOWN)`: the module drops everything it registered with the host in INIT (services, settings observers, dynamic classes, log sinks).
3. Check `canUnload()` (no object of the module alive) **and** that nobody but the loader still references the module's factory.
4. If still busy: **rollback**. `ModuleMain(INIT)` runs again, the classes are re-registered, and `unload` returns `SQ_E_BUSY`. The module keeps working. (At exit, `unloadAll(force)` instead leaves a busy DLL mapped rather than crash.)
5. Otherwise release the factory and `FreeLibrary`.

Consequences for module authors:

- `onInit` may run again after `onShutdown` **in the same DLL image** (busy rollback). `onShutdown` must reset every global/static it set, and `onInit` must not assume fresh statics.
- Anything you register in `onInit` and forget to remove in `onShutdown` makes your module permanently un-reloadable (always `SQ_E_BUSY`). `tests/modules/selfreg` and `sqtest_module_lifecycle` show the correct pattern.

`reload(name)` is unload followed by load of the same path. It runs at runtime without restarting: press **F5** in the app window, or run `SquizzyStudio --reload-test N`.

### 4.2 Versions and ABI

- `SQ_PLATFORM_ABI = SQ_ABI_MAKE(major, minor)`, generated from `VERSION`. Today it is **1.0**.
- By default a module accepts `[the ABI it was compiled against, same major .0xFFFF]`.
- The module version comes from `sq_add_service(... VERSION x.y.z)` as `SQ_MODULE_VERSION`. It is stamped into `sq_module_info.version` and into the Windows VERSIONINFO resource.

### 4.3 Binary rules for interfaces (COM-style, no COM)

- Interfaces are structs of pure virtual `SQ_CALL` methods, use single inheritance from `sq::IUnknown`, and have **no** virtual destructor, overloads or data members.
- Parameters may only be: fixed-width ints, `float`/`double`, `sq_bool`, `const char*` (UTF-8, NUL-terminated), `sq_guid`, POD structs whose first field is `uint32_t struct_size`, interface pointers, and plain function-pointer callbacks plus a `void* user` argument. **Never** STL types, references, exceptions or `bool` (use `sq_bool`).
- Return `sq_result`: `SQ_OK` (0), `SQ_S_FALSE` (1), or an `SQ_E_*` code (negative). Check results with `SQ_SUCCEEDED`/`SQ_FAILED`. Text for a code comes from `sq_result_string()`.
- **Refcounting:**
  - Anything returned through `void** out` or `createInstance` is add-ref'd, and the caller releases it.
  - Methods that return a *borrowed* pointer say so in a comment, for example `IHostContext::getSettings`.
  - `sq::Ptr<T>` does this for you: use `Ptr<T>::adopt(raw)` for an add-ref'd pointer, `ptr.putVoid()` for out-params, and `ptr.as<U>()` for queryInterface.
- **Strings out:** use a caller-provided buffer. The callee writes the needed size, including the NUL, and returns `SQ_E_BUFFER_TOO_SMALL` when the buffer is too short. See `ISettings::getString` and `sq::getString()`.
- **Strings handed to the host** (class names, module info) are copied by the host. Pointers into your DLL die when it unloads.
- **Strings handed out by the host:** `sq_class_info` strings from `IClassRegistry` are interned by the app and stay valid for the life of the process (the header only promises "until the next mutation"; code to the weaker promise if you want to be portable to other hosts). `ISettings` strings come back through your own buffer.
- **No exception may cross the boundary.** `sq::createObject` and `SQ_MODULE_EXPORTS` already catch at the factory and ModuleMain level. Inside your methods, catch your own exceptions.
- **queryInterface identity:** asking any interface of an object for `IUnknown` must return the same pointer. `sq::Object<>` guarantees this.
- C callers: `sq/unknown.h` and `sq/module.h` have C vtable views of `IUnknown`, `IClassFactory` and `IHostContext`, and `tests/c_abi_probe.c` checks they compile and agree. Area interfaces are C++-only. Their vtable layout follows the same rule: declaration order, base first, `self` as the first argument.

---

## 5. Writing a service (about 30 lines)

### 5.1 Code: `services/hello/hello.cpp`

```cpp
#include "sq/sdk.hpp"
#include "sq/interfaces/hello.h"   // your interface header (5.3)

namespace {
class Hello final : public sq::Object<Hello, sq::IHello> {
 public:
  int32_t SQ_CALL twice(int32_t x) override { return 2 * x; }
};

sq_result onInit(sq::IHostContext* host) {
  sq::logf(SQ_LOG_INFO, "hello up, block size %lld",
           (long long)host->getSettings()->getInt("Engine.BlockSize", 256));
  return SQ_OK;
}

constexpr sq::ClassEntry kClasses[] = {
  sq::classEntry<Hello>(sq::hello::kHelloClassId, "Service", "Hello Service"),
};
constexpr sq::ModuleDesc kModule{
  .fourcc = SQ_FOURCC('H','E','L','O'),     // unique 4-char tag
  .name = "hello",                          // == directory name == DLL name (checked by service_exports)
  .description = "example service",
  .classes = kClasses,
  .onInit = &onInit,                        // optional
  .onShutdown = nullptr,                    // optional: undo everything onInit registered
};
}  // namespace

SQ_MODULE_EXPORTS(kModule)
```

### 5.2 CMake: `services/hello/CMakeLists.txt`

```cmake
sq_add_service(NAME hello VERSION 0.1.0 SOURCES hello.cpp
               DESCRIPTION "Hello service"
               LIBS )                        # optional extra libs (third_party targets etc.)
sq_add_test(NAME hello SOURCES tests/test_hello.cpp LIBS sq::appcore DEPENDS svc_hello)
```

That is all. The root build discovers `services/hello/`, the app build depends on `svc_hello`, and the DLL lands in `bin/services/hello.dll`. `SquizzyStudio --list-classes` shows the class.

`services/dummy/` is the reference implementation: two classes, multi-interface QI, and settings use in `onInit`.

### 5.3 Exposing interfaces to other modules

Create **one** header, `sdk/include/sq/interfaces/<area>.h`. Copy the structure of `interfaces/dummy.h`:

```cpp
#ifndef SQ_INTERFACES_HELLO_H
#define SQ_INTERFACES_HELLO_H
#include "sq/module.h"
#ifdef __cplusplus
namespace sq {
struct IHello : IUnknown {
  SQ_INTERFACE(IHello, IUnknown, "{NEW-GUID}")       // IID + base for queryInterface
  virtual int32_t SQ_CALL twice(int32_t x) = 0;
};
namespace hello { inline constexpr sq_guid kHelloClassId = sq::guid("{ANOTHER-NEW-GUID}"); }
}
#endif
#endif
```

- **New GUIDs:** run `build\agent-<you>\bin\SquizzyStudio.exe --new-guid 5` (any built `bin\` works), or `[guid]::NewGuid()` in PowerShell. Never copy a GUID from anywhere else.
- An interface is **immutable** once another agent may depend on it. To change it, add `IHello2 : IHello` (or a fresh interface) with a **new IID**, and keep serving the old one.
- Interface derivation is supported: `SQ_INTERFACE(IHello2, IHello, "...")`. `sq::Object<>` answers queryInterface for every base in the chain.
- Use the POD types from `sq/media.h` (`sq_audio_block`, `sq_stream_format`, `sq_midi_event`, `sq_latency_info`, `sq_sample_time`) for audio and MIDI data crossing DLLs. Do not invent parallel ones.
- Header names follow the area, not the implementation. The frozen area headers and who implements and consumes them are in §5.5. A consumer that needs an interface which does not exist yet must not create it: define a private interface in its own directory for now and list the request in its handoff report.

### 5.4 Using other modules

```cpp
sq::IClassRegistry* reg = sq::host()->getClassRegistry();
auto codec = sq::createInstance<sq::ICodecService>(reg, sq::codec::kWavCodecClassId); // by class ID

sq_class_info ci{}; ci.struct_size = sizeof ci;                                // or enumerate by category
for (uint32_t i = 0; i < reg->getClassCount(); ++i)
  if (reg->getClassInfo(i, &ci) == SQ_OK && std::strcmp(ci.category, "Codec") == 0) { /* ... */ }

sq::Ptr<sq::IAudioEngine> eng;                                                   // singletons
sq::host()->queryService(&sq::IAudioEngine::iid, eng.putVoid());
```

- **Singletons:** the owner calls `host->registerService(&IFoo::iid, obj)` in `onInit` and `unregisterService` in `onShutdown`. If you forget, the object stays alive, so the loader refuses every unload of your module (`SQ_E_BUSY`, rolled back) and leaves the DLL mapped at exit.
- **Dynamic classes** you add with `IClassRegistry::registerClass` (and the factory you pass) belong to your module: `unregisterClass` them in `onShutdown`, or your module cannot be unloaded.
- **Script classes:** the scripting service registers each `<ScriptClass>` with `IClassRegistry::registerClass(info, factory)`, using category `"ScriptClass"`. The registry is the same for native and script classes.
- **Never hold another module's objects across its reload.** Drop them when the provider goes away (M1 will add a module-event observer). Until then, keep a `Ptr` only for as long as you need it.

---

### 5.5 Interface ownership (frozen area headers)

All headers below are authored and owned by the **orchestrator** and are **frozen**: implement them exactly; to extend one, request a new interface with a new IID. Shared helpers live in `interfaces/common.h` (`IAttributes`, `IByteStream`, `IObserver`, `ISubject`) with header-only implementations in `sq/cpp/attributes.hpp` (`sq::makeAttributes()`) and `sq/cpp/bytestream.hpp` (`sq::makeMemoryStream()`). Every header states ownership and threading at its top: read it.

| Header | Key interfaces | Implemented by | Consumed by |
|---|---|---|---|
| `common.h` | IAttributes, IByteStream, IObserver, ISubject | anyone (sq/cpp helpers) | everyone |
| `audiodevice.h` | IAudioDriver, IAudioDevice, IAudioCallback, IAudioDeviceListener | `audiodriver-wasapi`, `audiodriver-asio` (IAudioCallback: engine) | engine, app, ui |
| `midi.h` | IMidiDriver, IMidiInputPort/OutputPort, IMidiInputCallback, IMidiFileReader/Writer | `midi-win` (ports), `codec-midifile` (SMF; the plan's `midi-file`) | engine, formats, scripting, remote |
| `codec.h` | ICodecService, IAudioFileReader, IAudioFileWriter, `sq::codec::findCodec` | `codec-wav`, `codec-flac`, `codec-mp3`, `codec-ogg`, `codec-aac` | engine, formats, sqlite-index, tools |
| `document.h` | IDocumentManager, IDocument, INode, ITrack, IClip, ITempoMap, IEditor, IAction, built-in action GUIDs `sq::doc::k*` | engine (document unit) | formats, scripting-js, ui, app, tools |
| `audioengine.h` | IAudioEngine, ITransport, IRenderTask, IRenderObserver | engine (graph/render unit) | app, ui, scripting-js, tools, remote |
| `pluginhost.h` | IPluginFormat, IPluginInstance, IPluginHostCallback, IPluginScanSink, IPluginRegistry, `sq_plugin_desc` | `pluginhost-clap`, `pluginhost-vst3`, `pluginscanner` (IPluginRegistry); IPluginHostCallback: engine | engine, ui, sqlite-index |
| `index.h` | IContentIndex, IIndexResult | `sqlite-index` | ui, tools, formats, scripting-js, pluginscanner |
| `scripting.h` | IScriptEngine, IScriptPackage, IObjectRegistry, IUrlResolver, ICommandHandler, IController, IComponent | `scripting-js` | app, ui, engine (registers the `object://doc/` resolver), formats |
| `dsp.h` | `sq_vector_ops` / `sq_dsp_vector_ops()`, ITimeStretcher, IAnalyzer, IAnalysisResult, INNRuntime, INNModel | `engine/simd` (static lib `sq::dsp` / `sq::engine_simd`), `time-stretch`, `analysis-*`, tuner | engine, plugin hosts, codecs, analysis, ui |
| `ui.h` | IThemeResolver, IRemotePageProvider, IRemotePageSet, IRemotePage | `ui/` | hardware-remote, services, scripting-js, tools |
| `remote.h` | IControlSurfaceService, IOscService, IDiscoveryService, ITempoSync, ICloudAccount | `hardware-surface` (IControlSurfaceService), `hardware-remote` (IOscService, IDiscoveryService), `link-ableton` (ITempoSync), `cloud-client` (all optional) | engine, app, ui, scripting-js |
| `surface.h` | ISurfacePump (QI from IControlSurfaceService), `sq::surface::*` | `hardware-surface` | app (`Shell::idle` pumps it) |
| `asio.h` | IAsioDriverStatus (QI from the ASIO IAudioDriver) | `audiodriver-asio` | app (`Audio.ListDevices`), ui |
| `video.h` | IVideoEngine, IVideoReader, `sq::video::kVideoEngineClassId` (category `Video`) | `video-engine` (Media Foundation; optional ffmpeg seam) | app (`Video.Probe`), ui, engine, tools |
| `dummy.h` | IDummyService, IDummyCounter | `dummy` (M0) | tests |

`tests/test_interfaces.cpp` (CTest `interfaces`) includes all of them as C++, and `tests/c_interfaces_probe.c` as C. It fails on any duplicate or null IID/class ID, on a POD whose first field is not `struct_size`, and on C/C++ size disagreement. The orchestrator adds new headers there.

**Mock the interface in your tests if the implementer is another agent.** Never wait for, link, or load another agent's unfinished module in your tests: derive a small fake from `sq::Object<Fake, IThatInterface>` in your test file (return `SQ_E_NOTIMPL` for what you do not use), and register it in an `sq::app::ClassRegistry` with `registerClass()` or hand it over directly. `tests/test_interfaces.cpp` ("codec::findCodec") shows the pattern. Integration tests against the real modules come later and belong to the orchestrator.

Singletons from these headers are registered by their implementer with `host->registerService(&IFoo::iid, obj)`: `IDocumentManager`, `IAudioEngine` (engine), `IPluginRegistry` (pluginscanner), `IScriptEngine` + `IObjectRegistry` (scripting-js), `IThemeResolver` + `IRemotePageProvider` (ui, registered by the app shell through `sq::ui::UiServices`), `INNRuntime` (the first analysis service that ships the runtime), `IControlSurfaceService` (hardware-surface), `IOscService` / `IDiscoveryService` (hardware-remote), `ITempoSync` (link-ableton), `ICloudAccount` (cloud-client), `IVideoEngine` (video-engine). Everything else is a class in the registry under the category named in its header (`AudioDriver`, `MidiDriver`, `MidiFile`, `Codec`, `PluginFormat`, `Index`, `Action`, `Command`, `ScriptClass`, `TimeStretch`, `Analysis`).

## 6. The host context (`sq::IHostContext`)

Received in `ModuleMain(INIT)`, reachable anywhere in your module through `sq::host()`, and valid until SHUTDOWN returns.

| Method | Use |
|---|---|
| `getHostAbi()`, `getHostVersion()` | Version handshake. `SQ_MODULE_EXPORTS` already rejects an out-of-range host. |
| `log(level, tag, msg)` | Or `sq::logf(SQ_LOG_INFO, "fmt", ...)`, which tags with your module name. You may also call the platform `sq_log_printf` directly. |
| `getSettings()` | Borrowed `ISettings*` (§7) |
| `getClassRegistry()` | Borrowed `IClassRegistry*` |
| `queryService`, `registerService`, `unregisterService` | Process-wide singletons keyed by IID |

Platform services are not reached through the context. Every module links `sqplatform` directly; `sq_add_service` does it for you. Include `sq/platform.h` (C) or `sq/platform.hpp` (C++ helpers).

---

## 7. Settings registry

- Keys are dotted, made of segments `[A-Za-z0-9_-]+`, and case-sensitive.
- Value types: string, int (int64), double, bool. Getters convert where sensible and fall back to the default otherwise.
- **Top-level namespaces:**
  - `Engine.*`, `Devices.*`, `Media.*`, `Platform.*`, `UI.*`
  - `Services.<module-name>.*`: a module's private settings, e.g. `Services.dummy.InitCount`
  - `Test.*`: tests only
- Defaults: the app registers core defaults in `app/src/main.cpp registerDefaults()`. A module sets its own defaults in `onInit`, and only for keys that are missing (check `has()` first).
- **Observers:** `addObserver(prefix, ISettingsObserver*, &cookie)`. Use `sq::SettingsObserverFn` to wrap a lambda. Callbacks run on the thread that made the change, outside the settings lock. Remove every observer in `onShutdown`; the registry holds a reference.
- **Persistence:**
  - The GUI saves to `%APPDATA%/SquizzyStudio/settings.xml` on exit.
  - Batch modes (`--headless`, `--selftest`, ...) persist only when `--settings FILE` is given.
  - Format: `<Settings format="1"><Section name="Engine"><Value name="BlockSize" type="int">256</Value>...`
- In scripting (M6), settings will be mirrored as `object://` URLs. Keep key names stable.
- Not realtime safe. Read settings on a non-RT thread and hand values to the audio thread through atomics.

---

## 8. Threading and realtime rules

These apply to the audio render thread, driver callbacks, the fast-lane monitor path, and any plugin or processor `process()` call:

1. **No heap allocation or free.** That means no `new`/`malloc`, no growing `std::vector`/`std::string`, and no `std::function` construction. Pre-allocate in prepare or activate.
2. **No locks.** No mutexes, condvars, `sq_mutex_*`, `sq_event_wait` or `sq_pool_*`, and no blocking on another thread. Use lock-free single-producer/single-consumer queues and atomics. `sq/platform/atomic.h` and `sq_spinlock` are allowed, the spinlock only for a handful of instructions and never while the other side can be descheduled for long.
3. **No I/O or syscalls that can block.** No logging, file access, IPC, settings, class registry, `queryInterface` on foreign objects, or `addRef`/`release` that might free memory.
4. **No exceptions.** Process code is `noexcept` in spirit.
5. Thread priority: the render thread calls `sq_thread_set_current_priority(SQ_PRIORITY_REALTIME)`. Drivers add MMCSS (Windows) or RT scheduling themselves.
6. The audio thread only touches objects whose lifetime is guaranteed by the engine's graph-swap protocol (engine-internal, documented in `engine/<x>/include`; plugin and device objects reach the RT thread only through `IAudioCallback::process` / `IPluginInstance::process`).

General threading:

- `sq_pool_*` is a FIFO worker pool for background jobs such as analysis, scanning and indexing.
- The module loader and every load/unload/reload call run on the main thread only.
- Settings, the class registry and the host service locator are thread-safe, but not RT-safe.
- Everything platform-level that is RT-safe says so in its header: `atomic.h`, `text.h`, `hash.h`, and `sq_time_monotonic_ns`.

---

## 9. ABI stamping and the lock

```text
cmake -P ci/stamp-abi.cmake check        # also the CTest test "abi_lock" and the first CI job
cmake -P ci/stamp-abi.cmake bump-minor   # additive change to sdk/include/sq/*.h or platform headers
cmake -P ci/stamp-abi.cmake bump-major   # breaking change: all modules must be rebuilt
cmake -P ci/stamp-abi.cmake update       # comment-only change
```

(On Windows without cmake on PATH, use the bundled `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`.)

Only the M0 owner or orchestrator runs `bump-*`. Area headers under `sdk/include/sq/interfaces/` are **not** covered by the lock. They are versioned by their IIDs.

---

## 10. Tests

- Framework: `tests/framework/sqtest.hpp`, in-house and dependency-free. Link `sq::test`, which provides `main()`.
  ```cpp
  #include "sqtest.hpp"
  SQ_TEST("area: what it proves") { REQUIRE(x); CHECK_EQ(a, b); CHECK_OK(sq_call()); }
  ```
  Run one binary as `bin\sqtest_<name>.exe [substring-filter] [--list]`.
- Register a test with `sq_add_test(NAME <area>_<what> SOURCES ... LIBS ... DEPENDS svc_<x>)` **in your own directory**, e.g. `services/<you>/tests/`. Test names must be globally unique, so prefix them with your area.
- Tests run with the working dir at `bin/`. Locate artifacts with `sq::specialDir(SQ_DIR_EXE)`; see `tests/test_util.hpp` for the patterns.
- Need a deliberately broken or odd module? Use `sq_add_service(NAME <x> TEST_MODULE ...)`. It goes to `bin/testmodules/` and is never auto-loaded.
- To test your service through the real loader, link `sq::appcore` and use `sq::app::Settings`, `ClassRegistry`, `HostContext` and `ModuleLoader`. `tests/test_module_reload.cpp` is the template.
- Use `LABELS gui` for tests that need a desktop session; CI runs with `-LE gui`.
- A case that cannot run on this machine (no audio endpoint, missing hardware) calls `SQ_SKIP("reason")`: it is reported `SKIP`, the exe exits 77, and `sq_add_test` registers 77 as CTest's `SKIP_RETURN_CODE`, so CTest lists the test as *Skipped*, never as passed. Do not `printf("skipped")` and return green.
- `bin/THIRD_PARTY_NOTICES.txt` is generated from every `third_party/*/LICENSE*` (`cmake/SqNotices.cmake`; CTest `third_party_notices`, `app_licenses`; `SquizzyStudio --licenses`). A newly vendored library only needs its LICENSE file in its directory.
- App-level smoke tests already registered: `app_selftest`, `app_headless`, `app_list_classes`, `app_reload_test`, `app_window_smoke`.
- **Contract tests that cover your service automatically:** `service_exports` opens every DLL in `bin/services/` and fails unless it exports exactly `GetClassFactory` + `ModuleMain`, its `sq_module_info.name` equals its file (= directory) name, its fourcc is non-zero, its ABI range contains the host's, every class has a non-null GUID, category and name, and `canUnload()` is true before anything is created. `module_lifecycle` covers the INIT/SHUTDOWN registration rules from 4.1.
- Minimal loader-level test for a service (verified to work from `services/<you>/tests/`):
  ```cpp
  #include "sq/app/module_loader.hpp"
  #include "sq/interfaces/hello.h"
  #include "sqtest.hpp"
  SQ_TEST("hello: loads through the real loader") {
    sq::app::Settings settings; sq::app::ClassRegistry reg;
    sq::app::HostContext host(settings, reg); sq::app::ModuleLoader loader(host, reg);
    std::string path = sq::joinPath(sq::joinPath(sq::specialDir(SQ_DIR_EXE), "services"),
                                    std::string("hello") + sq_dylib_suffix());
    REQUIRE(loader.load(path).ok());
    auto h = sq::createInstance<sq::IHello>(&reg, sq::hello::kHelloClassId);
    REQUIRE(h);
    CHECK_EQ(h->twice(21), 42);
  }
  ```

## 11. Tools and libraries

- `sq_add_tool(NAME sq-packager SOURCES ...)` builds `bin/sq-packager.exe`, linked with the SDK and platform.
- `sq_add_library(NAME sqengine SOURCES ... INCLUDE_DIRS include)` builds an internal static library for `engine/`, `formats/` and `ui/`. Give it an alias such as `add_library(sq::engine ALIAS sqengine)`.
- **Shared engine types:**
  - Cross-DLL POD types live in `sdk/include/sq/media.h` (core, frozen) plus the frozen area headers of section 5.5 (`document.h`, `audioengine.h`, ...).
  - In-process C++ engine types (document tree, actions, graph nodes) live in `engine/include/sq/engine/*.hpp`, behind `sq::engine`. The app links them directly; there is no DLL boundary on the hot path (plan §2.3).
  - `formats/<x>/` exposes `formats/<x>/include/sq/formats/<x>/*.hpp` behind `sq::formats_<x>`; `engine/<x>/` likewise behind `sq::engine_<x>`.
- `engine/simd` builds the static library `sq::engine_simd` (alias `sq::dsp`) that implements `sq_dsp_vector_ops()` from `interfaces/dsp.h`. Any module may link it (static, no DLL dependency). Do not define `sq_dsp_vector_ops()` anywhere else (duplicate symbol); a future `engine/dsp` must forward to it. NaN policy in every kernel set: integer conversion maps NaN to 0, `clamp` passes NaN through, `peak` ignores NaN.

## 12. third_party

See `third_party/README.md`. In short:

- One subdir per library, `third_party/<name>/`, with its LICENSE file and a `VERSION` file. Its CMakeLists defines `tp::<name>`. The root adds every `third_party/*/CMakeLists.txt` with `EXCLUDE_FROM_ALL`.
- **Shared:** `third_party/miniz` (3.0.2, MIT): zip + deflate/inflate, target `tp::miniz`, zlib-compatible names disabled (use the `mz_*` API). Use it instead of vendoring another zip/deflate library.
- Never vendor anything that is GPL or whose license needs a signed agreement. Put such SDKs behind a seam that compiles without them and activates only when the user supplies the SDK path through a CMake cache var (e.g. `-DSQ_ASIO_SDK_DIR=...`).
- Permissive licenses only for vendored source.
- LGPL and commercial components are loaded dynamically, each behind its own service.
- Nothing large in the repo.
- Link a library only from the component that needs it.

## 13. Platform layer cheat sheet

| Header | Gives you |
|---|---|
| `sq/platform/api.h` | `sq_buffer` (free with `sq_buffer_free`), `sq_alloc`/`sq_free`, `sq_result_string`, ABI query |
| `sq/platform/thread.h` | mutex, rwlock, event, TLS, threads (+priority/name), worker pool, monotonic time |
| `sq/platform/atomic.h` | header-only atomics + spinlock (RT-safe) |
| `sq/platform/system.h` | `sq_dylib_*`, special dirs, exe path, env, `sq_guid_generate` |
| `sq/platform/fs.h` | exists/mkdirs/remove/rename/copy/size/mtime/list, atomic whole-file read/write, streaming `sq_file_*` |
| `sq/platform/text.h` | UTF-8 validate/decode/encode, UTF-8 <-> UTF-16, ASCII case helpers |
| `sq/platform/xml.h` | XML DOM: parse/write/load/save, attributes, child navigation |
| `sq/platform/json.h` | JSON DOM: parse/write, build, ordered objects |
| `sq/platform/hash.h` | CRC32, SHA-256, FNV-1a, hex |
| `sq/platform/log.h` | leveled logging, file sink, custom sinks (remove them before unload) |
| `sq/platform/ipc.h` | named pipes (with timeouts), shared memory, named semaphores |
| `sq/platform.hpp` | C++ sugar: `sq::Buffer`, `readFile`/`writeFile`, `XmlNode`/`xmlParse`, `jsonParse`, `sha256Hex`, `specialDir`, `joinPath` |

All paths and strings are UTF-8, and `/` works on every OS.

## 14. Integration notes (orchestrator, after the first integration pass)

Implementer corrections to the frozen headers' comments (the headers are unchanged):

- `audioengine.h` is implemented by **engine/audio** (`sq::engine::audio::AudioEngine`), not "engine/graph". Output structs are written as `sizeof(*out)`; set `struct_size` anyway. Offline renders use a private kind 100 for click renders. The fast lane goes straight to the master (track fader/pan/mute/solo + master volume, no buses).
- `dsp.h` vector ops: **engine/simd** (see section 11).
- `midi.h` SMF reader/writer: **services/codec-midifile**. Channel events carry their bytes in `data1/data2`; `size/data` are only for meta and sysex (a non-zero `size` without `data` is rejected).
- `document.h`: an `IAction` may only commit or cancel transactions it opened itself (`SQ_E_ACCESS` otherwise), and `IDocumentManager::closeDocument` returns `SQ_E_BUSY` from inside one of that document's actions. The engine's document object answers `IDocument` **and** `IEditor` (QI either way). `kSetProperty` keys are the names `INode::getProperties` returns: tracks `name volumeDb pan mute solo armed monitor color output ...`, clips `name gainDb fadeIn fadeOut muted locked color ...`; any `user.*` key is free-form.
- `index.h`: `IIndexResult` is a true snapshot for results of up to 256 rows; larger unpaged results load metadata lazily (`SQ_E_CLOSED` after the store closes).
- `pluginhost.h`: `IPluginRegistry` is served by **services/pluginscanner** (manifest JSON + the `sq-pluginscan` child process). Offline renders compile their graphs (and so instantiate/activate the plugin inserts) on the main thread in `startRender`; the render worker only calls `process()`/`reset()`, and `AudioEngine::idle()` destroys the finished graphs on the main thread again (each compiled graph owns its own instances). The audio engine also listens to its driver (`IAudioDeviceListener`) and polls `isRunning()`: `idle()` closes a lost/reset device, notifies `engine.device` and reopens it (or the driver default).

Document-model conventions added by integration (no header change):

- Track inserts: track property `user.inserts`, one spec per line `<format>|<plugin id>|<path>[|<paramId>=<plain value>]...`. engine/audio turns specs into `PluginFx` processors through the host's `InsertResolver` (`app/src/shell.cpp`); the app command `Track.InsertPlugin` writes them (one undo step).
- Track input: `user.input_channel` (int, first device input), read by engine/audio's document builder.

The app shell (`app/include/sq/app/shell.hpp`, library `sq::shell`) is the single place that wires the in-process engine units into a host: modules first, then `edoc::registerDocumentServices`, `edit::registerEditServices`, the `AudioEngine` service (+ the null/offline driver), then the audio device from `Devices.Audio.*`. Everything user-facing goes through `Shell::execute("Category.Name", args, result)`: app built-ins, engine/edit commands, script package commands (from the `object://scripting` catalog) and native `Command` classes whose class name is `Category.Name`.

End-to-end tests (`tests/e2e/test_e2e.cpp`, CTest `e2e_milestones`) prove the milestone exit criteria across the real modules; keep them green in unfiltered builds.

Second integration pass (wave 2):

- **Surface split (decision):** `services/hardware-surface` is the only `IControlSurfaceService` (MIDI `.device`/`.surface.xml` maps, remote pages); `services/hardware-remote` keeps OSC and DNS-SD discovery only. `ISurfacePump` and `IAsioDriverStatus` were promoted unchanged (same IIDs) to `surface.h` and `asio.h`; the old service-local headers just include them.
- **Shell wiring** (`app/src/shell.cpp`): after the modules and engine services, the shell creates the `ThemeResolver`, loads the stock theme pack (RCDATA `SQTHEMES` embedded in `SquizzyStudio.exe` by `sq_skinc_embed_stock_themes`, else `<bin>/themes/sqthemes.sqtheme`, else theme folders in `<bin>/themes`), follows `UI.Theme`, and registers `IThemeResolver` + `IRemotePageProvider` + `theme://` through `sq::ui::UiServices` (released before modules unload). `Shell::idle()` calls `ISurfacePump::pump()` and runs queued OSC commands. The GUI builds the skin with the theme's `skinDefines()` (the default skin's short color names are mapped onto theme colors), uses the theme image provider, rebuilds on a theme switch, and feeds the UI Automation bridge (`setAccessibilityRoot` / `notifyAccessibilityChanged`); the GUI loop keeps pumping messages because UIA provider calls are marshalled to it.
- **New shell commands:** `Audio.ListDevices`, `Audio.SelectDevice`, `UI.SetTheme`, `Surface.List/LoadMap/LoadRemotePages/SelectPage`, `Osc.Listen/Stop/Send`, `Sync.Link`, `Account.Status/SignIn/SignOut`, `Video.Probe` (see `shell.hpp`). Inbound OSC `/sq/<Category>/<Name> [value]` may only run `Transport.*`, `Navigate.*`, `Edit.Undo/Redo` (unauthenticated UDP); the app defaults `Services.hardware-remote.OscBindAddress` to `127.0.0.1`.
- **Tests:** `sq_add_test` now sets `SKIP_RETURN_CODE 77` itself (the per-directory settings are redundant but harmless). e2e gained a VST3 insert case and an M5/M8 integration case; `app_integration_commands` drives the new commands through the real EXE.
- **VST3 test plugin** lives in `bin/testplugins-vst3/` on purpose: `bin/testplugins/` is scanned with every format by the CLAP-oriented tests, which count exactly one OK plugin.

`build.ps1 -CMakeArgs` accepts several options in one string (`"-DA=1 -DB=2"`) or as a list; from bash, quote Windows paths (`-BuildDir 'build\agent-x'`) or use forward slashes.
