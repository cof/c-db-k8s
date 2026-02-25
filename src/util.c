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

    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fprintf(stdout, "\n");
}

void log_err(const char *fmt, ...)
{
    va_list args;  

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

void log_estr(const char *file, int line, const char *estr, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    fprintf(stderr, "[%s:%d] ", file, line);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, ": %s\n", estr);
    va_end(args);
}
