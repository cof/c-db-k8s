/* 
 * read write buffer api
 */
#ifndef _RWBUF_H_
#define _RWBUF_H_

#include <sys/uio.h>

struct rwbuf {
    uint8_t *data; // memory buffer
    size_t max_size;  // max size buffer can reach
    size_t size;  // total capacity in bytes
    size_t ridx;  // read index
    size_t widx;  // write index
    unsigned int is_malloc : 1; // malloc or static buffer
    unsigned int is_grow   : 1; // can realloc
};

// api

#define RWBUF_MAX_SIZE (64 * 1024)

#define RWBUF_LOAD(_buf, _len)  { \
    .data = _buf, \
    .max_size = 0, \
    .size = _len, \
    .ridx = 0,  \
    .widx = 0, \
    .is_malloc = 0, \
    .is_grow = 0 \
}

void *rwbuf_mkspace(struct rwbuf *buf, size_t need);
int rwbuf_init(struct rwbuf *buf, size_t init_size, size_t max_size);
void rwbuf_deinit(struct rwbuf *buf);

int rwbuf_write(struct rwbuf *buf, void *data, size_t len);
int rwbuf_writev(struct rwbuf *buf, int nbuf, struct iovec iovs[nbuf]);
int rwbuf_readline(struct rwbuf *buf, struct str_slice *line, int eof);

// inline helpers
static inline uint8_t *rwbuf_rptr(struct rwbuf *buf)
{
    return buf->data + buf->ridx;
}

static inline size_t rwbuf_used(struct rwbuf *buf)
{
    return buf->widx - buf->ridx;
}

static inline uint8_t *rwbuf_wptr(struct rwbuf *buf)
{
    return buf->data + buf->widx;
}

static inline size_t rwbuf_space(struct rwbuf *buf)
{
    return buf->size - buf->widx;
}

static inline int rwbuf_ridx_adj(struct rwbuf *buf, size_t len)
{
    if (buf->ridx + len > buf->size) {
        return log_error_rf(
            "rbwbuf overflow ridx(%zu) + len(%zu) > cap %zu", 
            buf->ridx, len, buf->size);
    }

    buf->ridx += len;
    if (buf->ridx == buf->widx) {
        // empty - reset indexs
        buf->ridx = 0;
        buf->widx = 0;
    }

    return 0;
}

static inline size_t iovs_len(int nbuf, struct iovec iovs[nbuf])
{
    size_t len = 0;
    for (int i = 0; i < nbuf; i++)  {
        len += iovs[i].iov_len;
    }
    return len;
}

static inline void load_iov(struct iovec *iov, void *data, size_t len)
{
    iov->iov_base = data;
    iov->iov_len = len;
}

#endif
