/*
 * RWBUF - A simple memory buffer API
 * ----------------------------------
 * See rwbuf.h for API description.
 *
 * API sections
 * ------------
 * Init : init buffer state
 * Data I/O : read and write data to buffer
 * Line I/O : read and write line to buffer
 * Helpers  : buffer status and iov loader
 */
#include <stdio.h>
#include <stdlib.h>

#include "util.h"
#include "log.h"
#include "rwbuf.h"

// setup dynamic buffer
int rwbuf_init(struct rwbuf *buf, size_t init_size, size_t max_size)
{
    memset(buf, 0, sizeof(*buf));

    buf->max_size = max_size;
    buf->is_malloc = 1;
    buf->is_grow   = 1;

    if (init_size && !rwbuf_mkspace(buf, init_size)) {
        // no room
        return -1;
    }

    return 0;
}

// free memory
void rwbuf_deinit(struct rwbuf *buf)
{
    if (buf->data && buf->is_malloc) {
        free(buf->data);
        buf->data = NULL;
    }

    buf->size = 0;
    buf->ridx = 0;
    buf->widx = 0;
}

// reserve write space in buffer
void *rwbuf_mkspace(struct rwbuf *buf, size_t need)
{
    size_t space = buf->size - buf->widx;

    if (space >= need) {
        // have room
        return buf->data + buf->widx;
    }

    // can shift data down
    size_t used = buf->widx - buf->ridx;
    if (buf->ridx > 0) {
        memmove(buf->data, buf->data + buf->ridx, used);
        buf->ridx = 0;
        buf->widx = used;
    }

    space = buf->size - buf->widx;
    if (space < need) {
        // not enough space
        if (!buf->is_grow) return NULL;
        // calc new size
        size_t size = buf->size * 2;
        if (size < buf->widx + need) {
            size = buf->widx + need;
        }
        size = ALIGN_UP(size, 128);
        if (buf->max_size && size > buf->max_size) {
            return log_errno_rn("rwbuf size %zu > max %zu", size, buf->max_size);
        }
        // resize now
        uint8_t *data = realloc(buf->data, size);
        if (!data) {
            return log_errno_rn("rwbuf realloc %zu failed", size);
        }
        buf->data = data;
        buf->size = size;
    }

    // have room
    return buf->data + buf->widx;
}

// append memory buffer contents
int rwbuf_write(struct rwbuf *buf, void *data, size_t len)
{
    void *space = rwbuf_mkspace(buf, len);
    if (!space) return -1;

    memcpy(space, data, len);
    buf->widx += len;

    return 0;
}

// append memory buffers contents
int rwbuf_writev(struct rwbuf *buf, int nbuf, struct iovec iovs[nbuf])
{
    size_t len = iovs_len(nbuf, iovs);

    void *space = rwbuf_mkspace(buf, len);
    if (!space) return -1;

    for (int i = 0; i < nbuf; i++)  {
        space = mempcpy(space, iovs[i].iov_base, iovs[i].iov_len);
    }

    buf->widx += len;

    return 0;
}

// read a line (CRLF or LF terminated)
int rwbuf_readline(struct rwbuf *buf, struct str_slice *line, size_t max, uint32_t flags)
{
    size_t   rlen = rwbuf_used(buf);
    uint8_t *rptr = rwbuf_rptr(buf);

    uint8_t *eol = memchr(rptr, '\n', rlen);

    if (eol) {
        // found terminator
        size_t llen = eol - rptr + 1;
        char *str = (char *) rptr;

        // remove from buffer
        buf->ridx += llen;
        if (buf->ridx == buf->widx) {
            // empty buffer
            buf->ridx = 0;
            buf->widx = 0;
        }

        // trim cr/lf
        size_t len = llen;
        if (len && str[len - 1] == '\n') len--;
        if (len && str[len - 1] == '\r') len--;
        if (flags & RWBUF_ADDNUL) str[len] = '\0';

        // store line
        line->ptr = str;
        line->len = len;

        if (max && len > max) {
            if (flags & RWBUF_NOLOG) return -1;
            return log_error_rf("line too big - len %zu > max %zu", len, max);
        }

        // line length + CRLF
        return llen;
    }

    // incomplete line
    if (max && rlen > max) {
        if (flags & RWBUF_NOLOG) return -1;
        return log_error_rf("line too big - len %zu > max %zu", rlen, max);
    }

    if (flags & RWBUF_EOF) {
        // store line
        line->ptr = (char *) rptr;
        line->len = rlen;
        // empty buffer
        buf->ridx = 0;
        buf->widx = 0;
        // line length
        return rlen;
    }

    if (buf->ridx > 0) {
        // ensure partial line at buffer start
        memmove(buf->data, rptr, rlen);
        buf->ridx = 0;
        buf->widx = rlen;
    }

    // wait for eol
    return 0;
}
