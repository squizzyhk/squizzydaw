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
  src/gui_overlay.cpp src/gui_overlay.hpp src/gui_menubar.cpp src/gui_transport.cpp src/gui_browser.cpp src/gui_arrange.cpp src/gui_arrange.hpp src/gui_arrange_paint.cpp src/gui_arrange_mouse.cpp src/gui_waveform.cpp src/gui_waveform.hpp
  src/gui_editor.cpp src/gui_editor.hpp src/gui_editor_notes.cpp src/gui_editor_notes.hpp
  src/gui_editor_audio.cpp src/gui_mixer.cpp src/gui_devices.cpp src/gui_browser_devices.cpp
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
    # Every new mixer / instrument / automation / clip / song / MIDI command through the CLI. The
    # helper takes (command, expected-result regex) pairs: the test passes only when every command
    # printed "run <Name>: <expected>" in order.
    function(sq_app_cli_test name driver)
      set(_args --audio-driver ${driver})
      set(_re "")
      set(_list ${ARGN})
      list(LENGTH _list _n)
      math(EXPR _last "${_n} - 1")
      foreach(_i RANGE 0 ${_last} 2)
        math(EXPR _j "${_i} + 1")
        list(GET _list ${_i} _cmd)
        list(GET _list ${_j} _exp)
        list(APPEND _args --run "${_cmd}")
        string(REGEX MATCH "^[A-Za-z]+\\.[A-Za-z]+" _nm "${_cmd}")
        string(REPLACE "." "\\." _nm "${_nm}")
        if(_re)
          string(APPEND _re ".*")
        endif()
        string(APPEND _re "run ${_nm}: ${_exp}")
      endforeach()
      add_test(NAME ${name} COMMAND SquizzyStudio ${_args} WORKING_DIRECTORY "${SQ_BIN_DIR}")
      set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION "${_re}" TIMEOUT 120)
    endfunction()
    sq_app_cli_test(app_run_mixer_commands none
      File.New "ok"
      "Track.AddBus name=Verb" "ok - added track"
      "Track.AddInstrument name=Keys" "ok - added track .* \\(builtin\\|synth\\|\\)"
      "Track.AddInsert spec=builtin|eq||0=-3" "ok - insert builtin\\|eq\\|\\|0=-3 at slot 0"
      "Track.AddInsert spec=builtin|comp|" "ok - insert builtin\\|comp\\| at slot 1"
      "Track.SetInsertParam slot=0 param=1 value=2.5 merge=true" "ok"
      "Track.BypassInsert slot=1" "ok - insert 1 bypassed"
      "Track.MoveInsert slot=1 to=0" "ok"
      "Track.AddSend bus=Verb level=-6" "ok - send 0 -> 'Verb' -6 dB"
      "Track.SetSendLevel send=0 level=-3 merge=true" "ok"
      "Track.SetInstrumentParam param=0 value=0.5 merge=true" "ok"
      "Automation.SetLane target=volume points=\"0:0 4:-12\"" "ok - 'Keys' volume: 2 point"
      "Automation.AddPoint target=send.0 ppq=2 value=-6" "ok"
      "Clip.CreateMidi start=0 length=4" "ok - MIDI clip"
      "Clip.SetNotes notes=\"0,1,60,100/1,1,64,90\"" "ok - 2 note"
      "Track.Info" "ok - 'Keys' kind=instrument instrument=builtin\\|synth\\|\\|0=0\\.5 inserts=\\[!builtin\\|comp\\| / builtin\\|eq\\|\\|0=-3\\|1=2\\.5\\] sends=\\[{[-0-9A-F]+} -3\\] automation=\\[send\\.0\\(1\\),volume\\(2\\)\\] clips=1 events=2"
      "Track.Rename name=Piano" "ok"
      "Track.SetColor color=0xFF8800FF" "ok - 'Piano' color 0xFF8800FF"
      "Track.SetOutput bus=Verb" "ok - 'Piano' -> 'Verb'"
      "Track.SetOutput bus=master" "ok"
      "Track.Move index=0" "ok"
      "Clip.Rename name=Riff" "ok"
      "Clip.SetColor color=0x00FF00FF" "ok"
      "Song.SetTempo bpm=100 position=0" "ok"
      "Song.SetSignature num=3 den=4" "ok"
      "Transport.Locate ppq=4" "ok"
      "Transport.SetLoop start=0 end=8 enabled=true" "ok"
      "Automation.RemoveLane target=volume" "ok"
      "Track.RemoveInsert slot=0" "ok"
      "Track.RemoveSend send=0" "ok"
      "Track.SetInstrument spec=builtin|synth|" "ok"
      "Track.Info track=Piano" "ok - 'Piano' kind=instrument instrument=builtin\\|synth\\| inserts=\\[builtin\\|eq\\|\\|0=-3\\|1=2\\.5\\] sends=\\[\\] automation=\\[send\\.0\\(1\\)\\]"
      "Track.AddDrums" "ok - added track .*\\(builtin\\|drums\\|\\)"
      "Track.Remove" "ok - removed track 'Drums'"
      "Track.Remove track=Verb" "ok"
      Edit.Undo "ok"
      Midi.ListInputs "ok"
      "Midi.Inject bytes=90,60,100" "ok - 3 byte"
      "Midi.Inject bytes=0x80,60,0" "ok")
    # Live MIDI through the null device: sound while stopped, then a MIDI recording creates one
    # clip (one undo step).
    # Whole GUI headless (software renderer, no display needed): build a song, open every panel,
    # menu and dialog, draw in the piano roll, switch themes, write screenshots. Runs on every OS,
    # so CI exercises the MSVC-built GUI code too.
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-output/gui")
    add_test(NAME app_gui_smoke
      COMMAND SquizzyStudio --audio-driver none --run File.New --run "Track.AddDrums name=Drums"
              --run "Clip.CreateMidi start=0 length=8" --run "Clip.SetNotes notes=0,0.5,36,110/1,0.5,38,100/2,0.5,36,110/3,0.5,38,100"
              --run "Track.AddInstrument name=Keys" --run "Clip.CreateMidi start=0 length=8"
              --run "Clip.SetNotes notes=0,1,60,100/1,1,64,90/2,1,67,80" --run "Track.AddInsert spec=builtin|reverb|"
              --run "Track.AddInsert spec=builtin|eq|" --run "Track.AddBus name=FX"
              --ui-script "${CMAKE_CURRENT_SOURCE_DIR}/tests/gui_smoke.uiscript" --size 1600x1000
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/test-output/gui")
    set_tests_properties(app_gui_smoke PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "screenshot: gui_smoke_light\\.png.*ui-script: status="
      FAIL_REGULAR_EXPRESSION "unknown op|skin:")
    add_test(NAME app_gui_smoke_hidpi
      COMMAND SquizzyStudio --audio-driver none --run File.New --run "Track.AddInstrument name=Keys"
              --screenshot gui_smoke_hidpi.png --size 2400x1500 --scale 2
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/test-output/gui")
    set_tests_properties(app_gui_smoke_hidpi PROPERTIES TIMEOUT 60 PASS_REGULAR_EXPRESSION "screenshot: gui_smoke_hidpi\\.png \\(2400x1500\\)")
    sq_app_cli_test(app_run_live_midi null
      File.New "ok"
      "Track.AddInstrument name=Keys" "ok"
      "Midi.Inject bytes=90,60,100" "ok - 3 byte\\(s\\) -> 1 live MIDI track"
      "Transport.Wait ms=150" "ok"
      Track.Info "ok - 'Keys' .* clips=0 events=0 peak=0\\.[0-9]*[1-9]"
      "Midi.Inject bytes=80,60,0" "ok"
      Track.ToggleArm "ok"
      Transport.ToggleMetronome "ok"
      Transport.Record "ok"
      "Transport.Wait ms=200" "ok"
      "Midi.Inject bytes=90,64,100" "ok"
      "Transport.Wait ms=150" "ok"
      "Midi.Inject bytes=80,64,0" "ok"
      "Transport.Wait ms=100" "ok"
      Transport.Stop "ok"
      Track.Info "ok - 'Keys' .* clips=1 events=1 "
      Edit.Undo "ok"
      Track.Info "ok - 'Keys' .* clips=0 events=0 ")
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
