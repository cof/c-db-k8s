/*
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "util.h"

void log_info(const char *fmt, ...)
{
    va_list args;  

    fprintf(stdout, "[+] ");

    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);

    fprintf(stdout, "\n");
    fflush(stdout);
}


int _log_error(const char *file, int line, const char *func, int ec, const char *fmt, ...)
{
    va_list args;  

    fprintf(stderr, "[ERROR] %s:%d (%s): ", file, line, func);

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    if (ec != 0) {
        fprintf(stderr, ": %s (errno: %d)", strerror(ec), ec);
    }

    fprintf(stderr, "\n");

    // XXX always -1
    return -1;
}

void _fatal_error(const char *file, int line, const char *func, int ec, const char *fmt, ...)
{
    va_list args;  

    fprintf(stderr, "[FATAL] %s:%d (%s): ", file, line, func);

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    if (ec != 0) {
        fprintf(stderr, ": %s (errno: %d)", strerror(ec), ec);
    }

    fprintf(stderr, "\n");
    fflush(stderr);

    exit(1);
}

char *gen_path(const char *dir, const char *name)
{
    if (!dir || !name) return NULL;

    char *path = NULL;
    int rc = asprintf(&path, "%s/%s", dir, name);

    if (rc == -1) {
        // out of memory ?
        return NULL;
    }

    return path;
}

char *slice_strdup(const struct str_slice str)
{
    char *copy = malloc(str.len + 1);

    if (copy) {
        memcpy(copy, str.ptr, str.len);
        copy[str.len] = 0;
    }

    return copy;
}

char *itoa(char *buf, int len, int val)
{
    if (!buf || len == 0) {
        return buf;
    }

    char *str = &buf[len -1];
    *str = '\0';
    if (val == 0) *--str = '0';

    while (val) {
        *--str = (val % 10) + '0';
        val /= 10;
    }

    return str; 
}

char *int_tostr(int val) 
{
    static char bufs[16][10];
    static int idx;

    char *str = bufs[idx];
    idx = (idx + 1) & 15;

    return itoa(str, sizeof(bufs[0][0]), val);
}
