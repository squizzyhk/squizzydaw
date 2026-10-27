# Public SDK: header-only C ABI + C++ helpers. Target: sq::sdk
add_library(sqsdk INTERFACE)
add_library(sq::sdk ALIAS sqsdk)
target_include_directories(sqsdk INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${SQ_GENERATED_INCLUDE}")
target_compile_features(sqsdk INTERFACE cxx_std_20)
