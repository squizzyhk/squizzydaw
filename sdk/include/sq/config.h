/* Squizzy Studio SDK - compiler/platform configuration. C and C++. */
#ifndef SQ_CONFIG_H
#define SQ_CONFIG_H

#if defined(_WIN32)
#  define SQ_OS_WINDOWS 1
#elif defined(__APPLE__)
#  define SQ_OS_MACOS 1
#  define SQ_OS_POSIX 1
#elif defined(__linux__)
#  define SQ_OS_LINUX 1
#  define SQ_OS_POSIX 1
#else
#  define SQ_OS_POSIX 1
#endif

/* Calling convention for every function that crosses a DLL boundary
   (exports, interface methods, callbacks). Only x86-32 Windows differs; x64 is the target. */
#if defined(_WIN32) && !defined(_WIN64)
#  define SQ_CALL __stdcall
#else
#  define SQ_CALL
#endif

#if defined(_WIN32)
#  define SQ_DLL_EXPORT __declspec(dllexport)
#  define SQ_DLL_IMPORT __declspec(dllimport)
#else
#  define SQ_DLL_EXPORT __attribute__((visibility("default")))
#  define SQ_DLL_IMPORT
#endif

#ifdef __cplusplus
#  define SQ_EXTERN_C extern "C"
#  define SQ_EXTERN_C_BEGIN extern "C" {
#  define SQ_EXTERN_C_END }
#else
#  define SQ_EXTERN_C
#  define SQ_EXTERN_C_BEGIN
#  define SQ_EXTERN_C_END
#endif

#if defined(__cplusplus)
#  define SQ_INLINE inline
#else
#  define SQ_INLINE static inline
#endif

#endif /* SQ_CONFIG_H */
