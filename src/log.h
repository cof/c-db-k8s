#ifndef __LOG_H__
#define __LOG_H__

#include <errno.h>

extern int verbose;

// logger
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

#define log_msg_rf(msg) ({ \
    log_msg(msg); \
    UTIL_FAIL; \
})

// report msg, return 0
#define log_info_rz(what, ...) ({ \
    log_info(what,  __VA_ARGS__); \
    UTIL_OK; \
})

// report estr
#define log_error(...) \
    _log_error(__FILE__, __LINE__, __func__, 0, __VA_ARGS__)

// report estr, return FAIL
#define log_error_rf(...) ({ \
    _log_error(__FILE__, __LINE__, __func__, 0,  __VA_ARGS__); \
    UTIL_FAIL; \
})

#define log_error_re(ec, ...) ({ \
    _log_error(__FILE__, __LINE__, __func__, 0,  __VA_ARGS__); \
    (ec); \
})

// report estr, return 0
#define log_error_rz(...) ({ \
    _log_error(__FILE__, __LINE__, __func__, 0,  __VA_ARGS__); \
    UTIL_OK; \
})

// report estr, return NULL
#define log_error_rn(...) ({ \
    _log_error(__FILE__, __LINE__, __func__, 0,  __VA_ARGS__); \
    (void *) NULL; \
})

// report errno + estr
#define log_errno(...) \
    _log_error(__FILE__, __LINE__, __func__, errno,  __VA_ARGS__)

// report errno + estr, return NULL
#define log_errno_rn(...) ({ \
    _log_error(__FILE__, __LINE__, __func__, errno,  __VA_ARGS__); \
    (void *) NULL; \
})

// report errno + estr, return ec
#define log_errno_re(ec, ...) ({ \
    _log_error(__FILE__, __LINE__, __func__, errno,  __VA_ARGS__); \
    (ec); \
})

// report errno + estr, return FAIL
#define log_errno_rf(...) ({ \
    _log_error(__FILE__, __LINE__, __func__, errno,  __VA_ARGS__); \
    UTIL_FAIL; \
})

#define log_ec_rf(ec, ...) ({ \
    _log_error(__FILE__, __LINE__, __func__, ec,  __VA_ARGS__); \
    UTIL_FAIL; \
})


#define fatal_error(...) _fatal_error(__FILE__, __LINE__, __func__, 0,  __VA_ARGS__)
#define fatal_errno(...) _fatal_error(__FILE__, __LINE__, __func__, errno,  __VA_ARGS__)
#define log_errorn(...) (log_error(__VA_ARGS__), (void*)NULL)
#define log_debug(fmt, ...) { fprintf(stderr, fmt, ##__VA_ARGS__); fprintf(stderr, "\n"); }

#endif
