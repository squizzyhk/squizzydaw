# app/: shell EXE. sqappcore = settings, class registry, host context, module loader
# (a static lib so tests can link the exact same code). sqshell = the application shell (engine
# services + command system), shared by SquizzyStudio.exe and the end-to-end tests.
add_library(sqappcore STATIC
  src/settings.cpp
  src/class_registry.cpp
  src/host_context.cpp
  src/module_loader.cpp)
add_library(sq::appcore ALIAS sqappcore)
sq_target_defaults(sqappcore)
target_include_directories(sqappcore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(sqappcore PUBLIC sq::sdk sq::platform)

# ---- shell library ------------------------------------------------------------------------------
# The engine units are linked when they are part of the build (SQ_ONLY/SQ_SKIP may filter them);
# without them the shell still boots (module loader, scripting, browser) and says so.
add_library(sqshell STATIC src/shell.cpp include/sq/app/shell.hpp)
add_library(sq::shell ALIAS sqshell)
sq_target_defaults(sqshell)
target_link_libraries(sqshell PUBLIC sq::appcore PRIVATE sq::ui)  # ui/ is core: themes + remote pages
if(TARGET sq::engine_doc AND TARGET sq::engine_edit AND TARGET sq::engine_audio)
  target_link_libraries(sqshell PUBLIC sq::engine_doc sq::engine_edit sq::engine_audio)
  target_compile_definitions(sqshell PRIVATE SQ_SHELL_ENGINE=1)
  set(SQ_SHELL_ENGINE ON CACHE INTERNAL "")
  if(TARGET sq::formats_dawproject)
    target_link_libraries(sqshell PRIVATE sq::formats_dawproject)
    target_compile_definitions(sqshell PRIVATE SQ_SHELL_DAWPROJECT=1)
  endif()
else()
  target_compile_definitions(sqshell PRIVATE SQ_SHELL_ENGINE=0)
  set(SQ_SHELL_ENGINE OFF CACHE INTERNAL "")
  message(STATUS "sq: app shell built without engine units (engine/doc, engine/edit, engine/audio filtered out)")
endif()

# ---- theme resources compiled into the EXE --------------------------------------------------------
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
  OUTPUT "${_gen}/default_skin.h"
  COMMAND "${CMAKE_COMMAND}" -DIN=${CMAKE_CURRENT_SOURCE_DIR}/resources/default.skin.xml -DOUT=${_gen}/default_skin.h
          -DNAME=kDefaultSkin -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/default.skin.xml" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake"
  COMMENT "app: embedding default.skin.xml"
  VERBATIM)

add_executable(SquizzyStudio src/main.cpp src/gui.cpp src/gui.hpp src/gui_context.cpp src/gui_context.hpp src/gui_panels.hpp
  src/gui_overlay.cpp src/gui_overlay.hpp src/gui_menubar.cpp src/gui_transport.cpp src/gui_browser.cpp src/gui_arrange.cpp
  src/gui_editor.cpp src/gui_mixer.cpp src/gui_devices.cpp src/gui_browser_devices.cpp
  "${_gen}/default_skin.h" resources/default.skin.xml)
sq_target_defaults(SquizzyStudio)
target_include_directories(SquizzyStudio PRIVATE "${_gen}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(SquizzyStudio PRIVATE sq::shell sq::ui)
sq_set_output_dir(SquizzyStudio "${SQ_BIN_DIR}")
sq_stamp_version(SquizzyStudio SquizzyStudio ${SQ_VERSION} "Squizzy Studio" "VFT_APP")

# Stock themes (ui/themes, packed by tools/skin-compiler's sq-skinc): embedded as RCDATA "SQTHEMES" on
# Windows (Shell loads ThemePack::fromResource) and copied to <bin>/themes/sqthemes.sqtheme for
# every platform (the fallback). tools/ is configured before app/, so the helper exists unless
# tools/skin-compiler was filtered out; then the shell runs with the skin's built-in colors.
if(COMMAND sq_skinc_embed_stock_themes AND TARGET sq_stock_themes)
  sq_skinc_embed_stock_themes(SquizzyStudio)
  add_custom_command(TARGET SquizzyStudio POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SQ_BIN_DIR}/themes"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SQ_STOCK_THEMES_DIR}/sqthemes.sqtheme" "${SQ_BIN_DIR}/themes/sqthemes.sqtheme"
    VERBATIM)
  target_compile_definitions(SquizzyStudio PRIVATE SQ_APP_STOCK_THEMES=1)
  set(_sq_app_themes ON)
else()
  set(_sq_app_themes OFF)
  message(STATUS "sq: app built without the stock theme pack (tools/skin-compiler filtered out)")
endif()

# Building the app builds every discovered service module.
get_property(_svcs GLOBAL PROPERTY SQ_SERVICE_TARGETS)
if(_svcs)
  add_dependencies(SquizzyStudio ${_svcs})
endif()

if(SQ_BUILD_TESTS)
  add_test(NAME app_selftest COMMAND SquizzyStudio --selftest WORKING_DIRECTORY "${SQ_BIN_DIR}")
  add_test(NAME app_list_classes COMMAND SquizzyStudio --list-classes WORKING_DIRECTORY "${SQ_BIN_DIR}")
  set_tests_properties(app_list_classes PROPERTIES PASS_REGULAR_EXPRESSION "Dummy Service")
  add_test(NAME app_headless COMMAND SquizzyStudio --headless WORKING_DIRECTORY "${SQ_BIN_DIR}")
  set_tests_properties(app_headless PROPERTIES PASS_REGULAR_EXPRESSION "module dummy")
  add_test(NAME app_reload_test COMMAND SquizzyStudio --reload-test 25 WORKING_DIRECTORY "${SQ_BIN_DIR}")
  set_tests_properties(app_reload_test PROPERTIES PASS_REGULAR_EXPRESSION "reload-test: PASS")
  if(SQ_SHELL_ENGINE)
    # Commands through the CLI: new song, two tracks, undo one, list the command system.
    add_test(NAME app_run_commands
      COMMAND SquizzyStudio --audio-driver none --run File.New --run "Track.AddAudio name=Vox" --run Track.AddInstrument
              --run Edit.Undo --list-commands
      WORKING_DIRECTORY "${SQ_BIN_DIR}")
    set_tests_properties(app_run_commands PROPERTIES PASS_REGULAR_EXPRESSION "Edit +Split.*run Edit\\.Undo: ok")
  endif()
  if(_sq_app_themes)
    # The real EXE: embedded stock themes, theme switch, device listing, surfaces (no hardware).
    add_test(NAME app_integration_commands
      COMMAND SquizzyStudio --audio-driver none --run "UI.SetTheme name=light" --run Audio.ListDevices --run Surface.List
      WORKING_DIRECTORY "${SQ_BIN_DIR}")
    set_tests_properties(app_integration_commands PROPERTIES
      PASS_REGULAR_EXPRESSION "run UI\\.SetTheme: ok - theme: light.*run Audio\\.ListDevices: ok.*run Surface\\.List: ok")
  endif()
  if(WIN32)
    # Opens the real window for ~400 ms and closes it (needs a desktop session).
    add_test(NAME app_window_smoke
      COMMAND SquizzyStudio --quit-after 400 --audio-driver null --settings "${CMAKE_BINARY_DIR}/test-output/window_smoke_settings.xml"
      WORKING_DIRECTORY "${SQ_BIN_DIR}")
    set_tests_properties(app_window_smoke PROPERTIES TIMEOUT 30 LABELS gui)
  endif()
endif()
