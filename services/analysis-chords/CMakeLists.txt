# services/analysis-chords: chromagram chord + key detection, plus the music-theory helper
# (static lib sq::theory: chord naming/parsing/identification, scales, quantisation).
sq_require_components(engine/simd)

sq_add_library(NAME sqsvc_theory SOURCES theory/src/theory.cpp INCLUDE_DIRS theory/include)
add_library(sq::theory ALIAS sqsvc_theory)

sq_add_library(NAME sqsvc_chords_core SOURCES src/chords_core.cpp INCLUDE_DIRS src
  PUBLIC_LIBS sq::theory sq::engine_simd)

sq_add_service(NAME analysis-chords VERSION 0.1.0
  SOURCES src/analysis_chords.cpp
  DESCRIPTION "Squizzy Studio chord and key analysis"
  LIBS sqsvc_chords_core)

sq_add_test(NAME analysis_chords_theory SOURCES tests/test_theory.cpp LIBS sq::theory)
sq_add_test(NAME analysis_chords_detect SOURCES tests/test_chords.cpp LIBS sqsvc_chords_core sq::appcore
  DEPENDS svc_analysis-chords TIMEOUT 300)
sq_add_test(NAME analysis_chords_adversarial SOURCES tests/test_chords_adversarial.cpp
  LIBS sqsvc_chords_core sq::appcore DEPENDS svc_analysis-chords TIMEOUT 60)
if(TARGET sqtest_analysis_chords_detect)
  target_include_directories(sqtest_analysis_chords_detect PRIVATE "${SQ_ROOT}/engine/simd/testing")
  target_include_directories(sqtest_analysis_chords_adversarial PRIVATE "${SQ_ROOT}/engine/simd/testing")
endif()
