# codec-ogg: Ogg Vorbis decode + encode (third_party/libvorbis incl. vorbisfile/vorbisenc +
# third_party/libogg, BSD-3-Clause) + Opus decode seam (libopusfile loaded at runtime when the user has it;
# never linked or vendored). No Opus encoder (libopus/libopusenc not vendored). Fixtures in
# tests/data were made with ffmpeg (libvorbis / libopus) from synthetic sine tones.
set(_sqcodec_shared "${CMAKE_CURRENT_SOURCE_DIR}/../codec-wav/shared")
sq_add_service(NAME codec-ogg VERSION 0.2.0
  SOURCES ogg.cpp
  DESCRIPTION "Squizzy Studio Ogg Vorbis/Opus codec"
  LIBS tp::libvorbis)
target_include_directories(svc_codec-ogg PRIVATE "${_sqcodec_shared}")

foreach(_t codec_ogg codec_ogg_opus)
  sq_add_test(NAME ${_t} SOURCES tests/test_${_t}.cpp LIBS sq::appcore DEPENDS svc_codec-ogg TIMEOUT 300)
  if(TARGET sqtest_${_t})
    target_include_directories(sqtest_${_t} PRIVATE "${_sqcodec_shared}")
    target_compile_definitions(sqtest_${_t} PRIVATE SQ_CODEC_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    # sq_add_test does not set it (yet): exit 77 = SQ_SKIP -> CTest "Skipped", never "Passed".
    set_tests_properties(${_t} PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endforeach()
