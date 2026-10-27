# codec-aac: AAC / M4A via Windows Media Foundation (system component, nothing vendored).
# Non-Windows builds produce a stub service that reports no formats.
set(_sqcodec_shared "${CMAKE_CURRENT_SOURCE_DIR}/../codec-wav/shared")
set(_aac_libs "")
if(WIN32)
  set(_aac_libs mfplat mfreadwrite mfuuid propsys ole32)
endif()
sq_add_service(NAME codec-aac VERSION 0.1.0
  SOURCES aac.cpp
  DESCRIPTION "Squizzy Studio AAC codec (Media Foundation)"
  LIBS ${_aac_libs})
target_include_directories(svc_codec-aac PRIVATE "${_sqcodec_shared}")

sq_add_test(NAME codec_aac SOURCES tests/test_codec_aac.cpp LIBS sq::appcore DEPENDS svc_codec-aac TIMEOUT 300)
if(TARGET sqtest_codec_aac)
  target_include_directories(sqtest_codec_aac PRIVATE "${_sqcodec_shared}")
endif()
if(MSVC)
  # Delay-load MF so the module still loads (and reports "unavailable") on Windows editions
  # without the Media Feature Pack; MFStartup is called under an SEH guard.
  target_link_options(svc_codec-aac PRIVATE /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll /DELAYLOAD:propsys.dll)
  target_link_libraries(svc_codec-aac PRIVATE delayimp)
endif()
