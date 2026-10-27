# miniz 3.0.2 (MIT, see LICENSE) - zip archive + deflate/inflate. Unmodified upstream amalgamation
# from https://github.com/richgel999/miniz/releases/tag/3.0.2 (miniz-3.0.2.zip,
# sha256 ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5).
# Target: tp::miniz (static). zlib-compatible names are disabled so it can coexist with a real zlib.
add_library(tp_miniz STATIC miniz.c miniz.h)
add_library(tp::miniz ALIAS tp_miniz)
target_include_directories(tp_miniz PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(tp_miniz PUBLIC MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
if(MSVC)
  target_compile_options(tp_miniz PRIVATE /W0 /utf-8)
  target_compile_definitions(tp_miniz PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(tp_miniz PRIVATE -w)
  target_compile_definitions(tp_miniz PRIVATE _LARGEFILE64_SOURCE)
endif()
