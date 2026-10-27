# SQLite 3.50.4 amalgamation (public domain, see LICENSE). Unmodified upstream files from
# https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
# (sha256 1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032); shell.c not vendored.
# Target: tp::sqlite (static, FTS5 enabled). Vendored by services/sqlite-index.
add_library(tp_sqlite STATIC sqlite3.c sqlite3.h)
add_library(tp::sqlite ALIAS tp_sqlite)
target_include_directories(tp_sqlite PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(tp_sqlite PRIVATE
  SQLITE_ENABLE_FTS5=1
  SQLITE_THREADSAFE=1
  SQLITE_DEFAULT_FOREIGN_KEYS=1
  SQLITE_DQS=0
  SQLITE_OMIT_LOAD_EXTENSION=1
  SQLITE_DEFAULT_WAL_SYNCHRONOUS=1)
set_target_properties(tp_sqlite PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(tp_sqlite PRIVATE /W0 /utf-8 /bigobj)
  target_compile_definitions(tp_sqlite PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(tp_sqlite PRIVATE -w)
  find_package(Threads REQUIRED)
  target_link_libraries(tp_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
  if(UNIX AND NOT APPLE)
    target_link_libraries(tp_sqlite PUBLIC m)
  endif()
endif()
