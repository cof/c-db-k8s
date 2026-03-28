/* 
 * RWBUF -  A simple memory buffer API
 * -----------------------------------
 * A simple API for memory buffer management featuring
 * - Structure-composable: built for inline embedding, object compostion & memory locality
 * - Flexible memory: Support dynamic or user supplied static buffers
 * - Dynamic limits : support for locked or max_size limits
 * - Position independent: size_t read|write offsets for safe memory relocation/resizing
 * - scatter-gather - vectorzed memory tranfers via writev   
 * - support for readline with max length enforcement
 */
#ifndef _RWBUF_H_
#define _RWBUF_H_

#include <sys/uio.h>

// state structure
struct rwbuf {
    uint8_t *data; // memory buffer
    size_t max_size;  // max size buffer can reach
    size_t size;  // total capacity in bytes
    size_t ridx;  // read index
    size_t widx;  // write index
    unsigned int is_malloc  : 1; // malloc or static buffer
    unsigned int is_grow    : 1; // can realloc
};

/*
 * Initialization
 * --------------
 * RWBUF_INIT(buf, len) : setup static buffer
 * rwbuf_init(buf, init_size, max_size) : setup dynamic buffer
 * rwbuf_deinit(buf) : free memory
 */
#define RWBUF_INIT(_buf, _len)  { \
    .data = _buf, \
    .max_size = 0, \
    .size = _len, \
    .ridx = 0,  \
    .widx = 0, \
    .is_malloc = 0, \
    .is_grow = 0 \
}

int rwbuf_init(struct rwbuf *buf, size_t init_size, size_t max_size);
void rwbuf_deinit(struct rwbuf *buf);
void *rwbuf_mkspace(struct rwbuf *buf, size_t need);

/*
 * read and write
 * ---------------
 * rwbuf_mkspace(buf, need_len)   : reserve write space in buffer
 * rwbuf_write(buf, buf, len)     : append memory buffer
 * rwbuf_writev(buf, niov, ivovs) : append memory buffers
 */
int rwbuf_write(struct rwbuf *buf, void *data, size_t len);
int rwbuf_writev(struct rwbuf *buf, int nbuf, struct iovec iovs[nbuf]);

/*
 * readline flags
 */
#define RWBUF_EOF    0x1 // return terminal fragment if eof
#define RWBUF_NOLOG  0x2 // dont log max_line error
#define RWBUF_ADDNUL 0x4 // add nul terminator to line

/*
 * rwbuf_readline(buf, str, max_line, flags) : read a line (CRLF or LF terminated)
 *  e.g 
 *   rc = rwbuf_readline(sock->recv_buf, &line, 128, RWBUF_NOLOG)
 */
int rwbuf_readline(struct rwbuf *buf, struct str_slice *line, size_t max, uint32_t flags);

/*
 * Helpers
 * -------
 * rwbuf_rptr(buf)  : return ptr to readable dataq
 * rwbuf_wptr(buf)  : return ptr to writable space
 * rwbuf_used(buf)  : return size of readable data
 * rwbuf_space(buf) : return size of writeable space 
 * rwbuf_rdinc(buf) : increment read buffer index by len
 *
 * iovs_len(niov, iovs)    : calc total iov_len of iovs
 * iov_load(iov, buf, len) : load buffer address into iovec 
 */
static inline uint8_t *rwbuf_rptr(struct rwbuf *buf)
{
    return buf->data + buf->ridx;
}

static inline uint8_t *rwbuf_wptr(struct rwbuf *buf)
{
    return buf->data + buf->widx;
}

static inline size_t rwbuf_used(struct rwbuf *buf)
{
    return buf->widx - buf->ridx;
}


static inline size_t rwbuf_space(struct rwbuf *buf)
{
    return buf->size - buf->widx;
}

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

static inline size_t iovs_len(int niov, const struct iovec iovs[static niov])
{
    size_t len = 0;

    for (int i = 0; i < niov; i++)  {
        len += iovs[i].iov_len;
    }

    return len;
}

static inline void iov_load(struct iovec *iov, void *buf, size_t len)
{
    iov->iov_base = buf;
    iov->iov_len = len;
}

#endif
