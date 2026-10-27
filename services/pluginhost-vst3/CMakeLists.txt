# pluginhost-vst3: VST3 plugin host adapter (IPluginFormat "vst3") over the vendored, MIT-licensed
# VST3 pluginterfaces (third_party/vst3, tp::vst3) + an original VST3 test plugin.
sq_add_service(NAME pluginhost-vst3 VERSION 0.1.0
  SOURCES vst3_format.cpp vst3_instance.cpp vst3_module.cpp vst3_hostobjects.cpp vst3_common.cpp vst3_iids.cpp
  DESCRIPTION "Squizzy Studio VST3 plugin host adapter"
  LIBS tp::vst3 ${CMAKE_DL_LIBS})

if(SQ_BUILD_TESTS)
  # VST3 bundle layout: <name>.vst3/Contents/<arch>/<name>[.vst3|.so]. Must match archDirs() in
  # vst3_module.cpp.
  if(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
      set(_sq_vst3_arch "arm64-win")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(_sq_vst3_arch "x86_64-win")
    else()
      set(_sq_vst3_arch "x86-win")
    endif()
    set(_sq_vst3_ext ".vst3")
  elseif(APPLE)
    set(_sq_vst3_arch "MacOS")
    set(_sq_vst3_ext "")
  else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
      set(_sq_vst3_arch "aarch64-linux")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
      set(_sq_vst3_arch "x86_64-linux")
    else()
      set(_sq_vst3_arch "i386-linux")
    endif()
    set(_sq_vst3_ext ".so")
  endif()

  # Own folder (not bin/testplugins): the pluginscanner and e2e tests scan that one with every
  # loaded format and count the plugins they find.
  set(_sq_vst3_bundle "${SQ_BIN_DIR}/testplugins-vst3/sq-test-gain.vst3")
  add_library(sqvst3test_gain MODULE testplugins/test_vst3_gain.cpp)
  sq_target_defaults(sqvst3test_gain)
  target_link_libraries(sqvst3test_gain PRIVATE tp::vst3)
  set_target_properties(sqvst3test_gain PROPERTIES OUTPUT_NAME "sq-test-gain" PREFIX "" SUFFIX "${_sq_vst3_ext}")
  sq_set_output_dir(sqvst3test_gain "${_sq_vst3_bundle}/Contents/${_sq_vst3_arch}")

  sq_add_test(NAME pluginhost_vst3 SOURCES tests/test_vst3_host.cpp
    LIBS sq::appcore
    DEPENDS svc_pluginhost-vst3 sqvst3test_gain)
  if(TARGET sqtest_pluginhost_vst3)
    target_compile_definitions(sqtest_pluginhost_vst3 PRIVATE
      SQ_VST3_TEST_ARCH="${_sq_vst3_arch}" SQ_VST3_TEST_BINEXT="${_sq_vst3_ext}")
  endif()

  # Opt-in (SQ_VST3_SMOKE_DIR): exercises locally installed third-party plugins; Skipped otherwise.
  sq_add_test(NAME pluginhost_vst3_installed SOURCES tests/test_vst3_installed.cpp
    LIBS sq::appcore
    DEPENDS svc_pluginhost-vst3)
  # sq_add_test does not set SKIP_RETURN_CODE yet (CONTRACTS section 10 says it does): set it here so
  # the opt-in case shows as Skipped, never as passed or failed.
  if(TEST pluginhost_vst3_installed)
    set_tests_properties(pluginhost_vst3_installed PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
  endif()
endif()
