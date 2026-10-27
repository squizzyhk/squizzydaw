# services/video-engine: video files -> BGRA frames + timestamps (sq/interfaces/video.h).
#
# Backends:
#   mediafoundation  Windows system component (nothing vendored). mfplat/mfreadwrite are
#                    delay-loaded so the module loads on editions without the Media Feature Pack.
#   ffmpeg           LGPL seam. Never vendored. Without SQ_FFMPEG_DIR a stub reports it
#                    unavailable. With
#                      -DSQ_FFMPEG_DIR=<ffmpeg *shared* dev package with include/ (and bin/)>
#                    the adapter is compiled against the HEADERS ONLY; nothing is linked. At run
#                    time it opens the matching shared libraries (avformat-<major>.dll, ...) from
#                    Services.video-engine.FfmpegDir or the default search path and resolves its
#                    functions with sq_dylib_symbol (dynamic use, as the LGPL requires), so the
#                    module loads and reports ffmpeg unavailable where it is missing. Use an LGPL
#                    build of ffmpeg (no --enable-gpl / --enable-nonfree).
set(SQ_FFMPEG_DIR "" CACHE PATH "ffmpeg shared dev package (LGPL build) for the video-engine ffmpeg backend; empty = stub")

sq_add_library(NAME sqvideo_core
  SOURCES src/frame_util.cpp
  INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src")

set(_video_libs sqvideo_core)
if(WIN32)
  list(APPEND _video_libs mfplat mfreadwrite mfuuid propsys ole32)
endif()

if(SQ_FFMPEG_DIR)
  foreach(_h libavformat/avformat.h libavcodec/avcodec.h libavutil/imgutils.h libswscale/swscale.h
          libswresample/version.h)
    if(NOT EXISTS "${SQ_FFMPEG_DIR}/include/${_h}")
      message(FATAL_ERROR "sq: SQ_FFMPEG_DIR='${SQ_FFMPEG_DIR}' lacks include/${_h}")
    endif()
  endforeach()
  message(STATUS "sq: video-engine: ffmpeg backend enabled (headers from ${SQ_FFMPEG_DIR}; libraries loaded at run time)")
else()
  message(STATUS "sq: video-engine: ffmpeg backend is a stub (no SQ_FFMPEG_DIR)")
endif()

sq_add_service(NAME video-engine VERSION 0.1.0
  SOURCES src/module.cpp src/mf_backend.cpp src/ffmpeg_backend.cpp
  DESCRIPTION "Squizzy Studio video decoding service"
  LIBS ${_video_libs})

if(MSVC)
  target_link_options(svc_video-engine PRIVATE /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll /DELAYLOAD:propsys.dll)
  target_link_libraries(svc_video-engine PRIVATE delayimp)
endif()
if(SQ_FFMPEG_DIR)
  target_compile_definitions(svc_video-engine PRIVATE SQ_HAVE_FFMPEG=1)
  target_include_directories(svc_video-engine SYSTEM PRIVATE "${SQ_FFMPEG_DIR}/include")
  if(MSVC)
    target_compile_options(svc_video-engine PRIVATE /external:W0 /external:anglebrackets)
  endif()
endif()

sq_add_test(NAME video_engine_util SOURCES tests/test_frame_util.cpp LIBS sqvideo_core)
set(_video_test_libs sq::appcore)
if(WIN32)
  list(APPEND _video_test_libs mfplat mfreadwrite mfuuid ole32)
endif()
sq_add_test(NAME video_engine_module SOURCES tests/test_module.cpp LIBS ${_video_test_libs}
            DEPENDS svc_video-engine TIMEOUT 180 ARGS "[core]")
if(TARGET sqtest_video_engine_module)
  set_tests_properties(video_engine_module PROPERTIES SKIP_RETURN_CODE 77)
  # The ffmpeg case is its own entry so a build without the seam shows it as Skipped.
  add_test(NAME video_engine_ffmpeg COMMAND sqtest_video_engine_module "[ffmpeg]" WORKING_DIRECTORY "${SQ_BIN_DIR}")
  set_tests_properties(video_engine_ffmpeg PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
  if(SQ_FFMPEG_DIR)
    target_compile_definitions(sqtest_video_engine_module PRIVATE SQ_TEST_FFMPEG_BIN="${SQ_FFMPEG_DIR}/bin")
  endif()
endif()
