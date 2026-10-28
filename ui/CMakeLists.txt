# squi: backend-neutral UI layer (window + renderer abstraction, flexbox layout, retained widgets,
# skin XML, accessibility tree, portable raster backend) plus the M5 pieces: themes (theme://
# resolver, @Nx variants, colors/metrics, theme packs), image decoding (WIC on Windows, portable
# PNG everywhere), hardware remote pages from skins, and the UI Automation bridge (Windows).
# Windows backend: Win32 + GDI. Skia/Yoga backends are not built yet (see STATUS.md); keep
# include/sq/ui/*.hpp neutral (uia.hpp only exposes opaque pointers).
sq_add_library(NAME squi
  SOURCES
    src/window_win32.cpp
    src/window_x11.cpp
    src/window_null.cpp
    src/soft_renderer.cpp
    src/render_common.cpp
    src/raster.cpp
    src/layout.cpp
    src/widgets.cpp
    src/skin.cpp
    src/image.cpp
    src/image_wic.cpp
    src/theme.cpp
    src/remote_pages.cpp
    src/uia_win32.cpp
    include/sq/ui/ui.hpp
    include/sq/ui/image.hpp
    include/sq/ui/theme.hpp
    include/sq/ui/remote_pages.hpp
    include/sq/ui/uia.hpp
  LIBS tp::miniz tp::stb_truetype
  INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(sq::ui ALIAS squi)
# X11 window (Linux/BSD) presenting the software renderer. Optional: without X11 headers the app
# still builds and runs headless.
if(UNIX AND NOT APPLE)
  find_package(X11)
  if(X11_FOUND)
    target_compile_definitions(squi PRIVATE SQ_UI_X11=1)
    target_link_libraries(squi PRIVATE X11::X11)
    message(STATUS "sq: ui: X11 window backend")
  else()
    message(STATUS "sq: ui: no X11 (install libx11-dev): no window backend, headless only")
  endif()
endif()
# UI fonts (third_party/inter-font) next to the binaries: <bin>/fonts/.
if(SQ_UI_FONT_FILES)
  file(MAKE_DIRECTORY "${SQ_BIN_DIR}/fonts")
  foreach(_f IN LISTS SQ_UI_FONT_FILES)
    get_filename_component(_n "${_f}" NAME)
    configure_file("${_f}" "${SQ_BIN_DIR}/fonts/${_n}" COPYONLY)
  endforeach()
endif()
if(WIN32)
  target_link_libraries(squi PRIVATE user32 gdi32 ole32 oleaut32 windowscodecs uiautomationcore)
endif()

# Stock themes (dark, light and their shared base). tools/skin-compiler packs them into the
# resource blob the EXE embeds; tests read the sources directly.
set(SQ_UI_THEMES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/themes" CACHE INTERNAL "ui/ stock theme sources")

function(_squi_test name)
  cmake_parse_arguments(T "" "" "ARGS" ${ARGN})
  sq_add_test(NAME ${name} SOURCES ${T_UNPARSED_ARGUMENTS} LIBS sq::ui ARGS ${T_ARGS})
  if(TARGET sqtest_${name})
    target_compile_definitions(sqtest_${name} PRIVATE SQ_UI_THEMES_DIR="${SQ_UI_THEMES_DIR}")
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endfunction()

_squi_test(ui_core tests/test_ui.cpp)
_squi_test(ui_image tests/test_image.cpp)
_squi_test(ui_theme tests/test_theme.cpp)
_squi_test(ui_remote tests/test_remote.cpp)
# ui_uia runs the provider cases; the real-window + UIA client case needs a desktop session.
_squi_test(ui_uia tests/test_uia.cpp ARGS "uia provider")
if(TARGET sqtest_ui_uia)
  add_test(NAME ui_uia_window COMMAND sqtest_ui_uia "uia window" WORKING_DIRECTORY "${SQ_BIN_DIR}")
  set_tests_properties(ui_uia_window PROPERTIES TIMEOUT 60 LABELS gui SKIP_RETURN_CODE 77)
endif()
# Adversarial hardening: bombs, deep skins, UIA thread affinity (the window case needs a desktop).
_squi_test(ui_m5_hardening tests/test_m5_hardening.cpp ARGS "hardening:")
if(TARGET sqtest_ui_m5_hardening)
  add_test(NAME ui_m5_hardening_uia COMMAND sqtest_ui_m5_hardening "hardening uia" WORKING_DIRECTORY "${SQ_BIN_DIR}")
  set_tests_properties(ui_m5_hardening_uia PROPERTIES TIMEOUT 60 LABELS gui SKIP_RETURN_CODE 77)
  if(WIN32)
    target_link_libraries(sqtest_ui_m5_hardening PRIVATE psapi ole32 oleaut32 uiautomationcore user32)
  endif()
endif()
if(TARGET sqtest_ui_image)
  target_link_libraries(sqtest_ui_image PRIVATE tp::miniz)  # builds interlaced / odd-layout PNGs
endif()
if(TARGET sqtest_ui_theme)
  target_link_libraries(sqtest_ui_theme PRIVATE sq::appcore)  # UiServices against the real host
endif()
if(TARGET sqtest_ui_uia AND WIN32)
  target_link_libraries(sqtest_ui_uia PRIVATE ole32 oleaut32 uiautomationcore user32)
endif()
