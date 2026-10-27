/* Squizzy Studio platform - dynamic libraries, process info, special dirs, environment, GUIDs. */
#ifndef SQ_PLATFORM_SYSTEM_H
#define SQ_PLATFORM_SYSTEM_H

#include "sq/platform/api.h"

typedef struct sq_dylib sq_dylib;
/* Loads a shared library by UTF-8 path. Windows: the library's dependencies are searched in its
   own folder first (LOAD_WITH_ALTERED_SEARCH_PATH), then the exe folder.
   On failure, err (optional) receives a message. */
SQP_API sq_result sq_dylib_open(const char* path, sq_dylib** out, char* err, size_t err_size);
SQP_API void* sq_dylib_symbol(sq_dylib* lib, const char* name);
SQP_API void sq_dylib_close(sq_dylib* lib);
/* True if a library with this path is currently mapped into the process. */
SQP_API sq_bool sq_dylib_is_loaded(const char* path);
/* Platform library suffix including the dot: ".dll", ".so", ".dylib". */
SQP_API const char* sq_dylib_suffix(void);

typedef enum sq_special_dir {
  SQ_DIR_EXE = 0,         /* folder containing the running executable */
  SQ_DIR_USER_CONFIG = 1, /* %APPDATA%/SquizzyStudio, ~/.config/SquizzyStudio */
  SQ_DIR_USER_DATA = 2,   /* %LOCALAPPDATA%/SquizzyStudio, ~/.local/share/SquizzyStudio */
  SQ_DIR_TEMP = 3,
  SQ_DIR_HOME = 4,
  SQ_DIR_DOCUMENTS = 5
} sq_special_dir;
SQP_API sq_result sq_special_dir_get(sq_special_dir which, sq_buffer* out);
SQP_API sq_result sq_exe_path(sq_buffer* out);
SQP_API uint32_t sq_process_id(void);
/* SQ_E_NOTFOUND if unset. */
SQP_API sq_result sq_env_get(const char* name, sq_buffer* out);
SQP_API sq_result sq_env_set(const char* name, const char* value /* NULL = unset */);

/* Random (version 4) GUID from the OS random source. */
SQP_API sq_result sq_guid_generate(sq_guid* out);

#endif
