# services/pluginscanner: IPluginRegistry service + the out-of-process scanner tool sq-pluginscan
# (one child process per plugin file; crashes/hangs are quarantined). The service talks to the
# format adapters only through the class registry (category "PluginFormat").
sq_add_tool(NAME sq-pluginscan SOURCES src/scan_main.cpp src/manifest.cpp src/manifest.hpp LIBS sq::appcore)

sq_add_service(NAME pluginscanner VERSION 0.1.0
  SOURCES src/registry.cpp src/manifest.cpp src/manifest.hpp src/spawn.cpp src/spawn.hpp
  DESCRIPTION "Squizzy Studio plugin registry and out-of-process scanner")
add_dependencies(svc_pluginscanner sq-pluginscan)

if(SQ_BUILD_TESTS AND TARGET svc_pluginhost-clap)
  sq_add_test(NAME pluginscanner_quarantine SOURCES tests/test_scanner.cpp
    LIBS sq::appcore
    DEPENDS svc_pluginscanner svc_pluginhost-clap sq-pluginscan sq_clap_test_plugins
    TIMEOUT 180)
endif()
