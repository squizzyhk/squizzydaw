# codec-wav: WAV / RF64 / BW64 / AIFF / AIFF-C reader+writer (own code, no third-party deps).
# shared/sqcodec/ holds header-only helpers that the other codec-* services include as source
# (no link dependency between services).
sq_add_service(NAME codec-wav VERSION 0.1.0
  SOURCES wav.cpp wav_reader.cpp wav_writer.cpp wav_internal.hpp
          shared/sqcodec/common.hpp shared/sqcodec/riff_markers.hpp
  DESCRIPTION "Squizzy Studio WAV/AIFF codec")
target_include_directories(svc_codec-wav PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shared")

sq_add_test(NAME codec_wav SOURCES tests/test_codec_wav.cpp LIBS sq::appcore DEPENDS svc_codec-wav TIMEOUT 300)
if(TARGET sqtest_codec_wav)
  target_include_directories(sqtest_codec_wav PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shared")
endif()
