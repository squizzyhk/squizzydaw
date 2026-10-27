# QuickJS-ng 0.16.2 (MIT, see LICENSE) - embeddable ECMAScript engine. Unmodified upstream
# amalgamation from https://github.com/quickjs-ng/quickjs/releases/tag/v0.16.2
# (quickjs-amalgam.zip, sha256 866bad1e2f148b6f8144a4a3d4c3f8a0754495a6920fe56a7dd698cf0ad27f65).
# quickjs-libc (OS bindings) is NOT compiled in: QJS_BUILD_LIBC stays undefined.
# Target: tp::quickjs (static). Used by services/scripting-js only.
add_library(tp_quickjs STATIC quickjs-amalgam.c quickjs.h)
add_library(tp::quickjs ALIAS tp_quickjs)
target_include_directories(tp_quickjs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(tp_quickjs PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
  target_compile_options(tp_quickjs PRIVATE /W0 /utf-8 /experimental:c11atomics)
  target_compile_definitions(tp_quickjs PRIVATE _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(tp_quickjs PRIVATE -w -funsigned-char)
  target_compile_definitions(tp_quickjs PRIVATE _GNU_SOURCE)
  find_package(Threads)
  if(Threads_FOUND)
    target_link_libraries(tp_quickjs PUBLIC Threads::Threads)
  endif()
  if(UNIX AND NOT APPLE)
    target_link_libraries(tp_quickjs PUBLIC m)
  endif()
endif()
