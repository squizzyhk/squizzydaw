# services/audiodriver-asio: ASIO IAudioDriver behind a licence seam.
#
# The ASIO SDK is supplied separately. Without an SDK directory or local archive the module
# still builds and loads: the driver class is registered, reports zero devices and explains why
# (IAsioDriverStatus, include/sq/audiodriver-asio/asio_status.h). With
#   -DSQ_ASIO_SDK_DIR=<dir containing common/asio.h, common/asiosys.h, common/iasiodrv.h>
# the real host (src/asio_sdk_backend.cpp) is compiled in (Windows only).
#
# Everything except the SDK adapter (sample conversion, registry enumeration, device/driver
# logic, message routing) is SDK-independent, lives in the static library sqasio_core and is
# unit-tested against a fake registry and a fake driver backend.
set(SQ_ASIO_SDK_DIR "" CACHE PATH "Path to the separately supplied ASIO SDK; overrides the local SDK archive")
option(SQ_ASIO_USE_LOCAL_ARCHIVE "Use the user-supplied ASIO SDK ZIP when no SDK directory is set" ON)

# Keep extracted SDK sources in the ignored build tree. An explicit SDK directory wins.
set(_sq_asio_archive "${PROJECT_SOURCE_DIR}/asio sdk/ASIO-SDK_2.3.4_2025-10-15.zip")
if(WIN32 AND NOT SQ_ASIO_SDK_DIR AND SQ_ASIO_USE_LOCAL_ARCHIVE AND EXISTS "${_sq_asio_archive}")
  file(SHA256 "${_sq_asio_archive}" _sq_asio_hash)
  set(_sq_asio_unpack "${CMAKE_CURRENT_BINARY_DIR}/sdk/${_sq_asio_hash}")
  if(NOT EXISTS "${_sq_asio_unpack}/complete")
    file(ARCHIVE_EXTRACT INPUT "${_sq_asio_archive}" DESTINATION "${_sq_asio_unpack}")
    file(WRITE "${_sq_asio_unpack}/complete" "${_sq_asio_hash}")
  endif()
  set(SQ_ASIO_SDK_DIR "${_sq_asio_unpack}/ASIOSDK")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sq_asio_archive}")
endif()

sq_add_library(NAME sqasio_core
  SOURCES src/asio_convert.cpp src/asio_registry.cpp src/asio_driver.cpp
  INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(WIN32)
  target_link_libraries(sqasio_core PRIVATE advapi32)
endif()

set(_sq_asio_sdk_common "")
if(SQ_ASIO_SDK_DIR)
  foreach(_cand "${SQ_ASIO_SDK_DIR}/common" "${SQ_ASIO_SDK_DIR}")
    if(NOT _sq_asio_sdk_common AND EXISTS "${_cand}/iasiodrv.h" AND EXISTS "${_cand}/asio.h" AND EXISTS "${_cand}/asiosys.h")
      set(_sq_asio_sdk_common "${_cand}")
    endif()
  endforeach()
  if(NOT WIN32)
    message(WARNING "sq: SQ_ASIO_SDK_DIR is set but the ASIO host is Windows-only; building the seam only")
    set(_sq_asio_sdk_common "")
  elseif(NOT _sq_asio_sdk_common)
    message(FATAL_ERROR "sq: SQ_ASIO_SDK_DIR='${SQ_ASIO_SDK_DIR}' does not contain common/iasiodrv.h, common/asio.h and common/asiosys.h")
  endif()
endif()

set(_sq_asio_sources src/asio_module.cpp)
if(_sq_asio_sdk_common)
  list(APPEND _sq_asio_sources src/asio_sdk_backend.cpp)
  message(STATUS "sq: audiodriver-asio: ASIO host enabled (SDK headers in ${_sq_asio_sdk_common})")
else()
  message(STATUS "sq: audiodriver-asio: seam only (no SDK selected): the driver reports zero devices")
endif()

sq_add_service(NAME audiodriver-asio VERSION 0.1.0
  SOURCES ${_sq_asio_sources}
  DESCRIPTION "Squizzy Studio ASIO audio driver host"
  LIBS sqasio_core)
if(_sq_asio_sdk_common)
  target_include_directories(svc_audiodriver-asio SYSTEM PRIVATE "${_sq_asio_sdk_common}")
  target_compile_definitions(svc_audiodriver-asio PRIVATE SQ_HAVE_ASIO_SDK=1)
  target_link_libraries(svc_audiodriver-asio PRIVATE ole32 user32)
endif()

sq_add_test(NAME audiodriver_asio_convert SOURCES tests/test_convert.cpp LIBS sqasio_core)
sq_add_test(NAME audiodriver_asio_registry SOURCES tests/test_registry.cpp LIBS sqasio_core)
sq_add_test(NAME audiodriver_asio_device SOURCES tests/test_device.cpp LIBS sqasio_core)
# Loader-level cases: one CTest entry each so the hardware case shows as "Skipped" on its own.
sq_add_test(NAME audiodriver_asio_module SOURCES tests/test_module.cpp LIBS sq::appcore sqasio_core
            DEPENDS svc_audiodriver-asio ARGS "[module]")
if(TARGET sqtest_audiodriver_asio_module)
  if(_sq_asio_sdk_common)
    target_compile_definitions(sqtest_audiodriver_asio_module PRIVATE SQ_TEST_EXPECT_ASIO_HOST=1)
  else()
    target_compile_definitions(sqtest_audiodriver_asio_module PRIVATE SQ_TEST_EXPECT_ASIO_HOST=0)
  endif()
  # sq_add_test does not set SKIP_RETURN_CODE (CONTRACTS section 10 says it does): set it here.
  set_tests_properties(audiodriver_asio_module PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME audiodriver_asio_stream COMMAND sqtest_audiodriver_asio_module "[stream]" WORKING_DIRECTORY "${SQ_BIN_DIR}")
  set_tests_properties(audiodriver_asio_stream PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()
