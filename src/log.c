/*
 * LOG - a logger API
 * -------------------
 * See log.h for description.
 *
 * API sections
 * ------------
 * functions : direct functions
 * macros    : various msg-str and fmt-str macros
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "log.h"


void log_msg(const char *msg)
{
    fputs(msg, stderr);
    fflush(stderr);
}

void log_info(const char *what, const char *fmt, ...)
{
    va_list args;

    fprintf(stderr, "[%s] ", what);

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
    fflush(stderr);
}

int log_cmd_err(const char *cmd, const char *opt, const char *fmt, ...)
{
    va_list args;  

    fprintf(stderr, "[ERROR] %s: %s: ", cmd, opt);

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
    fflush(stderr);
    
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
    fflush(stderr);
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

// log_info cmd-line - useful for debugging pod exec issues
void log_argv(const char *what, int argc, char *argv[])
{
    for (int i= 0 ; i < argc; i++) {
        log_info(what, "argv[%d]=%s", i, argv[i]);
    }
}
