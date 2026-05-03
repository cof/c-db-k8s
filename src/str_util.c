/*
 * STR_UTIL api
 * -------------
 * See str_util.h for description
 *
 */

#include "str_util.h"


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
size_t ip4_str_encode(const uint8_t addr[static 4], char *str, size_t len)
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
size_t ip6_str_encode(const uint8_t addr[static 16], int flags, char *str, size_t len)
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
