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

int log_level = 0;
static FILE *log_fd = NULL;

// set logger
void log_init(FILE *dst, int level)
{
    if (!dst) dst = stderr;
    log_fd = dst;
    log_level = level;
}

// FIXME delete this
int log_cmd_err(const char *cmd, const char *opt, const char *fmt, ...)
{
    va_list args;  

    fprintf(log_fd, "[ERROR] %s: %s: ", cmd, opt);

    va_start(args, fmt);
    vfprintf(log_fd, fmt, args);
    va_end(args);

    fprintf(log_fd, "\n");
    fflush(log_fd);
    
    return -1;
}

void _log_msg(const char *file, int line, const char *func, 
    int ec, int what, const char *who, const char *fmt, ...)
{
    if (!who) {
        switch(what) {
        case LOG_INFO:  who = "INFO";  break;
        case LOG_DEBUG: who = "DEBUG"; break;
        case LOG_ERROR: who = "ERROR"; break;
        case LOG_FATAL: who = "FATAL"; break;
        }
    }

    if (who) fprintf(log_fd, "[%s] ", who);
    if (file && func) fprintf(log_fd, "%s:%d (%s): ", file, line, func);

    va_list args;
    va_start(args, fmt);
    vfprintf(log_fd, fmt, args);
    va_end(args);

    // add errno
    if (ec) fprintf(log_fd, ": %s (errno: %d)", strerror(ec), ec);

    fprintf(log_fd, "\n");
    fflush(log_fd);

    // fatal-check
    if (what == LOG_FATAL) exit(1);
}

// log_info cmd-line - useful for debugging pod exec issues
void log_argv(const char *what, int argc, char *argv[])
{
    for (int i= 0 ; i < argc; i++) {
        log_info(what, "argv[%d]=%s", i, argv[i]);
    }
}
