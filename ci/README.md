# ci/

| File | Purpose |
|---|---|
| `stamp-abi.cmake` | ABI version guard and stamping. `cmake -P ci/stamp-abi.cmake check\|print\|update\|bump-minor\|bump-major` |
| `../.github/workflows/ci.yml` | GitHub Actions matrix: an ABI-lock job, then windows-latest, ubuntu-latest and macos-latest builds and tests |
| `../CMakePresets.json` | Presets `windows-debug`, `windows-release`, `linux`, `macos` |

GitHub only runs workflows from `.github/workflows/`, so that is where the pipeline lives. This folder holds the scripts it calls.

## How versions are stamped
- `VERSION` (repo root) holds `SQ_VERSION` and the platform ABI `SQ_ABI_MAJOR/MINOR`.
- CMake generates `build/<dir>/generated/include/sq/version.h`. It provides `SQ_PLATFORM_ABI_MAJOR/MINOR`, `SQ_VERSION_*` and `SQ_BUILD_STAMP`, plus `+<git sha>` on CI.
- Every module reports `sdk_abi` (the ABI it was compiled against) and its `[abi_min, abi_max]` range through `sq_module_info`. The loader rejects modules whose range does not contain the host ABI.
- On Windows, every DLL and EXE also gets a VERSIONINFO resource. It carries `FileVersion` (from `sq_add_service(VERSION ...)`) and the custom strings `SquizzyPlatformABI` and `SquizzyBuildStamp`.
- `sdk/abi.lock` holds the SHA-256 of the core ABI headers. The `abi_lock` CTest test and the CI `abi` job fail when those headers change without a bump.
