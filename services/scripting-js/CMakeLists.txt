# services/scripting-js: JavaScript engine service (QuickJS-ng), host object registry, script
# packages (<ClassFactory><ScriptClass>) registered into the app class registry, and the bundled
# command packages (packages/*, copied to bin/scripts/).
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
  OUTPUT "${_gen}/bootstrap_js.h"
  COMMAND "${CMAKE_COMMAND}" -DIN=${CMAKE_CURRENT_SOURCE_DIR}/js/bootstrap.js -DOUT=${_gen}/bootstrap_js.h
          -DNAME=kBootstrapJs -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/js/bootstrap.js" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake"
  COMMENT "scripting-js: embedding bootstrap.js"
  VERBATIM)

sq_add_service(NAME scripting-js VERSION 0.1.0
  DESCRIPTION "Squizzy Studio JavaScript scripting service"
  SOURCES
    src/module.cpp
    src/engine.cpp
    src/bridge.cpp
    src/script_object.cpp
    src/package.cpp
    src/registry.cpp
    src/engine.hpp
    src/qjs.hpp
    js/bootstrap.js
    "${_gen}/bootstrap_js.h"
  LIBS tp::quickjs tp::miniz)
target_include_directories(svc_scripting-js PRIVATE "${_gen}" "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Binary .package containers (formats/core reader). formats/ is configured before services/, so the
# target exists whenever formats/core is part of the build; a filtered build without it still
# builds and reports such packages as unsupported (the test then SKIPs that case visibly).
if(TARGET sq::formats_core)
  target_link_libraries(svc_scripting-js PRIVATE sq::formats_core)
  target_compile_definitions(svc_scripting-js PRIVATE SQJS_HAVE_FORMATS_CORE=1)
  set(_sqjs_formats ON)
else()
  message(STATUS "sq: scripting-js without formats/core: binary .package containers unsupported")
  set(_sqjs_formats OFF)
endif()

# Bundled packages -> bin/scripts/<name>/ (the service auto-loads that folder).
file(GLOB_RECURSE _pkg_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/packages/*")
set(_stamp "${CMAKE_CURRENT_BINARY_DIR}/scripts.stamp")
add_custom_command(
  OUTPUT "${_stamp}"
  COMMAND "${CMAKE_COMMAND}" -E remove_directory "${SQ_BIN_DIR}/scripts"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/packages" "${SQ_BIN_DIR}/scripts"
  COMMAND "${CMAKE_COMMAND}" -E touch "${_stamp}"
  DEPENDS ${_pkg_files}
  COMMENT "scripting-js: copying bundled script packages"
  VERBATIM)
add_custom_target(scripting-js_packages DEPENDS "${_stamp}")
add_dependencies(svc_scripting-js scripting-js_packages)

sq_add_test(NAME scripting_js SOURCES tests/test_scripting.cpp tests/mock_document.hpp
  LIBS sq::appcore tp::miniz DEPENDS svc_scripting-js scripting-js_packages TIMEOUT 300)
# Binary .package loading: own CTest so a build without formats/core shows it as Skipped (exit 77).
sq_add_test(NAME scripting_js_binary_package SOURCES tests/test_binary_package.cpp tests/mock_document.hpp
  LIBS sq::appcore DEPENDS svc_scripting-js TIMEOUT 120)
if(TARGET sqtest_scripting_js_binary_package)
  set_tests_properties(scripting_js_binary_package PROPERTIES SKIP_RETURN_CODE 77)
  if(_sqjs_formats)
    target_link_libraries(sqtest_scripting_js_binary_package PRIVATE sq::formats_core)
    target_compile_definitions(sqtest_scripting_js_binary_package PRIVATE SQJS_HAVE_FORMATS_CORE=1)
  endif()
endif()
