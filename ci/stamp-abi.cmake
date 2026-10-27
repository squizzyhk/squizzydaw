# ABI version stamping / guard. Run with CMake in script mode (no Python needed):
#
#   cmake -P ci/stamp-abi.cmake check       # CI + ctest: fail if core ABI headers changed without a bump
#   cmake -P ci/stamp-abi.cmake print       # print version, ABI and the current header hash
#   cmake -P ci/stamp-abi.cmake update      # re-lock after a NON-functional edit (comments, docs)
#   cmake -P ci/stamp-abi.cmake bump-minor  # additive, backwards-compatible ABI change
#   cmake -P ci/stamp-abi.cmake bump-major  # breaking change (old modules will be rejected)
#
# "Core ABI headers" = everything a module binary depends on across the DLL boundary:
#   sdk/include/sq/*.h  and  platform/include/sq/platform.h + platform/include/sq/platform/*.h
# NOT included: sdk/include/sq/interfaces/*.h (per-area interfaces carry their own IIDs: a
# changed interface gets a NEW IID instead of an ABI bump) and sdk/include/sq/cpp/*.hpp
# (header-only helpers compiled into each module).
#
# The lock lives in sdk/abi.lock; the version in VERSION (read by CMake into sq/version.h and
# stamped into every module's sq_module_info.sdk_abi and Windows VERSIONINFO resource).
cmake_minimum_required(VERSION 3.21)

get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(LOCK "${ROOT}/sdk/abi.lock")
set(VERSION_FILE "${ROOT}/VERSION")

if(CMAKE_ARGC LESS 4)
  set(MODE check)
else()
  set(MODE "${CMAKE_ARGV3}")
endif()

function(read_version)
  file(STRINGS "${VERSION_FILE}" _lines REGEX "^SQ_[A-Z_]+=")
  foreach(_l IN LISTS _lines)
    string(REGEX MATCH "^([A-Z_]+)=(.*)$" _m "${_l}")
    set(${CMAKE_MATCH_1} "${CMAKE_MATCH_2}" PARENT_SCOPE)
  endforeach()
endfunction()

function(compute_hash out)
  file(GLOB _sdk RELATIVE "${ROOT}" "${ROOT}/sdk/include/sq/*.h")
  file(GLOB _plat RELATIVE "${ROOT}" "${ROOT}/platform/include/sq/platform.h" "${ROOT}/platform/include/sq/platform/*.h")
  set(_files ${_sdk} ${_plat})
  list(SORT _files)
  set(_all "")
  foreach(f IN LISTS _files)
    file(READ "${ROOT}/${f}" _c)
    string(REPLACE "\r" "" _c "${_c}")  # line-ending independent
    string(APPEND _all "== ${f}\n${_c}\n")
  endforeach()
  string(SHA256 _h "${_all}")
  list(LENGTH _files _n)
  set(${out} "${_h}" PARENT_SCOPE)
  set(${out}_COUNT "${_n}" PARENT_SCOPE)
endfunction()

function(write_lock abi hash)
  file(WRITE "${LOCK}" "# Managed by ci/stamp-abi.cmake - do not edit by hand.\n# SHA-256 of the core ABI headers at this platform ABI version.\nabi=${abi}\nsha256=${hash}\n")
endfunction()

function(set_version_abi major minor)
  file(READ "${VERSION_FILE}" _v)
  string(REGEX REPLACE "SQ_ABI_MAJOR=[0-9]+" "SQ_ABI_MAJOR=${major}" _v "${_v}")
  string(REGEX REPLACE "SQ_ABI_MINOR=[0-9]+" "SQ_ABI_MINOR=${minor}" _v "${_v}")
  file(WRITE "${VERSION_FILE}" "${_v}")
endfunction()

read_version()
set(ABI "${SQ_ABI_MAJOR}.${SQ_ABI_MINOR}")
compute_hash(HASH)

if(MODE STREQUAL "print")
  message("SQ_VERSION=${SQ_VERSION}")
  message("SQ_ABI=${ABI}")
  message("ABI_HEADERS=${HASH_COUNT}")
  message("ABI_SHA256=${HASH}")
elseif(MODE STREQUAL "update")
  write_lock("${ABI}" "${HASH}")
  message("abi.lock updated: abi=${ABI} sha256=${HASH}")
elseif(MODE STREQUAL "bump-minor")
  math(EXPR _m "${SQ_ABI_MINOR} + 1")
  set_version_abi(${SQ_ABI_MAJOR} ${_m})
  write_lock("${SQ_ABI_MAJOR}.${_m}" "${HASH}")
  message("platform ABI bumped ${ABI} -> ${SQ_ABI_MAJOR}.${_m} (reconfigure to restamp)")
elseif(MODE STREQUAL "bump-major")
  math(EXPR _M "${SQ_ABI_MAJOR} + 1")
  set_version_abi(${_M} 0)
  write_lock("${_M}.0" "${HASH}")
  message("platform ABI bumped ${ABI} -> ${_M}.0 (BREAKING: every module must be rebuilt)")
elseif(MODE STREQUAL "check")
  if(NOT EXISTS "${LOCK}")
    message(FATAL_ERROR "sdk/abi.lock missing: run `cmake -P ci/stamp-abi.cmake update`")
  endif()
  file(STRINGS "${LOCK}" _abi REGEX "^abi=")
  file(STRINGS "${LOCK}" _sha REGEX "^sha256=")
  string(REPLACE "abi=" "" _abi "${_abi}")
  string(REPLACE "sha256=" "" _sha "${_sha}")
  if(NOT _abi STREQUAL ABI)
    message(FATAL_ERROR "VERSION says ABI ${ABI} but sdk/abi.lock says ${_abi}: use bump-minor/bump-major, never edit VERSION's ABI by hand")
  endif()
  if(NOT _sha STREQUAL HASH)
    message(FATAL_ERROR
      "Core ABI headers changed but the platform ABI (${ABI}) was not bumped.\n"
      "  additive/compatible change : cmake -P ci/stamp-abi.cmake bump-minor\n"
      "  breaking change            : cmake -P ci/stamp-abi.cmake bump-major\n"
      "  comments/docs only         : cmake -P ci/stamp-abi.cmake update")
  endif()
  message("ABI lock OK: platform ABI ${ABI}, ${HASH_COUNT} headers, sha256 ${HASH}")
else()
  message(FATAL_ERROR "unknown mode '${MODE}' (check|print|update|bump-minor|bump-major)")
endif()
