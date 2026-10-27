# services/time-stretch: ITimeStretcher (WSOLA + phase vocoder, own implementation).
sq_require_components(engine/simd)

# Core as a static lib so the unit tests can drive it without the module boundary.
sq_add_library(NAME sqsvc_time_stretch_core
  SOURCES src/stretch_core.cpp src/algorithms.cpp
  INCLUDE_DIRS src
  PUBLIC_LIBS sq::engine_simd)

sq_add_service(NAME time-stretch VERSION 0.1.0
  SOURCES src/time_stretch.cpp
  DESCRIPTION "Squizzy Studio time stretch / pitch shift"
  LIBS sqsvc_time_stretch_core)

sq_add_test(NAME time_stretch_core SOURCES tests/test_stretch_core.cpp
  LIBS sqsvc_time_stretch_core TIMEOUT 300)
sq_add_test(NAME time_stretch_service SOURCES tests/test_stretch_service.cpp
  LIBS sq::appcore DEPENDS svc_time-stretch)
sq_add_test(NAME time_stretch_adversarial SOURCES tests/test_stretch_adversarial.cpp
  LIBS sqsvc_time_stretch_core TIMEOUT 300)
if(TARGET sqtest_time_stretch_core)
  target_include_directories(sqtest_time_stretch_core PRIVATE "${SQ_ROOT}/engine/simd/testing")
  target_include_directories(sqtest_time_stretch_adversarial PRIVATE "${SQ_ROOT}/engine/simd/testing")
  target_include_directories(sqtest_time_stretch_service PRIVATE "${SQ_ROOT}/engine/simd/testing")
endif()
