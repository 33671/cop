#ifndef UTILS_UTF8_H
#define UTILS_UTF8_H

#include <stdint.h>
#include <stddef.h>

#include "sds/sds.h"

void sanitize_utf8(uint8_t *data, size_t len);
void strip_ansi_escapes(uint8_t *data, size_t len);
int utf8_char_width(const char *s, int *bytes);
int utf8_string_width(const char *s);

/* Extract the last N lines of `data` (length `len`). Each line is truncated
 * to `max_line` characters with "[truncated]" appended when it exceeds.
 * Result is allocated in the given arena. */
sds get_last_n_lines_truncated(Arena *a, const char *data, size_t len,
                                int n, int max_line);

#endif
