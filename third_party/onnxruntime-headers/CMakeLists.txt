# ONNX Runtime C API header only (MIT, see LICENSE), v1.17.3, unmodified upstream
# include/onnxruntime/core/session/onnxruntime_c_api.h from https://github.com/microsoft/onnxruntime
# (tag v1.17.3). No runtime binary is vendored: services/analysis-stems loads onnxruntime
# dynamically at run time if the user has it installed, otherwise the NN runtime is unavailable.
# Target: tp::onnxruntime_headers (interface, header only).
add_library(tp_onnxruntime_headers INTERFACE)
add_library(tp::onnxruntime_headers ALIAS tp_onnxruntime_headers)
target_include_directories(tp_onnxruntime_headers SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
