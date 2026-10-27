# codec-flac: FLAC decode (third_party/dr_flac) + original encoder (flac_encoder.cpp, md5.hpp).
# Shared header-only helpers come from services/codec-wav/shared (source include, no link dep).
set(_sqcodec_shared "${CMAKE_CURRENT_SOURCE_DIR}/../codec-wav/shared")
sq_add_service(NAME codec-flac VERSION 0.1.0
  SOURCES flac.cpp flac_encoder.cpp flac_encoder.hpp md5.hpp
  DESCRIPTION "Squizzy Studio FLAC codec"
  LIBS tp::dr_flac)
target_include_directories(svc_codec-flac PRIVATE "${_sqcodec_shared}")

sq_add_test(NAME codec_flac SOURCES tests/test_codec_flac.cpp LIBS sq::appcore DEPENDS svc_codec-flac TIMEOUT 300)
if(TARGET sqtest_codec_flac)
  target_include_directories(sqtest_codec_flac PRIVATE "${_sqcodec_shared}" "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
