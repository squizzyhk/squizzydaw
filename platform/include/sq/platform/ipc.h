/* Squizzy Studio platform - inter-process communication: named pipes, shared memory, semaphores.
   Names are short identifiers ([A-Za-z0-9._-], <= 100 chars) mapped to OS namespaces:
     Windows: pipes \\.\pipe\sq.<name>, shm/sem Local\sq.<kind>.<name>
     POSIX:   pipes = AF_UNIX socket <tmp>/sq.<name>.sock, shm "/sq.shm.<name>", sem "/sq.sem.<name>"
   Used by the out-of-process plugin scanner and future sandboxed hosts. Not realtime safe
   (except sq_shm_data access and sq_sem_post). */
#ifndef SQ_PLATFORM_IPC_H
#define SQ_PLATFORM_IPC_H

#include "sq/platform/api.h"

/* ---- named pipes: bidirectional byte streams ---- */
typedef struct sq_pipe_server sq_pipe_server;
typedef struct sq_pipe sq_pipe;
SQP_API sq_result sq_pipe_listen(const char* name, sq_pipe_server** out);
/* Waits for one client. timeout_ms < 0 = infinite. SQ_E_TIMEOUT on timeout. */
SQP_API sq_result sq_pipe_accept(sq_pipe_server* s, int32_t timeout_ms, sq_pipe** out);
SQP_API void sq_pipe_server_close(sq_pipe_server* s);
/* Connects to a listening server, retrying until timeout_ms. */
SQP_API sq_result sq_pipe_connect(const char* name, int32_t timeout_ms, sq_pipe** out);
/* Writes all bytes. */
SQP_API sq_result sq_pipe_write(sq_pipe* p, const void* data, size_t size);
/* Reads up to size bytes (at least 1 unless timeout/closed). SQ_E_CLOSED when the peer is gone. */
SQP_API sq_result sq_pipe_read(sq_pipe* p, void* buf, size_t size, size_t* got, int32_t timeout_ms);
/* Reads exactly size bytes. */
SQP_API sq_result sq_pipe_read_exact(sq_pipe* p, void* buf, size_t size, int32_t timeout_ms);
SQP_API void sq_pipe_close(sq_pipe* p);

/* ---- shared memory ---- */
typedef struct sq_shm sq_shm;
/* SQ_E_EXISTS if a segment with this name already exists. Memory is zero-initialised. */
SQP_API sq_result sq_shm_create(const char* name, size_t size, sq_shm** out);
SQP_API sq_result sq_shm_open(const char* name, sq_shm** out);
SQP_API void* sq_shm_data(sq_shm* s);
SQP_API size_t sq_shm_size(sq_shm* s);
SQP_API void sq_shm_close(sq_shm* s); /* the creator's close also unlinks the name on POSIX */

/* ---- counting semaphores (name NULL = process-local) ---- */
typedef struct sq_sem sq_sem;
SQP_API sq_result sq_sem_create(const char* name, uint32_t initial, uint32_t max_count, sq_sem** out);
SQP_API sq_result sq_sem_open(const char* name, sq_sem** out);
/* timeout_ms < 0 = infinite. SQ_OK or SQ_E_TIMEOUT. */
SQP_API sq_result sq_sem_wait(sq_sem* s, int32_t timeout_ms);
SQP_API sq_result sq_sem_post(sq_sem* s, uint32_t count);
SQP_API void sq_sem_close(sq_sem* s);

#endif
