# dr_mp3 (public domain / MIT-0, see LICENSE and VERSION). Unmodified upstream header; the only added
# file is dr_mp3_impl.c (implementation TU). Target: tp::dr_mp3 (static). Used by services/codec-mp3.
add_library(tp_dr_mp3 STATIC dr_mp3_impl.c dr_mp3.h)
add_library(tp::dr_mp3 ALIAS tp_dr_mp3)
target_include_directories(tp_dr_mp3 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(tp_dr_mp3 PUBLIC DR_MP3_NO_STDIO)
set_target_properties(tp_dr_mp3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(tp_dr_mp3 PRIVATE /W0 /utf-8)
  target_compile_definitions(tp_dr_mp3 PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(tp_dr_mp3 PRIVATE -w)
endif()
