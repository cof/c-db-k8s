/*
 * LOG - the logger API
 * --------------------
 * Simple logger API that can report informaton and detailed error msesages.
 *
 * Overview
 * --------
 * Basic idea is you use logger to report information to users, log what a process
 * is give detailed error message if something fails.
 *
 * There are 3 basic log types:
 *
 *  info  - "[what] fmt-str" 
 *  error - "[ERROR] file:line (func): fmt-str"
 *  fatal - "[FATAL] file:line (func): fmt-str
 *
 * Logger use fmt-str to allow complete control of what logged.
 *
 * e.g
 *  log_info("+", "The service is up");
 *  log_info("INFO", "did %s","something");
 *
 * Logger also has a range of macros that report the file, line and func
 * where an error has occured allowing devs to easily trace problems in
 * the code base.
 *
 * Logger also has a range of macros that can be used to both log a msg
 * and return from a function all in one line.  
 *
 * These macros using the following suffix patterns:
 *
 *  _rf - return fail (-1)
 *  _rc - return code
 *  _rn - return null
 *  _rz - return zero
 *
 * This together with error reporting provides for a clear and simple form of 
 * exception handling where code can in one line both report an error and return
 * back to the caller all in one line.
 *
 * e.g.
 *
 *  return log_error_rf("My func failed");
 *  return log_error_rc(-2, "foo1 failed)
 *  return log_errno_rf("foo2 failed");
 *  return log_errno_rc(-4, "foo2 failed");
 *
 * For a full list see macros section below.
 *
 * API sections
 * ------------
 * functions : direct functions
 * macros    : various msg-str and fmt-str macros
 */
#ifndef _LOG_H_
#define _LOG_H_

#include <errno.h>

extern int verbose;

/*
 * functions : direct functions
 * -----------------------------
 * log_msg(msg)             :  log msg - a simple fputs(msg)
 * log_info(what, fmt, ...) : log info  - "[what] fmt-str"
 * log_cmd_err(cmd, opt, fmt) : FIXME this need to go
 * _log_error(file,line,unc, ec, fmt, ...) : log error - "[ERROR] file:line (func): fmt-str":
 * _fatal_error(file, line, func, ec, fmt, ...) : log fatal,exit(1) - "[FATAL] file:line (func): fmt-str
 * log_argv(what, argc, argv) : log_info cmd-line - useful for debuing pod exec issues
 */
void log_msg(const char *msg);
void log_info(const char *what, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int log_cmd_err(const char *cmd, const char *opt, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void _log_error(const char *file, int line, const char *func, int ec, const char *fmt, ...) 
    __attribute__((format(printf, 5, 6)));
void _fatal_error(const char *file, int line, const char *func, int ec, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
void log_argv(const char *what, int argc, char *argv[]);

/*
 * macros : various msg-str and fmt-str macros
 * -------------------------------------------
 * log_msg_rf(msg)          : log msg/return fail
 * log_info_rc(what,rc,...) : log_info/return code
 * -
 * log_error(...)           : log_error msg - direct wrapper to _log_error.
 * log_error_rf(...)        : log_error msg - return fail
 * log_error_rc(rc, ...)    : log_error msg - return code
 * log_error_rz(..,)        : log_error msg - return zero
 * log_error_rn(..,)        : log_error msg - return NUL
 * -
 * log_errno(...)           : log error msg + errno
 * log_errno_rf(...)        : log error msg + errno - return fail
 * log_errno_rc(...)        : log error msg + errno - return code
 * log_errno_rn(...)        : log error msg + errno - return null
 * -
 * log_ec_rf(ec, ..,)       : log error msg wth ec as errno - return fail
 * log_debug(fmt, ...)      : simple wrapper around fprintf(stderr, fmt, ...)
 * fatal_error(...)         : log fatal error msg and exit
 * fatal_errno(...)         : log fatal error msg and errno and exit
 */
#define log_msg_rf(msg) ({ \
    log_msg(msg); \
    UTIL_FAIL; \
})

// report msg, return rc
#define log_info_rc(what, rc, ...) ({ \
    log_info(what,  __VA_ARGS__); \
    (rc); \
})

#define log_error(...) \
    _log_error(__FILE__, __LINE__, __func__, 0, __VA_ARGS__)

#define log_error_rf(...) ({ \
    _log_error(__FILE__, __LINE__, __func__, 0,  __VA_ARGS__); \
    UTIL_FAIL; \
})

#define log_error_rc(rc, ...) ({ \
    _log_error(__FILE__, __LINE__, __func__, 0,  __VA_ARGS__); \
    (rc); \
})

#define log_error_rz(...) ({ \
    _log_error(__FILE__, __LINE__, __func__, 0,  __VA_ARGS__); \
    UTIL_OK; \
})

#define log_error_rn(...) ({ \
    _log_error(__FILE__, __LINE__, __func__, 0,  __VA_ARGS__); \
    (void *) NULL; \
})

#define log_errno(...) \
    _log_error(__FILE__, __LINE__, __func__, errno,  __VA_ARGS__)

#define log_errno_rf(...) ({ \
    _log_error(__FILE__, __LINE__, __func__, errno,  __VA_ARGS__); \
    UTIL_FAIL; \
})

#define log_errno_rc(rc, ...) ({ \
    _log_error(__FILE__, __LINE__, __func__, errno,  __VA_ARGS__); \
    (ec); \
})

#define log_errno_rn(...) ({ \
    _log_error(__FILE__, __LINE__, __func__, errno,  __VA_ARGS__); \
    (void *) NULL; \
})

#define log_ec_rf(ec, ...) ({ \
    _log_error(__FILE__, __LINE__, __func__, ec,  __VA_ARGS__); \
    UTIL_FAIL; \
})

#define fatal_error(...) _fatal_error(__FILE__, __LINE__, __func__, 0,  __VA_ARGS__)
#define fatal_errno(...) _fatal_error(__FILE__, __LINE__, __func__, errno,  __VA_ARGS__)
#define log_debug(fmt, ...) { fprintf(stderr, fmt, ##__VA_ARGS__); fprintf(stderr, "\n"); }

#endif
