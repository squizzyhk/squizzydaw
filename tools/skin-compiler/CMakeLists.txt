# tools/skin-compiler: sq-skinc, the theme/skin compiler (plan 2.5: "a skin-compiler tool validates
# and packs" theme assets that the EXE embeds). Library sq::tools_skinc + CLI bin/sq-skinc.exe.
#
# Build products for the stock themes (ui/themes), produced by the sq-skinc executable itself:
#   ${SQ_STOCK_THEMES_DIR}/sqthemes.sqtheme   the pack (deterministic)
#   ${SQ_STOCK_THEMES_DIR}/sqthemes.rc        RCDATA "SQTHEMES" -> add to an EXE's sources to embed
#   ${SQ_STOCK_THEMES_DIR}/sqthemes.h         SQ_THEME_PACK_RESOURCE / _SIZE / _SHA256
#   ${SQ_STOCK_THEMES_DIR}/sqthemes.json      manifest
# Target sq_stock_themes builds them; sq_skinc_embed_stock_themes(<exe target>) wires an EXE up
# (adds the .rc on Windows + the header include dir); ThemePack::fromResource() then loads it.
sq_add_library(NAME sqtools_skinc
  SOURCES src/skinc.cpp include/sq/tools/skin_compiler/skinc.hpp
  LIBS sq::ui
  INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(sq::tools_skinc ALIAS sqtools_skinc)

sq_add_tool(NAME sq-skinc SOURCES src/main.cpp LIBS sq::tools_skinc)
if(WIN32)
  target_link_libraries(sq-skinc PRIVATE shell32)
endif()

if(NOT SQ_UI_THEMES_DIR)
  set(SQ_UI_THEMES_DIR "${SQ_ROOT}/ui/themes")
endif()
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SQ_STOCK_THEMES_DIR "${_gen}" CACHE INTERNAL "sq-skinc output for ui/themes")
file(GLOB_RECURSE _theme_sources CONFIGURE_DEPENDS "${SQ_UI_THEMES_DIR}/*")
add_custom_command(
  OUTPUT "${_gen}/sqthemes.sqtheme" "${_gen}/sqthemes.rc" "${_gen}/sqthemes.h" "${_gen}/sqthemes.json"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gen}"
  COMMAND sq-skinc --strict --quiet -o "${_gen}/sqthemes.sqtheme" --rc "${_gen}/sqthemes.rc" --header "${_gen}/sqthemes.h"
          --manifest "${_gen}/sqthemes.json" "${SQ_UI_THEMES_DIR}"
  DEPENDS sq-skinc ${_theme_sources}
  COMMENT "sq-skinc: validating and packing ui/themes"
  VERBATIM)
add_custom_target(sq_stock_themes ALL DEPENDS "${_gen}/sqthemes.sqtheme" "${_gen}/sqthemes.rc" "${_gen}/sqthemes.h")

function(sq_skinc_embed_stock_themes target)
  add_dependencies(${target} sq_stock_themes)
  target_include_directories(${target} PRIVATE "${SQ_STOCK_THEMES_DIR}")
  if(WIN32)
    target_sources(${target} PRIVATE "${SQ_STOCK_THEMES_DIR}/sqthemes.rc")
    set_source_files_properties("${SQ_STOCK_THEMES_DIR}/sqthemes.rc" PROPERTIES GENERATED TRUE)
  endif()
endfunction()

# ---- tests -----------------------------------------------------------------------------------------
sq_add_test(NAME tools_skin-compiler_roundtrip SOURCES tests/test_skinc.cpp LIBS sq::tools_skinc sq::ui DEPENDS sq_stock_themes)
if(TARGET sqtest_tools_skin-compiler_roundtrip)
  target_compile_definitions(sqtest_tools_skin-compiler_roundtrip PRIVATE
    SQ_UI_THEMES_DIR="${SQ_UI_THEMES_DIR}" SQ_STOCK_PACK="${_gen}/sqthemes.sqtheme" SQ_STOCK_RC="${_gen}/sqthemes.rc")
  set_tests_properties(tools_skin-compiler_roundtrip PROPERTIES SKIP_RETURN_CODE 77)
endif()
sq_add_test(NAME tools_skin-compiler_embedded SOURCES tests/test_embedded.cpp LIBS sq::ui DEPENDS sq_stock_themes)
if(TARGET sqtest_tools_skin-compiler_embedded)
  sq_skinc_embed_stock_themes(sqtest_tools_skin-compiler_embedded)
  target_compile_definitions(sqtest_tools_skin-compiler_embedded PRIVATE SQ_STOCK_PACK="${_gen}/sqthemes.sqtheme")
  set_tests_properties(tools_skin-compiler_embedded PROPERTIES SKIP_RETURN_CODE 77)
endif()
if(SQ_BUILD_TESTS)
  # The real executable: stock themes validate under --strict; bad input exits 1; no args exits 2.
  add_test(NAME tools_skin-compiler_cli COMMAND sq-skinc --validate-only --strict "${SQ_UI_THEMES_DIR}" WORKING_DIRECTORY "${SQ_BIN_DIR}")
  set_tests_properties(tools_skin-compiler_cli PROPERTIES PASS_REGULAR_EXPRESSION "3 theme\\(s\\), [0-9]+ file\\(s\\), 0 warning")
  add_test(NAME tools_skin-compiler_cli_usage COMMAND sq-skinc WORKING_DIRECTORY "${SQ_BIN_DIR}")
  set_tests_properties(tools_skin-compiler_cli_usage PROPERTIES WILL_FAIL TRUE)
endif()
