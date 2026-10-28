# stb_truetype (public domain / MIT, see LICENSE and VERSION). Unmodified upstream header; the only
# added file is stb_truetype_impl.c. Target: tp::stb_truetype (static). Used by ui/ (software renderer text).
add_library(tp_stb_truetype STATIC stb_truetype_impl.c stb_truetype.h)
add_library(tp::stb_truetype ALIAS tp_stb_truetype)
target_include_directories(tp_stb_truetype PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(tp_stb_truetype PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(tp_stb_truetype PRIVATE /W0 /utf-8)
  target_compile_definitions(tp_stb_truetype PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(tp_stb_truetype PRIVATE -w)
endif()
