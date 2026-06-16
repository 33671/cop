#ifndef UTILS_H
#define UTILS_H

#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

void load_env_file(const char *path);

/*
 * Read from a non-blocking pipe fd and accumulate into a dynamically growing
 * buffer.  Designed for use after fdwait() reports FDW_IN or FDW_ERR on a
 * pipe read-end.
 *
 *   fd   – non-blocking pipe fd
 *   buf  – pointer to buffer pointer (may be realloc'd)
 *   len  – current used length
 *   cap  – current allocated capacity
 *
 * Returns:
 *   > 0   bytes read and accumulated (data was available)
 *     0   EOF - the write end of the pipe was closed
 *    -1   EAGAIN / EWOULDBLOCK - no data available, try later
 *    -2   fatal error - realloc failed or a real read error (check errno)
 *
 * On success (>0) the buffer is NUL-terminated.  On -2 the buffer is left
 * in an undefined state; the caller should free it and bail out.
 */
ssize_t pipe_drain(int fd, char **buf, size_t *len, size_t *cap);

#ifdef __cplusplus
}
#endif

#endif /* UTILS_H */
