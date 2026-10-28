# services/audiodriver-pulse: PulseAudio IAudioDriver for Linux (driver name "Pulse", app/CLI
# "pulse") over the simple API; libpulse-simple.so.0 is loaded with dlopen at runtime (no -dev
# package needed). Linux only: on other platforms nothing is added.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()

find_package(Threads REQUIRED)

sq_add_service(NAME audiodriver-pulse VERSION 0.1.0
  SOURCES src/pulse_module.cpp src/pulse_driver.cpp src/pulse_driver.hpp
  DESCRIPTION "Squizzy Studio PulseAudio driver"
  LIBS ${CMAKE_DL_LIBS} Threads::Threads)

# [enum] always runs; [stream] needs a reachable PulseAudio/PipeWire server, else Skipped (77).
sq_add_test(NAME audiodriver_pulse_module SOURCES tests/test_module.cpp LIBS sq::appcore DEPENDS svc_audiodriver-pulse
            ARGS "[enum]")
if(TARGET sqtest_audiodriver_pulse_module)
  add_test(NAME audiodriver_pulse_stream COMMAND sqtest_audiodriver_pulse_module "[stream]" WORKING_DIRECTORY "${SQ_BIN_DIR}")
  set_tests_properties(audiodriver_pulse_stream PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()
