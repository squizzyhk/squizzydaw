# Squizzy Studio Windows installer

A per-machine x64 MSI built with [WiX v5](https://wixtoolset.org/) (the `wix` dotnet tool plus the
`WixToolset.UI.wixext` extension).

## Files

| File | Purpose |
| --- | --- |
| `SquizzyStudio.wxs` | The package: harvests the staged app tree with the WiX v5 `Files` element, installs to `%ProgramFiles%\Squizzy Studio`, adds Start Menu and Desktop shortcuts, the `.song` association (ProgId `SquizzyStudio.Song`), the Add/Remove Programs icon and the `WixUI_InstallDir` dialog set. |
| `build-installer.ps1` | Stages `build/release/bin` exactly like the CI "Package (Windows ...)" step (app, tools, `sqplatform.dll`, `services/`, `fonts/`, `themes/`, `scripts/`, notices, README, app-local VC++ runtime; no `*.pdb`, `*.ilk`, `sqtest_*.exe` or test plugin folders) and runs `wix build`. |
| `License.rtf` | Text shown on the licence page: all rights reserved, third-party notices in `THIRD_PARTY_NOTICES.txt`. |
| `../app/resources/SquizzyStudio.ico` | The application icon (16/32/48 BMP entries + 256 PNG entry). Compiled into `SquizzyStudio.exe` by `app/resources/app.rc` (resource `IDI_APP`, the EXE's first icon, used by the shortcuts and the `.song` ProgId) and passed to `wix` as `IconIco` for the Add/Remove Programs entry. |

## Prerequisites

```powershell
dotnet tool install --global wix --version 5.0.2
wix extension add -g WixToolset.UI.wixext/5.0.2
wix extension list -g          # must list WixToolset.UI.wixext 5.0.2
```

Visual Studio 2022 is needed for the app-local VC++ runtime (`vswhere` locates the newest
`VC\Redist\MSVC\<ver>\x64\Microsoft.VC143.CRT`). Pass `-NoVcRuntime` to build without it.

## Build

```powershell
.\build.ps1 -Preset windows-release -BuildDir build\release   # produce build\release\bin
.\installer\build-installer.ps1                               # -> dist\SquizzyStudio-<version>-x64.msi
```

Parameters: `-BuildDir build/release` (default), `-OutDir dist` (default), `-Version a.b.c` (default:
`SQ_VERSION` from `VERSION`), `-Wix <path>` (default: `wix` on PATH or `%USERPROFILE%\.dotnet\tools\wix.exe`).
The script fails on any missing input or a non-zero `wix` exit code and prints the MSI path and size at the end.

The underlying command is:

```text
wix build installer\SquizzyStudio.wxs -arch x64 -d StageDir=<stage> -d Version=<a.b.c> -d LicenseRtf=installer\License.rtf -d IconIco=app\resources\SquizzyStudio.ico -ext WixToolset.UI.wixext -pdbtype none -o dist\SquizzyStudio-<a.b.c>-x64.msi
```

## Versioning and upgrades

- `UpgradeCode` in `SquizzyStudio.wxs` is fixed and must never change; `MajorUpgrade` uses it to replace an
  earlier install in place. Installing an older version over a newer one is refused.
- The MSI version is `SQ_VERSION` with any pre-release suffix removed (MSI accepts only `a.b.c`). Bump
  `VERSION` (via `ci/stamp-abi.cmake`) for every shipped build.
- `AllowSameVersionUpgrades="yes"`: a rebuild with the same version (every development build is `0.1.0`)
  also replaces the installed copy instead of installing a second product beside it. The `Upgrade` table
  row therefore includes the package's own version (`VersionMaxInclusive`); `wix msi validate` reports
  ICE61 for this, which is expected (`wix build`, which the script runs, does no ICE validation). `RemoveExistingProducts` keeps the default `afterInstallValidate` schedule, so
  the old files are removed before the new ones are copied.

## Known limits

- `.song` is registered with an `open` verb that runs `SquizzyStudio.exe "%1"`; the app opens that song
  before showing the window (`SquizzyStudio [options] [song.song]`).
- x64 only; no per-user install option (the package is `Scope="perMachine"` and asks for elevation).
