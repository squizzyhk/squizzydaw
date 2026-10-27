# Core tests (M0). Component agents put their tests in their own dir using sq_add_test().

# Test-only module with an out-of-range ABI (goes to bin/testmodules, never auto-loaded).
sq_add_service(NAME badabi TEST_MODULE SOURCES modules/badabi/badabi.cpp VERSION 1.0.0)
# Test-only module that registers host objects in INIT and removes them in SHUTDOWN.
sq_add_service(NAME selfreg TEST_MODULE SOURCES modules/selfreg/selfreg.cpp VERSION 1.0.0)

set(_dummy svc_dummy)

sq_add_test(NAME guid SOURCES test_guid.cpp c_abi_probe.c)
# Compile check of every area interface header (as C and C++) + the sq/cpp helper objects.
sq_add_test(NAME interfaces SOURCES test_interfaces.cpp c_interfaces_probe.c LIBS sq::appcore)
sq_add_test(NAME factory_abi SOURCES test_factory_abi.cpp LIBS sq::appcore DEPENDS ${_dummy})
sq_add_test(NAME abi_reject SOURCES test_abi_reject.cpp LIBS sq::appcore DEPENDS ${_dummy} svc_badabi)
sq_add_test(NAME settings SOURCES test_settings.cpp LIBS sq::appcore)
sq_add_test(NAME xml_json SOURCES test_xml_json.cpp)
sq_add_test(NAME hash SOURCES test_hash.cpp)
sq_add_test(NAME ipc SOURCES test_ipc.cpp)
sq_add_test(NAME platform SOURCES test_platform.cpp)
sq_add_test(NAME module_reload SOURCES test_module_reload.cpp LIBS sq::appcore DEPENDS ${_dummy})
sq_add_test(NAME module_lifecycle SOURCES test_module_lifecycle.cpp LIBS sq::appcore DEPENDS ${_dummy} svc_selfreg)
# Depends on SquizzyStudio, which depends on every discovered service: all of bin/services is checked.
sq_add_test(NAME service_exports SOURCES test_service_exports.cpp DEPENDS SquizzyStudio svc_badabi svc_selfreg)

# SQ_ONLY / SQ_SKIP matching rules (cmake/SqHelpers.cmake), run in script mode.
add_test(NAME component_filter COMMAND "${CMAKE_COMMAND}" -DSQ_ROOT=${SQ_ROOT} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_component_filter.cmake")

# Vendored-library license notices ship next to the binaries (cmake/SqNotices.cmake).
add_test(NAME third_party_notices COMMAND "${CMAKE_COMMAND}" -DSQ_ROOT=${SQ_ROOT} -DNOTICES=${SQ_BIN_DIR}/THIRD_PARTY_NOTICES.txt
         -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_notices.cmake")
add_test(NAME app_licenses COMMAND SquizzyStudio --licenses WORKING_DIRECTORY "${SQ_BIN_DIR}")
set_tests_properties(app_licenses PROPERTIES PASS_REGULAR_EXPRESSION "miniz")

# Core ABI headers must not change without an ABI bump (see ci/stamp-abi.cmake).
add_test(NAME abi_lock COMMAND "${CMAKE_COMMAND}" -P "${SQ_ROOT}/ci/stamp-abi.cmake" check)

# End-to-end tests over the real app shell + every service module (plan exit criteria M1-M7).
# Needs the engine units; services it uses but which are filtered out make the affected cases fail
# loudly (they are part of the integration contract), so run it in unfiltered builds.
if(SQ_SHELL_ENGINE)
  set(_e2e_deps SquizzyStudio)
  foreach(_t IN ITEMS sq-pluginscan sq_clap_test_plugins scripting-js_packages sqvst3test_gain sq_stock_themes)
    if(TARGET ${_t})
      list(APPEND _e2e_deps ${_t})
    endif()
  endforeach()
  sq_add_test(NAME e2e_milestones SOURCES e2e/test_e2e.cpp LIBS sq::shell sq::ui DEPENDS ${_e2e_deps} TIMEOUT 600)
endif()
