# tools/packager: sq-pack, the content packager over formats/core (build / unpack / inspect /
# verify for script + skin .package, snapshot .package, .langpack, presets, .dsppreset, plain-XML
# data; extension init / install / uninstall / scan with the installdata ABI gate).
# Library sq::tools_packager (in-process API + the CLI as runCli) and bin/sq-pack(.exe).
sq_require_components(formats/core)

sq_add_library(NAME sqtools_packager
  SOURCES src/packager.cpp include/sq/tools/packager/packager.hpp
  PUBLIC_LIBS sq::formats_core
  INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(sq::tools_packager ALIAS sqtools_packager)

sq_add_tool(NAME sq-pack SOURCES src/main.cpp LIBS sq::tools_packager)
if(WIN32)
  target_link_libraries(sq-pack PRIVATE shell32)
endif()

# ---- tests -----------------------------------------------------------------------------------------
sq_add_test(NAME tools_packager_roundtrip SOURCES tests/test_packager.cpp LIBS sq::tools_packager DEPENDS sq-pack)
if(TARGET sqtest_tools_packager_roundtrip)
  set_tests_properties(tools_packager_roundtrip PROPERTIES SKIP_RETURN_CODE 77)
endif()
if(SQ_BUILD_TESTS)
  add_test(NAME tools_packager_cli_version COMMAND sq-pack --version WORKING_DIRECTORY "${SQ_BIN_DIR}")
  set_tests_properties(tools_packager_cli_version PROPERTIES PASS_REGULAR_EXPRESSION "^sq-pack \\(Squizzy Studio, platform ABI [0-9]+\\.[0-9]+\\)")
  add_test(NAME tools_packager_cli_usage COMMAND sq-pack WORKING_DIRECTORY "${SQ_BIN_DIR}")
  set_tests_properties(tools_packager_cli_usage PROPERTIES WILL_FAIL TRUE)
endif()
