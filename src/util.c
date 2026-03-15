/*
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <getopt.h>

#include "util.h"
#include "log.h"

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

// wrapper around getopt_long

int getopt_init(struct getopt_parse *parse, 
    int argc, char *argv[],
    size_t num_opt, struct get_opt opts[num_opt])
{
    memset(parse->long_opts, 0, sizeof(parse->long_opts));

    parse->argc = argc >= 0 ? argc : 0;
    parse->argv = argv;

    parse->opts = opts;
    parse->num_opt = num_opt;

    if (num_opt > GETOPT_MAX) {
        return log_error_rf("Num opts %zu > max %d", num_opt, GETOPT_MAX);
    }

    // disable getopt error reporiing
    opterr = 0;

    for (size_t i = 0; i < num_opt; i++) {
        struct option *long_opt = &parse->long_opts[i];
        long_opt->name = opts[i].name;
        long_opt->has_arg = opts[i].has_arg;
        long_opt->val = opts[i].val;
    }

    return 0;
}

int getopt_next(struct getopt_parse *parse)
{
    int rc = getopt_long_only(
        parse->argc, parse->argv,
        ":", parse->long_opts, 
        &parse->opt_idx
    );

    if (rc == -1) {
        // all cmd-line options parsed
        return GETOPT_EOF;
    }

    if (rc == ':') {
        //  Missing value
        struct get_opt *opt = getopt_missopt(parse);
        return log_error_re(GETOPT_MISSVAL, "Option: --%s requries an arg", opt->name);
    }

    if (rc == '?') {
        // Unknown option
        const char *opt = getopt_erropt(parse);
        return log_error_re(GETOPT_ERROPT, "Error: Unknown option %s", opt);
    }
    
    parse->val = slice_make_cstr(optarg);
        
    // option code
    return rc;
}

void print_usage(const char *cmd, 
    int num_opt, const struct get_opt opts[num_opt],
    int num_exa, char *examples[num_exa])
{
    const char *base = strrchr(cmd, '/');
    const char *prog_name = (base) ? base + 1 : cmd;
    int w= 15;

    printf("Usage: %s [OPTIONS]\n\n", prog_name);
    printf("Options:\n");

    for (int i = 0; i < num_opt; i++) {
        printf(" --%-*s %s", w, opts[i].name, opts[i].desc);
        if (opts[i].def_type) {
            const char *def_str = opts[i].def_type == 1
                ? int_tostr(opts[i].def_int)
                : opts[i].def_str;
            printf(" (default=%s)", def_str);
        }
        printf("\n");
    }

    if (!num_exa) return;

    printf("\nExamples:\n");
    for (int i = 0; i < num_exa; i++) {
        printf("  %s %s\n", prog_name, examples[i]);
    }
}
