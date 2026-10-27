# formats/dawproject: DAWproject (open interchange format) export/import subset over the
# document.h interfaces. Static library sq::formats_dawproject. Owner: integrator.
sq_require_components(formats/core)
sq_add_library(NAME sqformats_dawproject
  SOURCES src/dawproject.cpp include/sq/formats/dawproject/dawproject.hpp
  INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
  LIBS sq::formats_core)
add_library(sq::formats_dawproject ALIAS sqformats_dawproject)
# Round trip against the real document engine (engine/doc) when it is part of the build; the
# end-to-end test in tests/e2e covers it through the app shell as well.
if(TARGET sq::engine_doc)
  sq_add_test(NAME formats_dawproject_roundtrip SOURCES tests/test_dawproject.cpp
    LIBS sq::formats_dawproject sq::formats_core sq::engine_doc sq::appcore)
endif()
