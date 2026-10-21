/* ftpii-ng -- based on ftpii by Joseph Jordan (zlib license, see LICENSE.txt) */
#ifndef NET_H
#define NET_H

#include <gccore.h>
#include <network.h>

u64 now_ms(void);

/* Blocks until the network is up (or abort_flag set). Returns true if connected. */
bool net_start(volatile bool *abort_flag);
const char *net_ip_string(void);

s32 net_create_server(u16 port);
s32 net_set_nonblock(s32 s, bool nonblock);
void net_fast_close(s32 s);

/* Chunk-clamped IO that auto-falls back to 4K if the IOS rejects 32K buffers. */
s32 net_read_chunk(s32 s, void *buf, s32 len);
s32 net_write_chunk(s32 s, const void *buf, s32 len);

/* Send everything on a non-blocking socket, giving up after timeout_ms of no progress. */
s32 net_send_all(s32 s, const void *buf, s32 len, u32 timeout_ms);

#endif
