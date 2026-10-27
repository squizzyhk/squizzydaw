# services/analysis-stems: neural stem separation + the INNRuntime prediction dispatcher seam.
# ONNX Runtime is never linked or vendored as a binary: only its MIT C API header
# (third_party/onnxruntime-headers) is compiled in, and the library is opened at run time if the
# user has it (see src/analysis_stems.cpp). Without it the runtime is simply "unavailable".
sq_require_components(engine/simd)

sq_add_library(NAME sqsvc_stems_core SOURCES src/stem_separator.cpp src/ort_runtime.cpp
  INCLUDE_DIRS src include
  PUBLIC_LIBS sq::engine_simd
  LIBS tp::onnxruntime_headers)

sq_add_service(NAME analysis-stems VERSION 0.1.0
  SOURCES src/analysis_stems.cpp
  DESCRIPTION "Squizzy Studio stem separation (neural, optional runtime)"
  LIBS sqsvc_stems_core)

sq_add_test(NAME analysis_stems SOURCES tests/test_stems.cpp LIBS sqsvc_stems_core sq::appcore
  DEPENDS svc_analysis-stems)
sq_add_test(NAME analysis_stems_adversarial SOURCES tests/test_stems_adversarial.cpp LIBS sqsvc_stems_core)
if(TARGET sqtest_analysis_stems)
  target_include_directories(sqtest_analysis_stems PRIVATE "${SQ_ROOT}/engine/simd/testing")
endif()
