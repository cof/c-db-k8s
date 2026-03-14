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


int getopt_init(struct getopt_parse *parse, 
    int argc, char *argv[],
    size_t num_opt, struct get_opt opts[num_opt])
{
    if (num_opt > GETOPT_MAX) {
        return log_error_rf("Num opts %zu > max %d", num_opt, GETOPT_MAX);
    }

    parse->argc = argc;
    parse->argv = argv;

    parse->opts = opts;
    parse->num_opt = num_opt;


    for (size_t i = 0; i < num_opt; i++) {
        struct option *long_opt = &parse->long_opts[i];
        long_opt->name = opts[i].name;
        long_opt->has_arg = opts[i].has_arg;
        long_opt->flag = NULL;
        long_opt->val = opts[i].val;
    }

    memset(&parse->long_opts[num_opt], 0, sizeof(parse->long_opts[0]));

    return 0;

}

int getopt_next(struct getopt_parse *parse)
{
    int rc = getopt_long_only(
        parse->argc, parse->argv,
        "", parse->long_opts, 
        &parse->opt_idx
    );

    if (rc == -1) return rc;
    if (rc == '?' || rc == ':') {
        parse->opt_idx = optind -1;
        return rc;
    }
    
    parse->val = slice_make_cstr(optarg);
    
    return rc;
}
