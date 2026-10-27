# scripting-js

JavaScript scripting service (QuickJS-ng 0.16.2, vendored in `third_party/quickjs-ng`, MIT).
Implements `sq/interfaces/scripting.h`.

## What it registers

| What | Where |
|---|---|
| `IScriptEngine` | host service |
| `IObjectRegistry` | host service (longest prefix wins; thread-safe) |
| `object://settings/<dotted.key>` | `IController` over a settings key (`getProperty("value")`, `getProperty("Child")`, `getChild`). `object://app/settings/...` falls back here unless the app registers that prefix itself. |
| `package://<id>/<path>` | bytes of a file in a loaded package (`openStream`); `package://<id>` resolves to its `IScriptPackage` |
| `object://scripting` | `IController`: `info`, `lastError`, `packages` (`;`-list), `commands` (one line per menu command: `category\tname\ttitle\t{classId}\tpackageId`), `timeLimitMs` / `memoryLimitMB` / `stackSizeKB` (settable) |
| script classes | every `<ScriptClass>` is registered in the app class registry (`IClassRegistry::registerClass`) under its own `category` (default `ScriptClass`) |

`theme://` is resolved through the registry once `ui/` registers its resolver; the scripting service does not own it.

## Script packages

A directory, a ZIP archive (e.g. `name.package`, optionally wrapped in one top-level folder), or a **binary `.package` container** (formats/core: `89 'SQPK'` header + zlib payload, as built by `sq-pack build script <dir>`), each holding a `package.xml`:

```xml
<ScriptPackage id="squizzy.trackedit" name="Track Edit" version="1.0.0" abi="1.0">
  <ClassFactory>
    <ScriptClass classID="{GUID}" category="Command" name="Track Edit: Rename"
                 sourceFile="rename.js" functionName="createRenameCommand">
      <Command category="Track" name="Rename With Pattern" title="Rename Tracks..."/>
    </ScriptClass>
  </ClassFactory>
</ScriptPackage>
```

- `abi`: the package loads only if the host platform ABI has the same major and an equal or newer minor.
- Binary `.package` containers: a **Bundle** payload is the package tree (one top-level folder tolerated); an **Xml** payload is a manifest-only package (its `package.xml`); since it carries no source files, it may not declare any `<ScriptClass>`. A bare **JavaScript** payload (no manifest), a **skin** package and a corrupted/truncated container are refused (`SQ_E_UNSUPPORTED`, `getLastError()` says why). The reader is `sq::formats_core`, linked when `formats/core` is part of the build; a filtered build without it reports binary containers as unsupported (CTest `scripting_js_binary_package` then shows *Skipped*).
- Every `<ScriptClass sourceFile>` must be present in the package (directory, ZIP or binary): a package naming a missing source is refused at load (`SQ_E_PARSE`) before any class is registered.
- Each package runs in its own JS context (its own globals). Source files run once, on first use; `functionName` must be a global function of `sourceFile`. It receives `{classId, category, name, packageId}` and returns the object.
- `<Command>` children feed the `commands` catalog for menus. Dispatch: the app calls `ICommandHandler::interpretCommand` on each `Command` class; a handler that does not own the command returns `SQ_E_NOTFOUND`.
- Loaded at start-up: `<exe>/scripts/*` (the bundled packages), `<user config>/scripts/*`, and `Services.scripting-js.PackagePath` (`;`-separated). `IScriptPackage::unload()` fails with `SQ_E_BUSY` while objects from the package are alive, and also when called from native code that a script is currently running (retry from idle).

## Script objects seen from native code

A JS object answers `IUnknown` and `IController`, plus the interfaces it declares in `interfaces: [Host.Interfaces.X]`. Without that list, they are inferred from its methods: `interpretCommand` gives `ICommandHandler`, `onNotify` gives `IObserver`, `initialize`/`terminate` give `IComponent`, and `perform` gives `IAction`. Return values: `undefined` means `SQ_OK`, `true`/`false` mean OK/disabled, and a number is passed through as an `sq_result`.

## Host API (bootstrap)

`Host.Classes` (`createInstance`, `find`, `list`), `Host.Objects` (`getObjectByUrl`, `readText`, `register`), `Host.Interfaces`, `Host.Settings` (`get`, `set`, `keys`, `observe`), `Host.Documents` (`active()` returns a `Document`, with `tracks`, `track(i)`, `tempoMap`, `markers`, `findNode`, `editor`, `edit(title, fn)`, `execute(action, params)`), `Host.Actions` (the built-in `document.h` action GUIDs), `Host.Commands` (`handler(specs)`, `invoke(category, name, args)`), `Host.Audio.peak` (via codec services), `Host.Midi.packEvents`, `Host.Values.int/double/string/guid`, `Host.Package` (`include`, `readText`), `console`, and `setTimeout`/`setInterval` (both run from `IScriptEngine::idle()`).

Value conversion for `IAttributes`:

| JS value | `IAttributes` type |
|---|---|
| integral number | int |
| other number | double |
| canonical braced GUID string | guid |
| ArrayBuffer or typed array | blob |
| array of GUID strings | blob (packed `sq_guid[]`) |
| host wrapper | object |
| plain object | nested attributes |
| object with methods | script object |

Use `Host.Values.*` to force a type.

Command `args` conventions used by the bundled packages:

- `selection`: GUIDs separated by `;` or `,`, or a packed blob. A selected track also selects its clips.
- `position`: quarter notes.

## Limits and isolation

- `Services.scripting-js.TimeLimitMs` (default 10000) applies per outermost native-to-script call. The interrupt is uncatchable by scripts.
- `MemoryLimitMB` (default 256) covers the whole runtime.
- `StackSizeKB` (default 512). Always capped to what the calling thread's native stack really has left (minus a reserve); 0 or negative means the default, never "no limit" (that would let deep recursion crash the host).
- Script exceptions never cross the ABI. They become `SQ_E_FAIL` (`SQ_E_PARSE` for syntax errors), and `getLastError()` gives `file:line: Name: message` plus the stack.
- A transaction that a script opened and left open when its call ended (an exception or a timeout) is cancelled, so the document is unchanged.
- All calls are main-thread only. Calls from other threads get `SQ_E_ACCESS`. Releasing script objects from other threads is safe: the last release is decided under the engine's live-object lock and the JS value is freed by the engine thread on its next entry/idle. `getLastError` and the `object://scripting` properties other than `info` are engine-thread state and also answer `SQ_E_ACCESS` off-thread.
- Scripts have no file or OS access (quickjs-libc is not compiled in).

## Bundled packages (`packages/`)

| Package | Commands |
|---|---|
| audioedit | Audio/Normalize (peak via codecs), Gain Up/Down/Reset Gain, Apply/Remove Fades |
| musicedit | Music/Transpose, Octave Up/Down, Quantize, Legato |
| projectedit | Project/Insert Marker, Markers From Clips, Set Tempo, Scale Tempo |
| trackedit | Track/Rename With Pattern, Duplicate, Remove Empty |
| toolset | Edit/Nudge Left/Right, Split At Position (both skip locked clips), Toggle Clip Mute |
| macrohandler | Macro/Run, Run Saved (`Services.scripting-js.Macros.<name>`); Action "Action Sequence" for `IEditor::execute` |

`kSetProperty` keys used by the bundled commands are engine/doc's property names (the ones `INode::getProperties` returns): `name`, `gainDb`, `fadeIn`, `fadeOut` (seconds), `muted`, `locked` (bools) and `color` for clips, `name`, `volumeDb`, `pan` and `color` for tracks. (Integration fix: the first version used `sq_clip_info`-style names such as `gain_db`/`flags`, which the real engine rejects.)
