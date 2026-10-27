/* Squizzy Studio platform - logging. Thread-safe; NOT realtime safe (formats + locks).
   Default sinks: stderr (on) and, on Windows, the debugger output. Optional file sink. */
#ifndef SQ_PLATFORM_LOG_H
#define SQ_PLATFORM_LOG_H

#include "sq/platform/api.h"

typedef void (*sq_log_sink_fn)(void* user, int32_t level, const char* tag, const char* message);

SQP_API void sq_log_write(int32_t level, const char* tag, const char* message);
SQP_API void sq_log_printf(int32_t level, const char* tag, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
SQP_API void sq_log_set_level(int32_t level); /* messages below level are dropped */
SQP_API int32_t sq_log_get_level(void);
SQP_API const char* sq_log_level_name(int32_t level);
SQP_API int32_t sq_log_level_from_name(const char* name); /* "info" -> SQ_LOG_INFO; -1 if unknown */
SQP_API void sq_log_set_console(sq_bool enabled);
SQP_API sq_result sq_log_open_file(const char* path); /* appends; NULL closes */
/* Returns a sink id (> 0) or <= 0 on failure. A module MUST remove its sinks before unload. */
SQP_API int32_t sq_log_add_sink(sq_log_sink_fn fn, void* user);
SQP_API void sq_log_remove_sink(int32_t id);

#define SQ_LOGI(tag, ...) sq_log_printf(SQ_LOG_INFO, tag, __VA_ARGS__)
#define SQ_LOGW(tag, ...) sq_log_printf(SQ_LOG_WARN, tag, __VA_ARGS__)
#define SQ_LOGE(tag, ...) sq_log_printf(SQ_LOG_ERROR, tag, __VA_ARGS__)
#define SQ_LOGD(tag, ...) sq_log_printf(SQ_LOG_DEBUG, tag, __VA_ARGS__)

#endif
