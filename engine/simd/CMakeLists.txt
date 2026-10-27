# engine/simd: runtime-dispatched vector op table (sq_vector_ops of interfaces/dsp.h) + FFT.
# Target sq::engine_simd (static). Kernel sets: generic everywhere; SSE4.2 / AVX2+FMA / AVX-512F on
# x86-64. MSVC accepts every intrinsic without /arch, so no per-file flags are needed there (and
# none are used, which also keeps inline code of one ISA from leaking into another via COMDAT
# folding). GCC/Clang build each kernel TU with its own -m flags; those TUs include no STL headers.
set(_simd_sources
  src/dispatch.cpp
  src/ops_generic.cpp
  src/fft.cpp)

set(_simd_x86 OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64|x64)$" OR CMAKE_GENERATOR_PLATFORM MATCHES "^[xX]64$")
  set(_simd_x86 ON)
endif()
if(_simd_x86)
  list(APPEND _simd_sources src/ops_sse42.cpp src/ops_avx2.cpp src/ops_avx512.cpp)
  if(NOT MSVC)
    set_source_files_properties(src/ops_sse42.cpp PROPERTIES COMPILE_OPTIONS "-msse4.2;-mssse3")
    set_source_files_properties(src/ops_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
    set_source_files_properties(src/ops_avx512.cpp PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx2;-mfma")
  endif()
endif()

sq_add_library(NAME sqengine_simd SOURCES ${_simd_sources} INCLUDE_DIRS include)
add_library(sq::engine_simd ALIAS sqengine_simd)
if(_simd_x86)
  target_compile_definitions(sqengine_simd PRIVATE SQ_SIMD_X86=1)
endif()
# CONTRACTS names the provider of sq_dsp_vector_ops() "sq::dsp". Offer that name too unless an
# engine/dsp unit already defines it (it is configured first: children are added in sorted order).
if(NOT TARGET sq::dsp)
  add_library(sq::dsp ALIAS sqengine_simd)
endif()

sq_add_test(NAME engine_simd_kernels SOURCES tests/test_simd.cpp LIBS sq::engine_simd)
sq_add_test(NAME engine_simd_fft SOURCES tests/test_fft.cpp LIBS sq::engine_simd)
sq_add_test(NAME engine_simd_edge SOURCES tests/test_simd_edge.cpp LIBS sq::engine_simd)
