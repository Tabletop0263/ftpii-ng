/* ftpii-ng -- based on ftpii by Joseph Jordan (zlib license, see LICENSE.txt) */
#include <errno.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <string.h>
#include <sys/fcntl.h>
#include <unistd.h>

#include "net.h"
#include "ui.h"

#define IOS_O_NONBLOCK 0x04
#define NET_CHUNK_MAX 32768
#define NET_CHUNK_MIN 4096

static s32 net_chunk = NET_CHUNK_MAX;
static char ip_string[24] = "0.0.0.0";

u64 now_ms(void) {
    return ticks_to_millisecs(gettime());
}

const char *net_ip_string(void) {
    return ip_string;
}

bool net_start(volatile bool *abort_flag) {
    s32 result = -1;
    ui_set_status("Waiting for network...");
    while (!*abort_flag && result < 0) {
        net_deinit();
        while (!*abort_flag && (result = net_init()) == -EAGAIN) usleep(10000);
        if (result < 0) {
            ui_log("net_init failed (%d), retrying", result);
            usleep(500000);
        }
    }
    if (*abort_flag || result < 0) return false;

    u32 ip = 0;
    while (!*abort_flag && !(ip = net_gethostip())) usleep(100000);
    if (!ip) return false;
    struct in_addr a;
    a.s_addr = ip;
    snprintf(ip_string, sizeof(ip_string), "%s", inet_ntoa(a));
    return true;
}

s32 net_set_nonblock(s32 s, bool nonblock) {
    s32 flags = net_fcntl(s, F_GETFL, 0);
    if (flags < 0) return flags;
    return net_fcntl(s, F_SETFL, nonblock ? (flags | IOS_O_NONBLOCK) : (flags & ~IOS_O_NONBLOCK));
}

void net_fast_close(s32 s) {
    if (s >= 0) net_close(s);
}

s32 net_create_server(u16 port) {
    s32 server = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (server < 0) return server;
    net_set_nonblock(server, true);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    s32 ret = net_bind(server, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        net_close(server);
        ui_log("bind failed: %d", ret);
        return ret;
    }
    ret = net_listen(server, 4);
    if (ret < 0) {
        net_close(server);
        ui_log("listen failed: %d", ret);
        return ret;
    }
    return server;
}

s32 net_read_chunk(s32 s, void *buf, s32 len) {
    s32 r;
    if (len > net_chunk) len = net_chunk;
    r = net_read(s, buf, len);
    if (r == -EINVAL && net_chunk > NET_CHUNK_MIN) {
        net_chunk = NET_CHUNK_MIN;
        if (len > net_chunk) len = net_chunk;
        r = net_read(s, buf, len);
    }
    return r;
}

s32 net_write_chunk(s32 s, const void *buf, s32 len) {
    s32 r;
    if (len > net_chunk) len = net_chunk;
    r = net_write(s, buf, len);
    if (r == -EINVAL && net_chunk > NET_CHUNK_MIN) {
        net_chunk = NET_CHUNK_MIN;
        if (len > net_chunk) len = net_chunk;
        r = net_write(s, buf, len);
    }
    return r;
}

s32 net_send_all(s32 s, const void *buf, s32 len, u32 timeout_ms) {
    const char *p = buf;
    u64 last_progress = now_ms();
    while (len > 0) {
        s32 r = net_write_chunk(s, p, len);
        if (r > 0) {
            p += r;
            len -= r;
            last_progress = now_ms();
        } else if (r == 0 || r == -EAGAIN) {
            if (now_ms() - last_progress > timeout_ms) return -ETIMEDOUT;
            usleep(500);
        } else {
            return r;
        }
    }
    return 0;
}
