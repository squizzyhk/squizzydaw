/* Squizzy Studio platform - filesystem. All paths UTF-8; '/' and '\' both accepted on Windows. */
#ifndef SQ_PLATFORM_FS_H
#define SQ_PLATFORM_FS_H

#include "sq/platform/api.h"

SQP_API sq_bool sq_fs_exists(const char* path);
SQP_API sq_bool sq_fs_is_dir(const char* path);
SQP_API sq_bool sq_fs_is_file(const char* path);
SQP_API sq_result sq_fs_create_dirs(const char* path); /* mkdir -p; SQ_OK if it already exists */
SQP_API sq_result sq_fs_remove(const char* path);      /* file or empty dir; SQ_E_NOTFOUND if missing */
SQP_API sq_result sq_fs_remove_all(const char* path);  /* recursive */
SQP_API sq_result sq_fs_rename(const char* from, const char* to); /* replaces an existing file */
SQP_API sq_result sq_fs_copy_file(const char* from, const char* to, sq_bool overwrite);
SQP_API sq_result sq_fs_file_size(const char* path, uint64_t* out);
SQP_API sq_result sq_fs_mtime(const char* path, int64_t* out_unix_ms);
SQP_API sq_result sq_fs_absolute(const char* path, sq_buffer* out); /* normalized absolute path */
/* Whole-file helpers. write is atomic (temp file + rename) and creates parent dirs. */
SQP_API sq_result sq_fs_read_file(const char* path, sq_buffer* out);
SQP_API sq_result sq_fs_write_file(const char* path, const void* data, size_t size);
/* Calls fn for each entry (not recursive, no "." / ".."). Return SQ_FALSE from fn to stop. */
typedef sq_bool (*sq_fs_list_fn)(void* user, const char* name, sq_bool is_dir);
SQP_API sq_result sq_fs_list_dir(const char* path, sq_fs_list_fn fn, void* user);

/* Streaming file handle (for codecs etc.). */
#define SQ_FILE_READ     0x1u
#define SQ_FILE_WRITE    0x2u
#define SQ_FILE_CREATE   0x4u /* create if missing */
#define SQ_FILE_TRUNCATE 0x8u
#define SQ_FILE_APPEND   0x10u
#define SQ_SEEK_SET 0
#define SQ_SEEK_CUR 1
#define SQ_SEEK_END 2
typedef struct sq_file sq_file;
SQP_API sq_result sq_file_open(const char* path, uint32_t flags, sq_file** out);
SQP_API sq_result sq_file_read(sq_file* f, void* buf, size_t size, size_t* got);
SQP_API sq_result sq_file_write(sq_file* f, const void* buf, size_t size);
SQP_API sq_result sq_file_seek(sq_file* f, int64_t offset, int whence, uint64_t* new_pos);
SQP_API sq_result sq_file_size(sq_file* f, uint64_t* out);
SQP_API sq_result sq_file_flush(sq_file* f);
SQP_API void sq_file_close(sq_file* f);

#endif
