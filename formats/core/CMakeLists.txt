# formats/core: content-format readers/writers (static library sq::formats_core).
# Packages (.package script/skin + snapshot), .langpack, presets, .dsppreset, plain-XML data,
# extensions with install gating. Schemas: formats/SCHEMAS.md.
sq_add_library(NAME sqformats_core
  SOURCES
    src/common.cpp
    src/detect.cpp
    src/dsppreset.cpp
    src/extension.cpp
    src/gettext.cpp
    src/jsonutil.cpp
    src/langpack.cpp
    src/metainfo.cpp
    src/package.cpp
    src/preset.cpp
    src/snapshots.cpp
    src/xmldata.cpp
    src/xmlutil.cpp
    src/zip.cpp
  INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
  LIBS tp::miniz)
add_library(sq::formats_core ALIAS sqformats_core)

sq_add_test(NAME formats_core_roundtrip
  SOURCES tests/test_roundtrip.cpp
  LIBS sq::formats_core)
sq_add_test(NAME formats_core_extension
  SOURCES tests/test_extension.cpp
  LIBS sq::formats_core)
