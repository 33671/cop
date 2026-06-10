/*
 * debug.h
 *
 * Unified debug logging macro.
 * Include this header in any .c file to get debug_log().
 *
 * Compiled out unless -DDEBUG is set at build time.
 */

#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>

#ifdef DEBUG
    #define debug_log(fmt, ...) \
        fprintf(stderr, "[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#else
    #define debug_log(fmt, ...) ((void)0)
#endif

#endif /* DEBUG_H */
