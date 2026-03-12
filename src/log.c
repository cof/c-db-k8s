/*
 * logger api
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "log.h"

void log_msg(const char *msg)
{
    fputs(msg, stdout);
    fflush(stdout);
}

void _log_info(const char *what, const char *fmt, ...)
{
    va_list args;  

    fprintf(stdout, "[%s] ", what);

    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);

    fprintf(stdout, "\n");
}

int log_cmd_err(const char *cmd, const char *opt, const char *fmt, ...)
{
    va_list args;  

    fprintf(stdout, "[ERROR] %s: %s: ", cmd, opt);

    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);

    fprintf(stdout, "\n");
    
    return -1;
}

void _log_error(const char *file, int line, const char *func, int ec, const char *fmt, ...)
{
    va_list args;  

    fprintf(stderr, "[ERROR] %s:%d (%s): ", file, line, func);

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    if (ec != 0) {
        // have an errno
        fprintf(stderr, ": %s (errno: %d)", strerror(ec), ec);
    }

    fprintf(stderr, "\n");
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
