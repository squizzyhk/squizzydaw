# services/cloud-client: ICloudAccount (remote.h), offline-first. Optional package (plan 2.8, M8).
# Own code only. Network: WinHTTP (system component) behind an injectable transport; tests use
# a fake transport or a loopback (127.0.0.1) HTTP server inside the test, never a real network.
sq_add_library(NAME sqcloud_core
  SOURCES src/http.cpp src/device_flow.cpp src/store.cpp src/telemetry.cpp src/account.cpp
  INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(WIN32)
  target_link_libraries(sqcloud_core PUBLIC winhttp crypt32 bcrypt)
endif()

set(_cloud_libs sqcloud_core)
if(WIN32)
  list(APPEND _cloud_libs shell32)
endif()
sq_add_service(NAME cloud-client VERSION 0.1.0
  SOURCES src/module.cpp
  DESCRIPTION "Squizzy Studio cloud account service (optional)"
  LIBS ${_cloud_libs})

sq_add_test(NAME cloud_client_core SOURCES tests/test_core.cpp LIBS sqcloud_core)
set(_cloud_test_libs sqcloud_core)
if(WIN32)
  list(APPEND _cloud_test_libs ws2_32)
endif()
sq_add_test(NAME cloud_client_winhttp SOURCES tests/test_winhttp.cpp LIBS ${_cloud_test_libs})
sq_add_test(NAME cloud_client_module SOURCES tests/test_module.cpp LIBS sq::appcore ${_cloud_test_libs}
            DEPENDS svc_cloud-client TIMEOUT 120)
foreach(_t cloud_client_winhttp cloud_client_module)
  if(TARGET sqtest_${_t})
    set_tests_properties(${_t} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endforeach()
