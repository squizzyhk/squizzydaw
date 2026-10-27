# services/hardware-surface: MIDI control surfaces (IControlSurfaceService from remote.h).
# .device + .surface.xml maps (schema in src/surface_map.hpp, example in maps/), a lock-free MIDI
# input path into a matcher on a worker thread, object:// target writes and motor-fader/LED
# feedback from ISurfacePump::pump() on the main thread, remote pages (ui.h), hot-plug reconnect.
# No link-time dependency on other services: the MIDI driver comes from the host/class registry.
set(_hws_core src/surface_map.cpp src/surface_map.hpp src/matcher.cpp src/matcher.hpp src/port_diff.hpp)

sq_add_service(NAME hardware-surface VERSION 0.1.0
  SOURCES src/service.cpp ${_hws_core}
          include/sq/services/hardware_surface/hardware_surface.h
  DESCRIPTION "Squizzy Studio MIDI control-surface service")
target_include_directories(svc_hardware-surface PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Map parsing/validation, matching, feedback encoding, hot-plug planning: compiled in, no DLL.
sq_add_test(NAME hardware_surface_map SOURCES tests/test_surface_map.cpp ${_hws_core})
# The real module through the loader, against a mock MIDI driver, object registry and pages.
sq_add_test(NAME hardware_surface_service SOURCES tests/test_surface_service.cpp
  LIBS sq::appcore DEPENDS svc_hardware-surface)
foreach(_t sqtest_hardware_surface_map sqtest_hardware_surface_service)
  if(TARGET ${_t})
    target_include_directories(${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(${_t} PRIVATE SQ_HWS_MAPS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/maps")
  endif()
endforeach()
