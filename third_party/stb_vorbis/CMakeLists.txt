# stb_vorbis 1.22 (MIT / public domain, see LICENSE and VERSION). Unmodified upstream source; the
# added files are stb_vorbis.h (declarations view) and stb_vorbis_impl.c (implementation TU).
# Target: tp::stb_vorbis (static). Used by services/codec-ogg.
add_library(tp_stb_vorbis STATIC stb_vorbis_impl.c stb_vorbis.h)
add_library(tp::stb_vorbis ALIAS tp_stb_vorbis)
target_include_directories(tp_stb_vorbis PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(tp_stb_vorbis PUBLIC STB_VORBIS_NO_STDIO STB_VORBIS_NO_PUSHDATA_API)
set_target_properties(tp_stb_vorbis PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(tp_stb_vorbis PRIVATE /W0 /utf-8)
  target_compile_definitions(tp_stb_vorbis PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(tp_stb_vorbis PRIVATE -w)
endif()
