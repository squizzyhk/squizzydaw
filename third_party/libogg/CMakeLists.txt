# libogg 1.3.5 (BSD-3-Clause, see LICENSE and VERSION). Ogg bitstream framing.
# Target: tp::libogg (static). Used by services/codec-ogg (Vorbis encoder) via tp::libvorbis.
add_library(tp_libogg STATIC src/bitwise.c src/framing.c src/crctable.h
  include/ogg/ogg.h include/ogg/os_types.h include/ogg/config_types.h)
add_library(tp::libogg ALIAS tp_libogg)
target_include_directories(tp_libogg PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(tp_libogg PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(tp_libogg PRIVATE /W0)
  target_compile_definitions(tp_libogg PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(tp_libogg PRIVATE -w)
endif()
