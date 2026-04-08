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

#include "log.h"
#include "util.h"

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

// print ascii repr of int to string buffer
char *itoa(int val, char *buf, size_t len)
{
    if (!buf || len == 0) return buf;

    // start at buffer end - no reverse needed
    char *str = &buf[len -1];
    *str = '\0';
    if (val == 0) *--str = '0';

    while (val) {
        *--str = (val % 10) + '0';
        val /= 10;
    }

    return str; 
}

// convert int-val to string - uses wrap-around buffer list
char *int_tostr(int val) 
{
    static char bufs[16][10];
    static int idx;

    char *str = bufs[idx];
    idx = (idx + 1) & 15;

    return itoa(val, str, sizeof(bufs[0][0]));
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

/* inet API
 * --------
 */

// decode IPv4 addr-str e.g "172.0.1.10"
size_t ip4_str_decode(const char *str, size_t len, uint8_t dst[static 4])
{
    const char *str_ptr = str;
    const char *str_end = str + len;
    uint8_t *dst_ptr = dst;
    int num_digit = 0, num_dot = 0;
    uint32_t acc = 0;

    while (str_ptr < str_end) {
        int ch = *str_ptr++;
        switch(ch) {
        case '0' ... '9':
            // digit
            if (++num_digit > 3) return -1;
            acc = (acc << 3) + (acc << 1) + (ch - '0');
            if (acc > 255) return 0;
            break;
        case '.':
            // dot
            if (num_digit == 0 || num_dot >= 3) return 0;
            *dst_ptr++ = acc;
            num_dot++;
            acc = 0;
            num_digit = 0;
            break;
        default: 
            // unknown char - stop
            str_end = str;
        }
    }

    if (num_dot != 3 || num_digit == 0) return 0;
    *dst_ptr = acc;

    // bytes read
    return str_ptr - str;
}

// decode IPv6 addr-str - e.g "2001:db8::7"
size_t ip6_str_decode(const char *str, size_t len, uint8_t dst[static 16])
{
    const char *str_ptr = str;
    const char *str_end = str + len;
    const char *seg_ptr = str;
    uint8_t *dst_ptr = dst;
    uint8_t *zero_ptr = NULL;
    int num_hex = 0, num_seg = 0;
    uint32_t acc = 0;

    while (str_ptr < str_end) {
        int ch = *str_ptr++;
        switch(ch) {
        case '0' ... '9':
        case 'A' ... 'F':
        case 'a' ... 'f':
            // hex-digit
            if (++num_hex > 4) return 0;
            acc = acc << 4 | hex_to_nibble(ch);
            break;
        case ':':
            // end-segment
            if (num_hex) {
                if (num_seg >= 7) return 0;
                //if (dst_ptr + 2 > end) return 0;
                dst_ptr = enc_u16(dst_ptr, acc);
            }
            // double-colon
            if (str_ptr < str_end && *str_ptr == ':') {
                if (zero_ptr) return 0; // 2nd '::'
                zero_ptr = dst_ptr;
                str_ptr++;
            }
            // next-segment
            num_hex = acc = 0;
            seg_ptr = str_ptr + 1;
            num_seg++;
            break;
        case '.':
            // ip4-addr
            if (num_seg > 6) return 0;
            num_hex = 0;
            str_ptr = seg_ptr;
            str_ptr += ip4_str_decode(str_ptr, str_end - str_ptr, dst_ptr);
            if (str_ptr == seg_ptr) return 0;
            dst_ptr += 4;
            num_seg += 2;
            // stop
            str_end = str;
            break;
        default: 
            // stop
            str_end = str;
        }
    }

    // last-segment
    if (num_hex) {
        //if (dst_ptr + 2 > end) return 0;
        dst_ptr = enc_u16(dst_ptr, acc);
        num_seg++;
    }

    if (zero_ptr) {
        // zero-fill gap (8 - num_seg)
        size_t zero_move = dst_ptr - zero_ptr;
        size_t zero_size = 16 - (dst_ptr - dst);
        memmove(dst + 16 - zero_move, zero_ptr, zero_move);
        memset(zero_ptr, 0, zero_size);
        num_seg = 8;
    }

    if (num_seg != 8) return 0;

    // bytes read
    return str_ptr - str;
}

// convert ip4-addr to str
size_t ip4_str_encode(uint8_t addr[static 4], char *str, size_t len)
{
    if (len < IP4_ADDR_STRLEN) return 0;

    char *wptr = str;
    wptr = uint8_toa(wptr, addr[0]); *wptr++ = '.';
    wptr = uint8_toa(wptr, addr[1]); *wptr++ = '.';
    wptr = uint8_toa(wptr, addr[2]); *wptr++ = '.';
    wptr = uint8_toa(wptr, addr[3]); 
    *wptr = '\0';
    
    return wptr - str;
}

// convert ip6-addr to str
size_t ip6_str_encode(uint8_t addr[static 16], int flags, char *str, size_t len)
{
    size_t need_len = IP6_ADDR_STRLEN;
    if (flags & IP6_STR_ADDBRACK) need_len += 2;
    if (len < need_len) return 0;

    int zero_pos = -1, zero_len = 0;
    int cur_pos = -1, cur_len = 0;

    // find longest run of zeros
    for (int i = 0; i < 16; i += 2) {
        uint16_t val = addr[i] << 8 | addr[i + 1];
        if (val == 0) {
            // zero
            if (cur_pos == -1) cur_pos = i;
            cur_len += 2;
        }
        else {
            // not-zero
            if (cur_len > zero_len) {
                zero_pos = cur_pos;
                zero_len = cur_len;
            }
            cur_pos = -1; cur_len = 0;
        }
    }

    // last-check
    if (cur_len > zero_len) {
        zero_pos = cur_pos;
        zero_len = cur_len;
    }

    char *wptr = str;
    char *wend = str + len;
    int addr_len = 16;

    // rfc5952 - 4.2.2. Handling One 16-Bit 0 Field
    if (zero_len <= 2) zero_pos = -1;

    // rfc5952 - IPv4-Mapped or IPv4-Compatible address
    if ((flags & IP6_STR_NOIPV4) == 0 && zero_pos == 0 && 
        ((zero_len == 10 && addr[10] == 0xff && addr[11] == 0xff) ||
        (zero_len == 12)))
    {
        if (flags & IP6_STR_STRIPV4) {
            return ip4_str_encode(addr + 12, wptr, wend - wptr);
        }
        addr_len = 12;
    }

    if (flags & IP6_STR_ADDBRACK) *wptr++ = '[';

    for (int i = 0; i < addr_len; i += 2) {
        if (i || i == zero_pos) {
            *wptr++ = ':';
            if (i == zero_pos) {
                // zero compression
                *wptr++ = ':';
                i += zero_len;
                if (i >= 16) break;
            }
        }
        uint16_t val = addr[i] << 8 | addr[i + 1];
        wptr = uint16_toax(wptr, val);
    }

    if (addr_len == 12) {
        if (zero_len == 10) *wptr++ = ':';
        wptr += ip4_str_encode(addr + 12, wptr, wend - wptr);
    }

    if (flags & IP6_STR_ADDBRACK) *wptr++ = ']';

    *wptr = '\0';

    return wptr - str;
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
