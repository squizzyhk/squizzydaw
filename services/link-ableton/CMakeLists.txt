# services/link-ableton: ITempoSync (remote.h) behind a licence seam. Optional package (M8).
#
# The network tempo-sync SDK is licensed separately (GPLv2+ or a proprietary licence from its
# vendor) and is NEVER vendored. Without SQ_LINK_SDK_DIR the module builds and runs an own local
# tempo/beat timeline (peers = 0). With
#   -DSQ_LINK_SDK_DIR=<SDK checkout containing include/ableton/Link.hpp>
# the SDK adapter (src/sdk_backend.cpp) is compiled in. Building that configuration means YOU
# accept the SDK's licence terms for the resulting binary.
set(SQ_LINK_SDK_DIR "" CACHE PATH "Path to the separately licensed network tempo-sync SDK; empty = local timeline only")

sq_add_library(NAME sqtempo_core
  SOURCES src/timeline.cpp src/local_backend.cpp
  INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/include")

set(_tsync_sources src/module.cpp)
set(_tsync_sdk OFF)
if(SQ_LINK_SDK_DIR)
  if(NOT EXISTS "${SQ_LINK_SDK_DIR}/include/ableton/Link.hpp")
    message(FATAL_ERROR "sq: SQ_LINK_SDK_DIR='${SQ_LINK_SDK_DIR}' does not contain include/ableton/Link.hpp")
  endif()
  set(_tsync_sdk ON)
  list(APPEND _tsync_sources src/sdk_backend.cpp)
  message(STATUS "sq: link-ableton: SDK adapter enabled (${SQ_LINK_SDK_DIR})")
else()
  message(STATUS "sq: link-ableton: local timeline only (no SQ_LINK_SDK_DIR)")
endif()

sq_add_service(NAME link-ableton VERSION 0.1.0
  SOURCES ${_tsync_sources}
  DESCRIPTION "Squizzy Studio tempo sync service"
  LIBS sqtempo_core)

if(_tsync_sdk)
  # The SDK and the core library must agree on SQ_HAVE_LINK_SDK (makeSdkBackend is defined in
  # exactly one of local_backend.cpp / sdk_backend.cpp).
  target_compile_definitions(sqtempo_core PUBLIC SQ_HAVE_LINK_SDK=1)
  if(EXISTS "${SQ_LINK_SDK_DIR}/AbletonLinkConfig.cmake")
    # The SDK's config sets CMAKE_CXX_STANDARD 17 in the including scope: keep ours (C++20).
    set(_tsync_std "${CMAKE_CXX_STANDARD}")
    include("${SQ_LINK_SDK_DIR}/AbletonLinkConfig.cmake")
    set(CMAKE_CXX_STANDARD "${_tsync_std}")
  endif()
  if(TARGET Ableton::Link)
    target_link_libraries(svc_link-ableton PRIVATE Ableton::Link)
  else()
    target_include_directories(svc_link-ableton SYSTEM PRIVATE "${SQ_LINK_SDK_DIR}/include"
                               "${SQ_LINK_SDK_DIR}/modules/asio-standalone/asio/include")
    if(WIN32)
      target_compile_definitions(svc_link-ableton PRIVATE LINK_PLATFORM_WINDOWS=1)
      target_link_libraries(svc_link-ableton PRIVATE ws2_32 iphlpapi winmm)
    elseif(APPLE)
      target_compile_definitions(svc_link-ableton PRIVATE LINK_PLATFORM_MACOSX=1)
    else()
      target_compile_definitions(svc_link-ableton PRIVATE LINK_PLATFORM_LINUX=1)
    endif()
  endif()
  if(MSVC)
    # Third-party headers are not held to our /W4 /WX.
    target_compile_options(svc_link-ableton PRIVATE /external:W0 /external:anglebrackets)
  endif()
endif()

sq_add_test(NAME link_ableton_timeline SOURCES tests/test_timeline.cpp LIBS sqtempo_core)
sq_add_test(NAME link_ableton_module SOURCES tests/test_module.cpp LIBS sq::appcore DEPENDS svc_link-ableton
            ARGS "[local]")
if(TARGET sqtest_link_ableton_module)
  target_include_directories(sqtest_link_ableton_module PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  # SDK session case: its own entry, Skipped (exit 77) in builds without SQ_LINK_SDK_DIR.
  add_test(NAME link_ableton_sdk COMMAND sqtest_link_ableton_module "[sdk]" WORKING_DIRECTORY "${SQ_BIN_DIR}")
  set_tests_properties(link_ableton_module link_ableton_sdk PROPERTIES SKIP_RETURN_CODE 77)
endif()
