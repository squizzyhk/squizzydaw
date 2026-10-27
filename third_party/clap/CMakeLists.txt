# CLAP 1.2.10 (MIT, see LICENSE) - plugin ABI headers only. Unmodified upstream include/ tree from
# https://github.com/free-audio/clap/tree/1.2.10 . Target: tp::clap (header-only, INTERFACE).
add_library(tp_clap INTERFACE)
add_library(tp::clap ALIAS tp_clap)
target_include_directories(tp_clap SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
