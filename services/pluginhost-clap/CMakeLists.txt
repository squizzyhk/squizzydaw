# pluginhost-clap: CLAP plugin host adapter (IPluginFormat "clap") + CLAP test plugins.
sq_add_service(NAME pluginhost-clap VERSION 0.1.0
  SOURCES clap_format.cpp clap_instance.cpp clap_library.cpp
  DESCRIPTION "Squizzy Studio CLAP plugin host adapter"
  LIBS tp::clap)

if(SQ_BUILD_TESTS)
  # Test plugins (original code) -> bin/testplugins/*.clap. Never auto-loaded by anything.
  set(SQ_TEST_PLUGINS_DIR "${SQ_BIN_DIR}/testplugins")
  function(sq_clap_test_plugin name)
    cmake_parse_arguments(A "" "" "SOURCES;DEFINES" ${ARGN})
    add_library(${name} MODULE ${A_SOURCES})
    sq_target_defaults(${name})
    target_link_libraries(${name} PRIVATE tp::clap)
    target_compile_definitions(${name} PRIVATE ${A_DEFINES})
    string(REPLACE "sqclaptest_" "sq-test-" _out "${name}")
    set_target_properties(${name} PROPERTIES OUTPUT_NAME "${_out}" PREFIX "" SUFFIX ".clap")
    sq_set_output_dir(${name} "${SQ_TEST_PLUGINS_DIR}")
  endfunction()
  sq_clap_test_plugin(sqclaptest_gain SOURCES testplugins/test_gain.cpp)
  sq_clap_test_plugin(sqclaptest_crash SOURCES testplugins/test_misbehave.cpp DEFINES SQ_MISBEHAVE_CRASH)
  sq_clap_test_plugin(sqclaptest_hang SOURCES testplugins/test_misbehave.cpp DEFINES SQ_MISBEHAVE_HANG)
  add_custom_target(sq_clap_test_plugins DEPENDS sqclaptest_gain sqclaptest_crash sqclaptest_hang)

  sq_add_test(NAME pluginhost_clap SOURCES tests/test_clap_host.cpp
    LIBS sq::appcore
    DEPENDS svc_pluginhost-clap sqclaptest_gain)
endif()
