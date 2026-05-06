/*
 * sanitize_utf8.h
 *
 * In-place UTF-8 sanitizer. Scans a byte buffer and replaces any
 * invalid UTF-8 byte sequences with '?' (0x3F). The function
 * operates in a single forward pass and is safe to use on partial
 * or untrusted input.
 *
 * Invalid sequences include:
 *   - Illegal first bytes (0xC0, 0xC1, 0xF5–0xFF)
 *   - Continuation bytes (0x80–0xBF) not preceded by a valid multi-byte head
 *   - Missing continuation bytes (truncated sequences)
 *   - Continuation bytes that do not match the 0x80–0xBF pattern
 *   - Overlong encodings (e.g. 0xC0 0x80 for U+0000)
 *   - Surrogate code points (U+D800–U+DFFF)
 *   - Code points beyond U+10FFFF
 */

#ifndef SANITIZE_UTF8_H
#define SANITIZE_UTF8_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sanitize a UTF-8 buffer in place.
 *
 * @param data  Pointer to the byte buffer. If NULL, the function does nothing.
 * @param len   Number of bytes in the buffer.
 */
void sanitize_utf8(uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SANITIZE_UTF8_H */
