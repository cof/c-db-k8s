/*
 * UTIL api
 * --------
 * A misc utility API for apps.
 *
 * API sections
 * ------------
 * sys errors  : general error codes
 * min-max     : safe min/max funcs
 * str-helpers : misc string helpers
 * signal      : simple signal handler api
 * inet        : inet api
 * setter      : for setting string and int values
 * cmd-line    : cmd-line api
 */
#ifndef _UTIL_H_
#define _UTIL_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "macros.h"
#include "str_util.h"

// system errors
#define UTIL_OK    0
#define UTIL_FAIL -1
#define UTIL_EOF  -2

/* String helpers
 * --------------
 * ec_tostr(len, estrs, ec, def) : lookup a string for ec or return default
 * dbj2a_hash(key, len)       : return dbj2a hash of key buffer
 * dbj2a_hash_str(str)        : return dbj2a hash of string
 * gen_str(buf,len,fmt,..)    : generate a string to buffer
 * get_basename(path)         : return basename of path if found
 */
static inline const char *ec_tostr(int len, const char *estr[len], int ec, const char *def)
{
    const char *str;

    str = ec >= 0 && ec < len
        ? estr[ec]
        : NULL;

    return str ?: def;
}

static inline const char *get_basename(const char *path)
{
    if (!path) return NULL;
    const char *base = strrchr(path, '/');
    return base ? base + 1 : path;
}

int gen_str(char *buf, size_t len, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));


/* min-max API
 * -----------
 * max(x,y) : return max of x and y
 * min(x,y) : return min of x and y
 */
static inline size_t max(size_t x, size_t y)
{
    return x > y ? x : y;
}

static inline size_t min(size_t x, size_t y)
{
    return x < y ? x : y;
}


/* signal handler API
 * -----------------
 * Simple single handler API for apps featuring
 * - Structure-composable: built for inline embedding, object compostion & memory locality
 * - uses sigaction
 * - catches  SIGINT|SIGTERM
 * - ignores SIGPIPE
 * - logs signal, sender uid and pid for app
 * - simple set run to 1 to 0 design
 */

// signal handler state
struct simple_sig {
    volatile sig_atomic_t run;
    int signo;
    uid_t uid;
    pid_t pid;
};

/*
 * simple_sig API
 * --------------
 * setup_signals(sig) - setup signal handler
 */
int setup_signals(struct simple_sig *sig);


#define RUN_MAXARG 32
#define RUN_CAPS 0x1
#define RUN_NULL 0x2
int run_cmd(struct sbuf *buf, int flags, const char *fmt, ...) \
    __attribute__((format(printf, 3, 4)));


/*
 * Setter API
 * ----------
 * str_setval : set str with strdup(val)
 * int_setval :  set int with atoi(val)
 * uint_setval : set uint with atoi(val)
 */
int str_setval(char **str, const char *name, const char *val_str);
int int_setval(int *ival, const char *name, const char *val_str);
int uint_setval(uint32_t *uval, const char *name, const char *val_str);


/* cmd-line */

/*
 *  cmd-argv parser API
 *  -------------------
 *  Uses a simple stateful iterator over cmd-line args.
 *  No malloc just pass it arg,argv and array of opts
 *
 *  Example Usage:
 *  =============
 *  struct cmd_opt opts[] = {
 *      // name,    desc,          def,     has_arg, code
 *      { "--opt1", "description", "default", 1,      0 }:
 *      { "--opt2", "description", "default", 1,      1 }:
 *  };
 *  struct cmd_argv parser = { argc, argv, opts };
 *  while ( (rc = cmd_argv_next(&parser)) >= 0) {
 *      switch(rc) {
 *      case 0: // opt 1
 *      case 1: // opt 2
 *      }
 *   }
 *   if (rc != OPT_EOF) { printf("Error\n"); exit(1));
 */

// has_arg codes
#define OPT_NOARG  0 // option has no arg
#define OPT_REQARG 1 // option requires arg
#define OPT_OPTARG 2 // ???

// error codes
#define OPT_EOF     -2 // no more options
#define OPT_UNSUPP  -3 // option not found
#define OPT_MISSVAL -4 // option missing value

// cmd-line option
struct cmd_opt {
    const char *name;
    const char *desc;
    const char *def_str;
    int has_arg;  // 0=none, 1=required, 2=optional
    int code;
};

// cmd-line parser state
struct cmd_argv {
    int argc;             // number of cmd-line arg
    char **argv;          // array of cmd-line arg
    struct cmd_opt *opts; // array of options
    int argv_idx;         // current argv index
    int opt_idx;          // index of matched option
    struct cmd_opt *opt;  // matched option
    const char *name;     // matched argv name
    const char *value;    // matched argv value
};

/*
 * cmd_argv API
 * ------------
 * cmd_argv_next(parser)   : return next option code
 * opt_setstr(str, parser) : set string with option value
 * opt_setint(str, parser) : set int with option value
 * opt_setuint(str, parser) : set int with option value
 */
int cmd_argv_next(struct cmd_argv *parse);
int opt_setstr(char **str, struct cmd_argv *parse);
int opt_setint(int *iptr, struct cmd_argv *parse);
int opt_setuint(uint32_t *uptr, struct cmd_argv *parse);

/*
 * cmd-API
 * -------
 * cmd_mode = cmd_mode_find(mode, modes) : find cmd-mode entry
 * prog_usage(prog_name, opts, examples) : print cmd usage
 * mode_usage(prog_name, modes, examples) : print modes usage
 */

#define MODE_RUN(f) ((int (*)(void *))(f))

// cmd runner
struct cmd_mode {
    int (*run)(void *state);
    int mode;
    struct cmd_opt *opts;
    char *name;
    char *desc;
};

struct cmd_mode *cmd_mode_find(char *mode, struct cmd_mode modes[]);
void prog_usage(const char *prog_name, const struct cmd_opt opts[], const char *examples[]);
void mode_usage(const char *prog_name, const struct cmd_mode modes[], const char *examples[]);

#endif
