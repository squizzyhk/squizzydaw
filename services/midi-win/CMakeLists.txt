# services/midi-win: IMidiDriver over WinMM (Windows) + built-in loopback cables (all platforms).
# Output ports schedule by time_ns on a worker thread with a lock-free send(); inputs deliver from
# the driver thread through an RT-safe gate. Non-Windows builds expose the loopback ports only.
# Hot-plug: a watcher thread (device notifications on Windows + polling fallback) runs while
# IMidiDeviceListeners are registered.
sq_add_service(NAME midi-win VERSION 0.2.0
  SOURCES driver.cpp ports.cpp ports.hpp rt_util.hpp winmm.cpp hotplug.cpp hotplug.hpp
  DESCRIPTION "Squizzy Studio Windows MIDI driver")
if(WIN32)
  target_link_libraries(svc_midi-win PRIVATE winmm cfgmgr32)
endif()

sq_add_test(NAME midi_win_loopback SOURCES tests/test_midi_win.cpp LIBS sq::appcore DEPENDS svc_midi-win)
sq_add_test(NAME midi_win_recycler SOURCES tests/test_recycler.cpp)
sq_add_test(NAME midi_win_hotplug SOURCES tests/test_hotplug.cpp hotplug.cpp LIBS sq::appcore DEPENDS svc_midi-win)
if(TARGET sqtest_midi_win_hotplug)
  target_include_directories(sqtest_midi_win_hotplug PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  if(WIN32)
    target_link_libraries(sqtest_midi_win_hotplug PRIVATE cfgmgr32)
  endif()
endif()
