/* ftpii-ng -- based on ftpii by Joseph Jordan (zlib license, see LICENSE.txt) */
#include <errno.h>
#include <malloc.h>
#include <ogc/lwp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "net.h"
#include "ui.h"
#include "xfer.h"

#define XF_BUFS 8
#define XF_BUFSZ 32768
#define XF_STACK (32 * 1024)
#define XF_PRIO 64

#define BARRIER() __asm__ volatile("" ::: "memory")

struct xfer {
    FILE *f;
    bool writing;
    u8 *buf[XF_BUFS];
    u32 len[XF_BUFS];
    volatile u32 head; /* buffers produced */
    volatile u32 tail; /* buffers consumed */
    volatile int eof;
    volatile int err;
    volatile int quit;
    u32 sendpos; /* main thread: progress inside the buffer being sent */
    u8 *cur;     /* main thread: buffer being filled by recv */
    u32 fill;
    lwp_t thread;
    bool started;
};

static void *reader_main(void *arg) {
    xfer_t *x = arg;
    while (!x->quit) {
        if (x->head - x->tail >= XF_BUFS) {
            usleep(500);
            continue;
        }
        u32 slot = x->head % XF_BUFS;
        size_t n = fread(x->buf[slot], 1, XF_BUFSZ, x->f);
        if (n > 0) {
            x->len[slot] = (u32)n;
            BARRIER();
            x->head++;
        }
        if (n < XF_BUFSZ) {
            if (ferror(x->f)) x->err = EIO;
            BARRIER();
            x->eof = 1;
            break;
        }
    }
    return NULL;
}

static void *writer_main(void *arg) {
    xfer_t *x = arg;
    for (;;) {
        int e = x->eof;
        BARRIER();
        if (x->tail == x->head) {
            if (e || x->quit) break;
            usleep(500);
            continue;
        }
        u32 slot = x->tail % XF_BUFS;
        size_t n = fwrite(x->buf[slot], 1, x->len[slot], x->f);
        if (n != x->len[slot]) {
            x->err = errno ? errno : EIO;
            BARRIER();
            x->eof = 1;
            break;
        }
        BARRIER();
        x->tail++;
    }
    return NULL;
}

static xfer_t *xfer_new(FILE *f, bool writing) {
    xfer_t *x = calloc(1, sizeof(xfer_t));
    if (!x) return NULL;
    x->f = f;
    x->writing = writing;
    int i;
    for (i = 0; i < XF_BUFS; i++) {
        x->buf[i] = memalign(32, XF_BUFSZ);
        if (!x->buf[i]) goto fail;
    }
    if (LWP_CreateThread(&x->thread, writing ? writer_main : reader_main, x, NULL, XF_STACK, XF_PRIO) != 0) goto fail;
    x->started = true;
    return x;
fail:
    for (i = 0; i < XF_BUFS; i++) free(x->buf[i]);
    free(x);
    return NULL;
}

xfer_t *xfer_open_read(FILE *f) {
    return xfer_new(f, false);
}

xfer_t *xfer_open_write(FILE *f) {
    return xfer_new(f, true);
}

int xfer_send_step(xfer_t *x, s32 sock) {
    int e = x->eof;
    BARRIER();
    if (x->tail == x->head) {
        if (e) return x->err ? -x->err : XF_DONE;
        return XF_BUSY; /* reader still catching up */
    }
    u32 slot = x->tail % XF_BUFS;
    u32 len = x->len[slot];
    int progressed = 0;
    while (x->sendpos < len) {
        s32 r = net_write_chunk(sock, x->buf[slot] + x->sendpos, len - x->sendpos);
        if (r > 0) {
            x->sendpos += r;
            ui_add_bytes(r);
            progressed = 1;
        } else if (r == 0 || r == -EAGAIN) {
            break;
        } else {
            return r;
        }
    }
    if (x->sendpos >= len) {
        x->sendpos = 0;
        BARRIER();
        x->tail++;
    }
    return progressed ? XF_PROGRESS : XF_BUSY;
}

static void commit(xfer_t *x) {
    x->len[x->head % XF_BUFS] = x->fill;
    BARRIER();
    x->head++;
    x->cur = NULL;
    x->fill = 0;
}

int xfer_recv_step(xfer_t *x, s32 sock) {
    if (x->err) return -x->err;
    if (x->eof) {
        if (x->tail == x->head) return x->err ? -x->err : XF_DONE;
        return XF_BUSY; /* writer still draining */
    }
    int progressed = 0;
    int iter;
    for (iter = 0; iter < 8; iter++) {
        if (!x->cur) {
            if (x->head - x->tail >= XF_BUFS) break; /* ring full, writer is the bottleneck */
            x->cur = x->buf[x->head % XF_BUFS];
            x->fill = 0;
        }
        s32 r = net_read_chunk(sock, x->cur + x->fill, XF_BUFSZ - x->fill);
        if (r > 0) {
            x->fill += r;
            ui_add_bytes(r);
            progressed = 1;
            if (x->fill >= XF_BUFSZ) commit(x);
        } else if (r == -EAGAIN) {
            /* keep the disk busy: hand over a partial buffer if the writer is idle */
            if (x->fill > 0 && x->head == x->tail) commit(x);
            break;
        } else if (r == 0) {
            if (x->fill > 0) commit(x);
            BARRIER();
            x->eof = 1; /* writer drains the rest, then we report XF_DONE */
            return XF_PROGRESS;
        } else {
            return r;
        }
    }
    return progressed ? XF_PROGRESS : XF_BUSY;
}

int xfer_close(xfer_t *x) {
    int ret = 0;
    if (!x) return 0;
    if (x->started) {
        if (!x->writing) x->quit = 1; /* reader may be mid-file; writer finishes its queue */
        else if (!x->eof) x->eof = 1;
        LWP_JoinThread(x->thread, NULL);
    }
    if (x->f && fclose(x->f) != 0) ret = -1;
    if (x->err) ret = -1;
    int i;
    for (i = 0; i < XF_BUFS; i++) free(x->buf[i]);
    free(x);
    return ret;
}
