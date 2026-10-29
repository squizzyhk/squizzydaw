# services/hardware-remote: OSC 1.0 over UDP (IOscService) + mDNS/DNS-SD discovery (IDiscoveryService).
# Optional package (plan 2.9, M8). Own code only, nothing vendored.
#
# sqhwremote_core: the portable, socket-free codecs (OSC messages/bundles/time tags, DNS wire
# format + DNS-SD record helpers), unit-tested on their own.
# Discovery backends: the OS DNS-SD API (Windows dnsapi DnsService*, resolved at runtime) and an
# own mDNS responder/browser on UDP 5353 as fallback (Services.hardware-remote.DiscoveryBackend).
sq_add_library(NAME sqhwremote_core
  SOURCES src/osc.cpp src/dnssd_wire.cpp
  INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include")

set(_hwr_libs sqhwremote_core)
if(WIN32)
  list(APPEND _hwr_libs ws2_32 iphlpapi)
endif()
sq_add_service(NAME hardware-remote VERSION 0.1.0
  SOURCES src/module.cpp src/osc_service.cpp src/discovery.cpp src/discovery_win.cpp src/udp.cpp
  DESCRIPTION "Squizzy Studio OSC + DNS-SD discovery service"
  LIBS ${_hwr_libs})

sq_add_test(NAME hardware_remote_osc SOURCES tests/test_osc.cpp LIBS sqhwremote_core)
sq_add_test(NAME hardware_remote_dnssd SOURCES tests/test_dnssd.cpp LIBS sqhwremote_core)
# The system DNS-SD backend over a fake dnsapi (compiles src/discovery_win.cpp into the test).
if(WIN32)
  sq_add_test(NAME hardware_remote_system_dnssd SOURCES tests/test_system_dnssd.cpp src/udp.cpp
              LIBS sqhwremote_core ws2_32 iphlpapi)
endif()
# Loader-level cases. "[loopback]" only uses 127.0.0.1 and must always pass; "[mdns]" needs a
# working multicast-capable network stack / OS DNS-SD service and is reported Skipped (exit 77)
# when the advertisement is not seen.
sq_add_test(NAME hardware_remote_module SOURCES tests/test_module.cpp LIBS sq::appcore sqhwremote_core
            DEPENDS svc_hardware-remote ARGS "[loopback]")
if(TARGET sqtest_hardware_remote_module)
  set_tests_properties(hardware_remote_module PROPERTIES SKIP_RETURN_CODE 77)
  add_test(NAME hardware_remote_mdns COMMAND sqtest_hardware_remote_module "[mdns]" WORKING_DIRECTORY "${SQ_BIN_DIR}")
  set_tests_properties(hardware_remote_mdns PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
endif()
