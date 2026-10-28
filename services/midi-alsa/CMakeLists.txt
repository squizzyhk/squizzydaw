# services/midi-alsa: IMidiDriver over the ALSA sequencer, the platform MIDI service on Linux
# (also registered as the host's IMidiDriver service). libasound.so.2 is loaded with dlopen at
# runtime, so no ALSA development package is needed. Linux only: nothing is added elsewhere.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()

find_package(Threads REQUIRED)

sq_add_service(NAME midi-alsa VERSION 0.1.0
  SOURCES src/driver.cpp src/seq_api.cpp src/seq_api.hpp src/seq_event.hpp src/midi_alsa.hpp
  DESCRIPTION "Squizzy Studio ALSA sequencer MIDI driver"
  LIBS ${CMAKE_DL_LIBS} Threads::Threads)

sq_add_test(NAME midi_alsa_codec SOURCES tests/test_codec.cpp)
if(TARGET sqtest_midi_alsa_codec)
  target_include_directories(sqtest_midi_alsa_codec PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
# [enum] always runs; [loopback] needs /dev/snd/seq + a "Midi Through" port, else Skipped (77).
sq_add_test(NAME midi_alsa_module SOURCES tests/test_module.cpp LIBS sq::appcore DEPENDS svc_midi-alsa ARGS "[enum]")
if(TARGET sqtest_midi_alsa_module)
  add_test(NAME midi_alsa_loopback COMMAND sqtest_midi_alsa_module "[loopback]" WORKING_DIRECTORY "${SQ_BIN_DIR}")
  set_tests_properties(midi_alsa_loopback PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()
