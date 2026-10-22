/* ftpii-ng -- based on ftpii by Joseph Jordan (zlib license, see LICENSE.txt) */
#ifndef XFER_H
#define XFER_H

#include <gccore.h>
#include <stdio.h>

/*
 * Pipelined file transfer: a worker thread does all SD/USB I/O while the main
 * thread does all network I/O, joined by a ring of 32 KiB buffers. Disk and
 * network latency overlap instead of adding up.
 */

#define XF_BUSY 0
#define XF_PROGRESS 1
#define XF_DONE 2 /* errors are returned as negative errno values */

typedef struct xfer xfer_t;

xfer_t *xfer_open_read(FILE *f);  /* RETR: file -> network */
xfer_t *xfer_open_write(FILE *f); /* STOR: network -> file */
int xfer_send_step(xfer_t *x, s32 sock);
int xfer_recv_step(xfer_t *x, s32 sock);
u64 xfer_total(xfer_t *x); /* bytes moved over the network so far */
/* Stops the worker, closes the file, frees everything. Returns 0 if the file closed cleanly. */
int xfer_close(xfer_t *x);

#endif
