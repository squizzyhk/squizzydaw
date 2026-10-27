# codec-mp3: MP3 decode (third_party/dr_mp3) + ID3; encode through a user-installed LAME (LGPL),
# loaded at runtime only (never linked, never vendored). Test fixtures in tests/data were made
# with ffmpeg/libmp3lame from synthetic sine tones.
set(_sqcodec_shared "${CMAKE_CURRENT_SOURCE_DIR}/../codec-wav/shared")
sq_add_service(NAME codec-mp3 VERSION 0.1.0
  SOURCES mp3.cpp id3.hpp
  DESCRIPTION "Squizzy Studio MP3 codec"
  LIBS tp::dr_mp3)
target_include_directories(svc_codec-mp3 PRIVATE "${_sqcodec_shared}")

sq_add_test(NAME codec_mp3 SOURCES tests/test_codec_mp3.cpp LIBS sq::appcore DEPENDS svc_codec-mp3 TIMEOUT 300)
if(TARGET sqtest_codec_mp3)
  target_include_directories(sqtest_codec_mp3 PRIVATE "${_sqcodec_shared}")
  target_compile_definitions(sqtest_codec_mp3 PRIVATE SQ_CODEC_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
endif()
