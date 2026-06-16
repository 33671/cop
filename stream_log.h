/*
 * stream_log.h
 *
 * Internal logging helpers for the stream client.
 * NOT part of the public API - do not install.
 */

#ifndef STREAM_LOG_H
#define STREAM_LOG_H

#include "openai_stream_client.h"
#include <stdint.h>

/* Get current timestamp in milliseconds since epoch. */
int64_t get_timestamp_ms(void);

/* Format current timestamp into buf (size bytes). */
void get_timestamp_str(char *buf, size_t size);

/* Write a formatted log line with timestamp prefix. */
void log_write(stream_client_t *c, const char *format, ...);

/* Log raw binary data with escaping for non-printable chars. */
void log_raw_data(stream_client_t *c, const char *data, size_t len);

#endif /* STREAM_LOG_H */
