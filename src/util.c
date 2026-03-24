/*
 *
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

    glob_sig->run = 0;
}

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
    int val = atoi(val_str);
    if (val < 0) {
        return log_error_rf("%s cannot be negative", name);
    }
    *ival = val;

    return 0;
}

// cmd-line parsing
int opt_setstr(void *state, size_t offset, const char *name, const char *val)
{
    char **str = make_ptr(state, offset);
    
    return str_setval(str, name, val);
}

int opt_setint(void *state, size_t offset, const char *name, const char *val)
{
    int *ival = make_ptr(state, offset);
    
    return int_setval(ival, name, val);
}

static const struct cmd_opt *find_opt(const char *name, const struct cmd_opt opts[])
{
    for (int i = 0; opts[i].name; i++) {
        if (strcmp(name, opts[i].name) == 0) {
            return &opts[i];
        }
    }

    return NULL;
}

int parse_argv(int argc, char *argv[], const struct cmd_opt opts[], void *state)
{
    int i;

    for (i = 1; i < argc; i++) {
        const char *name = argv[i];
        if (*name != '-') {
            // positional argument
            return i;
        }
        // match name
        const struct cmd_opt *opt = find_opt(name, opts);
        if (!opt) {
            return log_error_rf( "Error: Unknown option %s", name);
        }
        // get value
        const char *val = NULL;
        if (opt->has_arg) {
            if (i + 1 >= argc) {
                return log_error_rf("Option: --%s requries a value", opt->name);
            }
            if (argv[i + 1][0] == '-') { 
                return log_error_rf("Option: --%s Missing value", opt->name);
            }
            val = argv[++i];
        }
        // tell user
        int rc = opt->setter(state, opt->offset, name, val);
        if (rc) return rc;
    }

    return i;
}



void print_usage(const char *cmd, const struct cmd_opt opts[], const char *examples[])
{
    const char *prog_name = get_basename(cmd);
    int w= 15;

    printf("Usage: %s [OPTIONS]\n\n", prog_name);
    printf("Options:\n");

    for (int i = 0; opts[i].name; i++)  {
        printf(" --%-*s %s", w, opts[i].name, opts[i].desc);
        if (opts[i].def_str) {
            printf(" (default=%s)", opts[i].def_str);
        }
        printf("\n");
    }

    printf("\nExamples:\n");
    for (int i = 0; examples[i]; i++)  {
        printf("  %s %s\n", prog_name, examples[i]);
    }
}
