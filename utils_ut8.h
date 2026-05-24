#ifndef UTILS_UT8_H
#define UTILS_UT8_H

#include <stdint.h>
#include <stddef.h>

void sanitize_utf8(uint8_t *data, size_t len);
int utf8_char_width(const char *s, int *bytes);
int utf8_string_width(const char *s);

#endif
