# libvorbis 1.3.7 + libvorbisenc + libvorbisfile (BSD-3-Clause, see LICENSE and VERSION). Upstream subset with one fix (PATCHES.md).
# Target: tp::libvorbis (static: decoder, vorbisenc encoder setup, vorbisfile). Used by services/codec-ogg.
set(_v lib)
add_library(tp_libvorbis STATIC
  ${_v}/mdct.c ${_v}/smallft.c ${_v}/block.c ${_v}/envelope.c ${_v}/window.c ${_v}/lsp.c ${_v}/lpc.c
  ${_v}/analysis.c ${_v}/synthesis.c ${_v}/psy.c ${_v}/info.c ${_v}/floor1.c ${_v}/floor0.c ${_v}/res0.c
  ${_v}/mapping0.c ${_v}/registry.c ${_v}/codebook.c ${_v}/sharedbook.c ${_v}/lookup.c ${_v}/bitrate.c
  ${_v}/vorbisenc.c ${_v}/vorbisfile.c
  include/vorbis/codec.h include/vorbis/vorbisenc.h include/vorbis/vorbisfile.h)
add_library(tp::libvorbis ALIAS tp_libvorbis)
target_include_directories(tp_libvorbis PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/lib")
target_link_libraries(tp_libvorbis PUBLIC tp::libogg)
set_target_properties(tp_libvorbis PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(tp_libvorbis PRIVATE /W0)
  target_compile_definitions(tp_libvorbis PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
else()
  target_compile_options(tp_libvorbis PRIVATE -w)
  if(UNIX)
    target_link_libraries(tp_libvorbis PRIVATE m)
  endif()
endif()
