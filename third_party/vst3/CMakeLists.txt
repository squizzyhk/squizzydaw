# VST3 pluginterfaces v3.8.1_build_84 (MIT since VST SDK 3.8, see LICENSE.txt) - minimal subset of
# https://github.com/steinbergmedia/vst3_pluginterfaces (base/, vst/, gui/iplugview.h), unmodified.
# Only the interface headers + base/funknown.cpp (FUID / atomic helpers) are vendored; no SDK
# helper classes. Target: tp::vst3 (static).
add_library(tp_vst3 STATIC pluginterfaces/base/funknown.cpp)
add_library(tp::vst3 ALIAS tp_vst3)
target_include_directories(tp_vst3 SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
  target_compile_options(tp_vst3 PRIVATE /W0 /utf-8)
  target_compile_definitions(tp_vst3 PUBLIC _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(tp_vst3 PUBLIC ole32)
else()
  target_compile_options(tp_vst3 PRIVATE -w)
endif()
if(APPLE)
  target_link_libraries(tp_vst3 PUBLIC "-framework CoreFoundation")
endif()
