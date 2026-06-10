/*
 * stream_log.c
 *
 * Internal logging helpers for the stream client.
 */

#define _GNU_SOURCE
#include "stream_log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#define LOG_TIMESTAMP_FMT "%Y-%m-%d %H:%M:%S"

int64_t get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void get_timestamp_str(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    
    strftime(buf, size, LOG_TIMESTAMP_FMT, tm_info);
    size_t len = strlen(buf);
    snprintf(buf + len, size - len, ".%03d", (int)(tv.tv_usec / 1000));
}

void log_write(stream_client_t *c, const char *format, ...) {
    if (!c || !c->log_fp) return;
    
    char timestamp[64];
    get_timestamp_str(timestamp, sizeof(timestamp));
    
    fprintf(c->log_fp, "[%s] ", timestamp);
    
    va_list args;
    va_start(args, format);
    vfprintf(c->log_fp, format, args);
    va_end(args);
    
    fprintf(c->log_fp, "\n");
    fflush(c->log_fp);
}

void log_raw_data(stream_client_t *c, const char *data, size_t len) {
    if (!c || !c->log_fp) return;
    
    char timestamp[64];
    get_timestamp_str(timestamp, sizeof(timestamp));
    
    fprintf(c->log_fp, "[%s] [RAW] ", timestamp);
    
    for (size_t i = 0; i < len; i++) {
        char ch = data[i];
        if (ch == '\n') {
            fprintf(c->log_fp, "\\n");
        } else if (ch == '\r') {
            fprintf(c->log_fp, "\\r");
        } else if (ch == '\t') {
            fprintf(c->log_fp, "\\t");
        } else if (ch < 32 || ch > 126) {
            fprintf(c->log_fp, "\\x%02x", (unsigned char)ch);
        } else {
            fputc(ch, c->log_fp);
        }
    }
    
    fprintf(c->log_fp, "\n");
    fflush(c->log_fp);
}
