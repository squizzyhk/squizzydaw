# Inter UI font (SIL OFL 1.1, see LICENSE). Data only: ui/ copies the files to <bin>/fonts/ and the
# software renderer loads them at runtime. No target is compiled.
set(SQ_UI_FONT_FILES "${CMAKE_CURRENT_SOURCE_DIR}/Inter-Regular.otf;${CMAKE_CURRENT_SOURCE_DIR}/Inter-SemiBold.otf" CACHE INTERNAL "UI fonts")
