/* 
 * A simple read|write buffer API
 * ------------------------------
 *
 * rwbuf_init    : init buffer state  e.g rwbuf_init(buf, 1024, 4096)
 * rwbuf_deinit  : free memory 
 * rwbuf_mkspace : reserve write space in buffer e.g rwbuf_mkspace(buf, 128)
 *
 * rwbuf_write   : write data to buffer  e.g rwbuf_write(buf, data, len)
 * rwbuf_writev  : write iovec array to buffer e.g. rwbuf(buf, niov, iovs)
 *
 * int rwbuf_readline(buf, line, max, flags)
 *
 *  read a line (CRLF or LF terminated)
 *  
 *  Args:
 *  -----
 *  buf   - addr of rwbuf
 *  line  - addr of str slice
 *  max   - max line size
 *  flags - bit mask or flags
 *
 *  RWBUF_EOF    - EOF read rest of buffer as line
 *  RWBUF_NOLOG  - dont log max line error
 *  RWBUF_ADDNUL - add nul terminator to str
 *
 *  e.g 
 *   rc = rwbuf_readline(sock->recv_buf, &line, 128, flags)
 *
 * Helpers
 * -------
 * rwbuf_rptr : return ptr to readable data
 * rwbuf_used : return size of readable data
 * rwbuf_wptr : return ptr to write space
 * rwbuf_space : return size of write space
 * rwbuf_rdinc : increment read buffer index by len  
 *
 * iovs_len  : calc total iov_len of iovec array
 * iovs_load : load data into iovec
 * 
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
    unsigned int is_malloc  : 1; // malloc or static buffer
    unsigned int is_grow    : 1; // can realloc
};

// api

#define RWBUF_LOAD(_buf, _len)  { \
    .data = _buf, \
    .max_size = 0, \
    .size = _len, \
    .ridx = 0,  \
    .widx = 0, \
    .is_malloc = 0, \
    .is_grow = 0 \
}

// Create, res
int rwbuf_init(struct rwbuf *buf, size_t init_size, size_t max_size);
void rwbuf_deinit(struct rwbuf *buf);
void *rwbuf_mkspace(struct rwbuf *buf, size_t need);

int rwbuf_write(struct rwbuf *buf, void *data, size_t len);
int rwbuf_writev(struct rwbuf *buf, int nbuf, struct iovec iovs[nbuf]);

// readline flags
#define RWBUF_EOF    0x1
#define RWBUF_NOLOG  0x2
#define RWBUF_ADDNUL 0x4
int rwbuf_readline(struct rwbuf *buf, struct str_slice *line, size_t max, uint32_t flags);

/*  inline helpers */


// return pointer to readable data
static inline uint8_t *rwbuf_rptr(struct rwbuf *buf)
{
    return buf->data + buf->ridx;
}

// return size of readable data
static inline size_t rwbuf_used(struct rwbuf *buf)
{
    return buf->widx - buf->ridx;
}

// return ptr to write space
static inline uint8_t *rwbuf_wptr(struct rwbuf *buf)
{
    return buf->data + buf->widx;
}

// return size of write space
static inline size_t rwbuf_space(struct rwbuf *buf)
{
    return buf->size - buf->widx;
}

// increment read buffer index by len
static inline int rwbuf_rdinc(struct rwbuf *buf, size_t len)
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

// calc total iov_len of iovec array
static inline size_t iovs_len(int nbuf, struct iovec iovs[nbuf])
{
    size_t len = 0;
    for (int i = 0; i < nbuf; i++)  {
        len += iovs[i].iov_len;
    }
    return len;
}

// load data into iovec 
static inline void iov_load(struct iovec *iov, void *data, size_t len)
{
    iov->iov_base = data;
    iov->iov_len = len;
}

#endif
