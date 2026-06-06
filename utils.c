#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <curl/curl.h>

#include "utils.h"

/* Load .env file into environment variables */
void load_env_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return;  /* Silently skip if file doesn't exist */
    }
    
    printf("Loading environment from: %s\n", path);
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        /* Remove trailing newline/carriage return */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }
        
        /* Skip empty lines and comments */
        if (len == 0 || line[0] == '#') continue;
        
        /* Find the '=' separator */
        char *eq = strchr(line, '=');
        if (!eq) continue;
        
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        
        /* Trim whitespace from key */
        while (*key == ' ' || *key == '\t') key++;
        char *kend = key + strlen(key) - 1;
        while (kend > key && (*kend == ' ' || *kend == '\t')) *kend-- = '\0';
        
        /* Trim whitespace from value (optional) */
        while (*value == ' ' || *value == '\t') value++;
        
        /* Skip if key is empty */
        if (strlen(key) == 0) continue;
        
        /* Set environment variable, overwriting any existing value */
        setenv(key, value, 1);
    }
    
    fclose(fp);
}

/*
 * Read from a non-blocking pipe fd and accumulate into a dynamically growing
 * buffer.  Shared by llm_runtime_popen() and extract_chunk_internal().
 *
 * Returns: >0 = bytes read, 0 = EOF, -1 = EAGAIN, -2 = fatal error.
 */
ssize_t pipe_drain(int fd, char **buf, size_t *len, size_t *cap) {
    char tmp[4096];
    ssize_t n = read(fd, tmp, sizeof(tmp));

    if (n > 0) {
        /* Grow buffer if needed */
        if (*len + n + 1 > *cap) {
            size_t nc = *cap * 2;
            while (nc < *len + n + 1) nc *= 2;
            char *nb = realloc(*buf, nc);
            if (!nb) return -2;
            *buf = nb;
            *cap = nc;
        }
        memcpy(*buf + *len, tmp, n);
        *len += n;
        (*buf)[*len] = '\0';
        return n;
    }

    if (n == 0) return 0;   /* EOF */

    return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
}
