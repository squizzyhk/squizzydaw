# services/sqlite-index: content browser index (*.tags.db) implementing sq/interfaces/index.h,
# plus the IIndexScanner extension (api/sq/sqlite_index/index_scanner.h).
# Third-party: tp::sqlite (third_party/sqlite, public domain, FTS5), tp::miniz (shared, MIT).

# Public extension header for consumers (ui, tools/indexer): link sq::sqlite_index_api.
add_library(sqsvc_sqlite_index_api INTERFACE)
target_include_directories(sqsvc_sqlite_index_api INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/api")
target_link_libraries(sqsvc_sqlite_index_api INTERFACE sq::sdk)
add_library(sq::sqlite_index_api ALIAS sqsvc_sqlite_index_api)

# Implementation as a static library so the tests can drive it directly (no DLL boundary).
sq_add_library(NAME sqsvc_sqlite_index_core
  SOURCES src/content_index.cpp src/scanner.cpp src/file_metadata.cpp src/sqlite_index.hpp
  INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
  PUBLIC_LIBS sq::sqlite_index_api tp::sqlite
  LIBS tp::miniz)
set_target_properties(sqsvc_sqlite_index_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

sq_add_service(NAME sqlite-index VERSION 0.1.0 SOURCES src/module.cpp
  DESCRIPTION "Squizzy Studio content index (SQLite)"
  LIBS sqsvc_sqlite_index_core)

sq_add_test(NAME sqlite_index_core SOURCES tests/test_sqlite_index.cpp
  LIBS sqsvc_sqlite_index_core tp::miniz)
sq_add_test(NAME sqlite_index_scanner SOURCES tests/test_scanner.cpp
  LIBS sqsvc_sqlite_index_core tp::miniz)
sq_add_test(NAME sqlite_index_module SOURCES tests/test_module.cpp
  LIBS sq::appcore sq::sqlite_index_api DEPENDS svc_sqlite-index)
sq_add_test(NAME sqlite_index_hardening SOURCES tests/test_hardening.cpp
  LIBS sqsvc_sqlite_index_core sq::appcore tp::miniz)
