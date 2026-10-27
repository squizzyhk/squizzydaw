# services/analysis-beat: onset / tempo / beat detection (IAnalyzer "beat") and the realtime
# YIN-style tuner (IAnalyzer "tuner").
sq_require_components(engine/simd)

sq_add_library(NAME sqsvc_beat_core SOURCES src/beat_core.cpp src/tuner_core.cpp INCLUDE_DIRS src
  PUBLIC_LIBS sq::engine_simd)

sq_add_service(NAME analysis-beat VERSION 0.1.0
  SOURCES src/analysis_beat.cpp
  DESCRIPTION "Squizzy Studio beat/tempo analysis and tuner"
  LIBS sqsvc_beat_core)

sq_add_test(NAME analysis_beat_tempo SOURCES tests/test_beat.cpp LIBS sqsvc_beat_core sq::appcore
  DEPENDS svc_analysis-beat TIMEOUT 300)
sq_add_test(NAME analysis_beat_tuner SOURCES tests/test_tuner.cpp LIBS sqsvc_beat_core sq::appcore
  DEPENDS svc_analysis-beat)
sq_add_test(NAME analysis_beat_adversarial SOURCES tests/test_beat_adversarial.cpp LIBS sqsvc_beat_core sq::appcore
  DEPENDS svc_analysis-beat TIMEOUT 60)
foreach(_t sqtest_analysis_beat_tempo sqtest_analysis_beat_tuner sqtest_analysis_beat_adversarial)
  if(TARGET ${_t})
    target_include_directories(${_t} PRIVATE "${SQ_ROOT}/engine/simd/testing")
  endif()
endforeach()
