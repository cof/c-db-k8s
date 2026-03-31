/*
 * Util api
 * --------
 * See util.h for description
 *
 * API sections
 * ------------
 * sys errors : general error codes
 * gen macros : array len, string literal, aligment, rmconst
 * ptr macros : ptr manipulation
 * str macros : Stringification
 * min-max    : safe min/max funcs
 * signal     : simple signal handler api
 * string     : misc string api
 * codec      : simple encoders and decoders
 * strbuf     : for simple string write buffer
 * str_slice  : for a memory view (buf+len)
 * setter     : for setting string and int values
 * cmd-line   : cmd-line parser api
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <getopt.h>

#include "util.h"
#include "log.h"

int verbose = 0;

// signal handling
static struct simple_sig *glob_sig = NULL;

// catch the signal and set run to 0
static void handle_signal(int signo, siginfo_t *info, void *ucontext)
{
    (void) ucontext;

    if (!glob_sig) return;

    glob_sig->signo = signo;
    glob_sig->pid = 0;
    glob_sig->uid = 0;

    if (info && info->si_code <= 0) {
        glob_sig->pid = info->si_pid;
        glob_sig->uid = info->si_uid;
    }

    // tell user
    glob_sig->run = 0;
}

// setup signal handler for app
int setup_signals(struct simple_sig *sig)
{
    if (!sig) return -1;
    struct sigaction sa = { 0 };

    sa.sa_sigaction = handle_signal;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        return log_errno_rf("setup sigint");
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        return log_errno_rf("setup sigterm");
    }

    // XXX prevent write(fd) trigger a signal
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        return log_errno_rf("setup SIGPIPE");
    }

    sig->run = 1;
    glob_sig = sig;

    // all done
    return 0;
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

// store ascii repr of int to string buffer
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

// convert int to string - uses wrap-around buffer list
char *int_tostr(int val) 
{
    static char bufs[16][10];
    static int idx;

    char *str = bufs[idx];
    idx = (idx + 1) & 15;

    return itoa(str, sizeof(bufs[0][0]), val);
}

// generate a string using a snprintf to buffer
int gen_str(char *buf, size_t len, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    int rc = vsnprintf(buf, len, fmt, args);
    va_end(args);

    if (rc < 0) {
        return log_errno_rf("gen-str printf failed");
    }

    if ((size_t) rc >= len) {
        return log_error_rf("gen-str no space");
    }

    return 0;
}

// generic setters
int str_setval(char **str, const char *name, const char *val_str)
{
    if (*str) free(*str);
    *str = strdup(val_str);
    if (!*str) {
        return log_errno_rf("%s strdup failed", name);
    }

    return 0;
}

int int_setval(int *ival, const char *name, const char *val_str)
{
    int val = strtol(val_str, NULL, 0);
    if (val < 0) {
        return log_error_rf("%s cannot be negative", name);
    }
    *ival = val;

    return 0;
}

int uint_setval(uint32_t *uval, const char *name, const char *val_str)
{
    (void) name;
    *uval = strtoul(val_str, NULL, 0);
    return 0;
}

/*
 *  cmd-line parsing API
 *  --------------------
 *  Uses a simple stateful iterator over cmd-line args.
 *  No malloc just pass it arg,argv and array of opts
 *
 *  Example Usage:
 *  =============
 *  struct cmd_opt opts[] = {
 *       // name, desc, def, has_arg, code
 *      { "--opt1", "description", "default", 1, 0 }:
 *      { "--opt2", "description", "default", 1, 0 }:
 *  };
 *
 *  int main(int argc, char *argv[]) {
 *  struct cmd_argv parser = { argc, argv, opts };
 *  while ( (rc = cmd_argv_next(&parser)) >= 0) {
 *      printf("opt %d name=%s value=%s\n", rc, parser->name, parser->value);
 *      switch(rc) {
 *      case 0:
 *      case 1:
 *      }
 *   }
 *   if (rc != OPT_EOF) { printf("Error\n"); exit(1));
 *  return 0;
 * }
 *
 */
int opt_setstr(char **str, struct cmd_argv *parse)
{
    return str_setval(str, parse->name, parse->value);
}

int opt_setint(int *iptr, struct cmd_argv *parse)
{
    return int_setval(iptr, parse->name, parse->value);
}

int opt_setuint(uint32_t *iptr, struct cmd_argv *parse)
{
    return uint_setval(iptr, parse->name, parse->value);
}

static int find_opt(const char *name, const struct cmd_opt opts[])
{
    for (int i = 0; opts[i].name; i++) {
        if (strcmp(name, opts[i].name) == 0) {
            return i;
        }
    }
    // not found
    return -1;
}

int cmd_argv_next(struct cmd_argv *parse) 
{
    // skip prog name
    if (parse->argv_idx == 0) parse->argv_idx++;
    if (parse->argv_idx >= parse->argc) return OPT_EOF;

    // match name
    parse->name = parse->argv[parse->argv_idx++];
    parse->opt_idx = find_opt(parse->name, parse->opts);
    if (parse->opt_idx == -1) {
        return log_error_rc(OPT_UNSUPP, "Error: Unknown option %s", parse->name);
    }
    parse->opt = parse->opts + parse->opt_idx;

    // get value
    parse->value = NULL;
    if (parse->opt->has_arg) {
        if (parse->argv_idx >= parse->argc) {
            return log_error_rc(OPT_MISSVAL, "Option: --%s Missing a value", parse->name);
        }
        if (parse->argv[parse->argv_idx][0] == '-') { 
            return log_error_rc(OPT_MISSVAL, "Option: --%s Missing value", parse->name);
        }
        // save value
        parse->value = parse->argv[parse->argv_idx++];
    }

    // tell user
    return parse->opt->code ? parse->opt->code : parse->opt_idx;
}

// print program usage
void prog_usage(const char *prog_name, const struct cmd_opt opts[], const char *examples[])
{
    const char *name = get_basename(prog_name) ?: "<null>";
    int w = 15;

    printf("Usage: %s [OPTIONS]\n\n", name);
    printf("Options:\n");

    for (int i = 0; opts[i].name; i++)  {
        printf(" %-*s %s", w, opts[i].name, opts[i].desc);
        if (opts[i].def_str) {
            printf(" (default=%s)", opts[i].def_str);
        }
        printf("\n");
    }

    printf("\nExamples:\n");
    for (int i = 0; examples[i]; i++)  {
        printf("  %s %s\n", name, examples[i]);
    }
}

// find cmd_mode entry
struct cmd_mode *cmd_mode_find(char *mode, struct cmd_mode modes[])
{
    for (size_t i = 0; modes[i].name; i++) {
        if (!strcmp(mode, modes[i].name)) return &modes[i];
    }

    return NULL;
}

// print mode usage
void mode_usage(const char *prog_name, struct cmd_mode modes[], const char *examples[])
{
    const char *name = get_basename(prog_name) ?: "<null>";
    int w = 15;

    printf("Usage: %s [MODE] [OPTIONS]\n\n", name);

    // list modes
    printf("MODE:\n");
    for (size_t i = 0; modes[i].name; i++) {
        printf("  %-*s %s\n", w, modes[i].name, modes[i].desc);
    }
    printf("\n");

    // list options
    for (size_t i = 0; modes[i].name; i++) {
        printf("%s Options:\n", modes[i].name);
        struct cmd_opt *opts = modes[i].opts;
        for (size_t j = 0; opts[j].name; j++) {
            struct cmd_opt *opt = &opts[j];
            printf("  %-*s %s\n", w, opt->name, opt->desc);
        }
        printf("\n");
    }

    // list examples
    printf("Examples:\n");
    for (int i = 0; examples[i]; i++)  {
        printf("  %s %s\n", name, examples[i]);
    }

}
