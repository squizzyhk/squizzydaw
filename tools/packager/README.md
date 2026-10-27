# tools/packager: `sq-pack`

Command-line packager over `formats/core` (`sq::formats_core`). Library `sq::tools_packager`
(`include/sq/tools/packager/packager.hpp`: `build`, `unpack`, `inspect`, `verify`,
`extensionInit`, and the whole CLI as `runCli`) plus `bin/sq-pack(.exe)`.

```
sq-pack build <type> <src> -o <out> [--info key=value]...
    type: script | skin | snapshot | langpack | preset | multipreset | instrument | trackpreset
sq-pack unpack <file> -o <dir> [--po]
sq-pack inspect <file|extension-dir>
sq-pack verify <file|extension-dir> [--canonical] [--no-gate] [--app id] [--abi M.m] [--os os] [--arch arch]
sq-pack ext init <dir> --id <id> --version <v> [--name ..] [--vendor ..] [--description ..]
                 [--target-app id] [--abi-min M.m] [--abi-max M.m|M.*] [--os os|*] [--arch arch|*]
sq-pack ext install <dir> <extensions-root> [gate options]
sq-pack ext uninstall <extensions-root> <id>
sq-pack ext scan <extensions-root> [gate options]
```

Exit codes: `0` ok, `1` invalid content / failed verification, `2` usage, `3` ABI mismatch
(installdata range or `package.xml abi` excludes the host), `4` unsupported (other application,
OS/arch, or a container version/format this build does not know).

## Containers and source layouts

| type | container | `build` input (= what `unpack` writes) |
|---|---|---|
| script | binary `.package`, kind script | a tree (Bundle payload) with `package.xml` at the root or in one top folder; every `<ScriptClass sourceFile>` must exist. Or one `.xml` file: a manifest-only Xml payload, which may not declare `<ScriptClass>` (it has no sources). A bare `.js` script package is refused (the script engine cannot load it); `verify` rejects one unless `--no-gate`. The `package.xml` id (or, without one, the file name) must be a valid engine package id |
| skin | binary `.package`, kind skin | a tree; every `.xml` must be well-formed. Or one `.xml` / `.js` file |
| snapshot | ZIP `.package` + `snapshots.json` | `snapshots.json` + the images it lists, or just images (`.png`/`.webp`; id = path without extension, `@2x` = scale 2) plus `--info` |
| langpack | ZIP `.langpack` | `metainfo.xml`, `catalogs/<module>.mo` **or** `.po` (compiled), `properties/<name>.properties` |
| preset kinds | ZIP `.preset` / `.multipreset` / `.instrument` / `.trackpreset` | `metainfo.xml`, `preset.xml`, `slots/<i>.state`, `attachments/...`; the tree's kind must match the type |
| extension | directory | `ext init` writes `metainfo.xml` + `installdata.xml`; add the payload files |

`inspect` / `verify` also read `.dsppreset`, `.chords`, `.musicalscale` and `.colorscheme`.

## Determinism

Inputs are sorted, ZIP members get fixed timestamps (formats/core writer), and every ZIP-based
build is canonicalised through the format's reader and writer. The same tree always gives the same
bytes, and `unpack` followed by `build` reproduces a container byte for byte (tested for every
container). `verify --canonical` checks that read -> write reproduces a file exactly (true for
anything `sq-pack` built). Files a container's schema would drop (a stray file in a langpack or
preset tree, an image `snapshots.json` does not list) are refused instead of silently lost. `unpack` needs an empty (or new) output directory and refuses, before writing anything, members whose names collide case-insensitively or as file vs. folder, or are not portable (Windows device names, trailing dot or space).

## Gating

- Extensions: `installdata.xml` targets (app id, ABI range, OS, arch) are checked by `verify`,
  `ext install` and `ext scan` against this build's identity (`squizzy.studio`, `SQ_PLATFORM_ABI`,
  compile-time OS/arch) or the `--app/--abi/--os/--arch` overrides. A rejected install writes
  nothing. `inspect` reports the gate result but never fails on it.
- Script packages: `verify` applies the script engine's rule to `package.xml abi="M.m"` (same
  major, minor <= host minor). `--no-gate` skips both checks.

## Tests

- `tools_packager_roundtrip`: build -> verify (canonical) -> unpack -> build byte-identical for
  script bundles, langpacks (from `.po` and from `.mo`), all four preset kinds and snapshot
  packages; manifest/source checks; ABI / app / platform gates; extension install/scan/uninstall;
  corrupted and truncated input; and the real `sq-pack` executable (same bytes as the library,
  exit codes).
- `tools_packager_cli_version`, `tools_packager_cli_usage`: the executable's banner and usage exit.
