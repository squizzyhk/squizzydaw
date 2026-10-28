# services/audiodriver-alsa: ALSA PCM IAudioDriver for Linux (driver name "ALSA", app/CLI "alsa").
# libasound.so.2 is loaded with dlopen at runtime, so neither the build nor CI needs the ALSA
# development package; without the runtime library the module loads and lists 0 devices.
# Linux only: on other platforms nothing is added.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()

find_package(Threads REQUIRED)

sq_add_service(NAME audiodriver-alsa VERSION 0.1.0
  SOURCES src/alsa_module.cpp src/alsa_driver.cpp src/alsa_driver.hpp src/alsa_api.cpp src/alsa_api.hpp src/alsa_convert.hpp
  DESCRIPTION "Squizzy Studio ALSA audio driver"
  LIBS ${CMAKE_DL_LIBS} Threads::Threads)

sq_add_test(NAME audiodriver_alsa_convert SOURCES tests/test_convert.cpp)
if(TARGET sqtest_audiodriver_alsa_convert)
  target_include_directories(sqtest_audiodriver_alsa_convert PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
# One CTest entry per case: [enum] always runs, the streaming cases use ALSA's "null" PCM (needs
# only libasound + its config) and are Skipped (exit 77) when that cannot be opened.
sq_add_test(NAME audiodriver_alsa_module SOURCES tests/test_module.cpp LIBS sq::appcore DEPENDS svc_audiodriver-alsa
            ARGS "[enum]")
if(TARGET sqtest_audiodriver_alsa_module)
  foreach(_case playback capture duplex default)
    add_test(NAME audiodriver_alsa_${_case} COMMAND sqtest_audiodriver_alsa_module "[${_case}]"
             WORKING_DIRECTORY "${SQ_BIN_DIR}")
    set_tests_properties(audiodriver_alsa_${_case} PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
  endforeach()
endif()
