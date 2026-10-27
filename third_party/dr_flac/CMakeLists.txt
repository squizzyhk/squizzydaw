# dr_flac (public domain / MIT-0, see LICENSE and VERSION). Unmodified upstream header; the only added
# file is dr_flac_impl.c (implementation TU). Target: tp::dr_flac (static). Used by services/codec-flac.
add_library(tp_dr_flac STATIC dr_flac_impl.c dr_flac.h)
add_library(tp::dr_flac ALIAS tp_dr_flac)
target_include_directories(tp_dr_flac PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(tp_dr_flac PUBLIC DR_FLAC_NO_STDIO DR_FLAC_NO_OGG)
set_target_properties(tp_dr_flac PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(tp_dr_flac PRIVATE /W0 /utf-8)
  target_compile_definitions(tp_dr_flac PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(tp_dr_flac PRIVATE -w)
endif()
