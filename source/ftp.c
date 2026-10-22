/* ftpii-ng -- based on ftpii by Joseph Jordan (zlib license, see LICENSE.txt) */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "ftp.h"
#include "net.h"
#include "ui.h"
#include "vfs.h"
#include "xfer.h"

#define MAX_CLIENTS 5
#define CMD_BUF 1024
#define DATA_CONNECT_TIMEOUT_MS 30000
#define DATA_STALL_TIMEOUT_MS 30000
#define IDLE_TIMEOUT_MS (20 * 60 * 1000)
#define CTL_POLL_BUSY_MS 20
#define ACCEPT_POLL_MS 100
#define LINK_CHECK_MS 5000

typedef enum { DK_NONE, DK_SEND_FILE, DK_RECV_FILE, DK_SEND_BUF } dkind_t;

typedef struct {
    s32 sock;
    struct sockaddr_in peer;
    bool user_ok, authed, quit;
    char type;
    char cwd[MAXPATHLEN];
    char rnfr[MAXPATHLEN];
    bool have_rnfr;
    u64 rest;
    s32 pasv_sock;
    struct sockaddr_in port_addr;
    bool have_port;
    char buf[CMD_BUF];
    s32 off;
    u64 last_cmd_ms;

    dkind_t dk;
    s32 dsock;
    bool dconnected;
    u64 dstart_ms, dlast_ms;
    xfer_t *xf;
    char *mem;
    u32 mem_len, mem_pos;
    bool xfer_counted;
} client_t;

/* ms elapsed since t; never underflows if t is slightly in the future */
static u64 since(u64 t) {
    u64 n = now_ms();
    return n > t ? n - t : 0;
}

static client_t *clients[MAX_CLIENTS];
static char *password = NULL;
static u16 pasv_port_next = 40000;

/* ---------- small helpers ---------- */

void ftp_set_password(const char *pw) {
    free(password);
    password = (pw && *pw) ? strdup(pw) : NULL;
}

static int client_index(client_t *c) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) if (clients[i] == c) return i;
    return -1;
}

static int reply(client_t *c, int code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int reply(client_t *c, int code, const char *fmt, ...) {
    char b[512];
    int n = snprintf(b, sizeof(b), "%d ", code);
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(b + n, sizeof(b) - n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    if (m > (int)(sizeof(b) - n - 3)) m = (int)(sizeof(b) - n - 3);
    n += m;
    b[n++] = '\r';
    b[n++] = '\n';
    s32 r = net_send_all(c->sock, b, n, 5000);
    if (r < 0) c->quit = true;
    return r;
}

static int reply_raw(client_t *c, const char *s) {
    s32 r = net_send_all(c->sock, s, (s32)strlen(s), 5000);
    if (r < 0) c->quit = true;
    return r;
}

/* ---------- growable string buffer for listings ---------- */

typedef struct {
    char *p;
    u32 len, cap;
} sb_t;

static void sb_printf(sb_t *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(sb_t *sb, const char *fmt, ...) {
    char tmp[MAXPATHLEN + 160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(tmp)) n = (int)sizeof(tmp) - 1;
    if (sb->len + n + 1 > sb->cap) {
        u32 ncap = sb->cap ? sb->cap * 2 : 4096;
        while (ncap < sb->len + n + 1) ncap *= 2;
        char *np = realloc(sb->p, ncap);
        if (!np) return;
        sb->p = np;
        sb->cap = ncap;
    }
    memcpy(sb->p + sb->len, tmp, n);
    sb->len += n;
    sb->p[sb->len] = '\0';
}

static const char *MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static void unix_line(sb_t *sb, const char *name, const struct stat *st, const struct tm *now) {
    struct tm tm;
    time_t t = st->st_mtime;
    gmtime_r(&t, &tm);
    char date[20];
    if (tm.tm_year == now->tm_year)
        snprintf(date, sizeof(date), "%s %2d %02d:%02d", MONTHS[tm.tm_mon % 12], tm.tm_mday, tm.tm_hour, tm.tm_min);
    else
        snprintf(date, sizeof(date), "%s %2d  %4d", MONTHS[tm.tm_mon % 12], tm.tm_mday, tm.tm_year + 1900);
    bool dir = S_ISDIR(st->st_mode);
    sb_printf(sb, "%s 1 wii wii %llu %s %s\r\n", dir ? "drwxr-xr-x" : "-rw-r--r--", (unsigned long long)st->st_size, date, name);
}

static void mlsx_fact(sb_t *sb, const char *name, const struct stat *st, bool full_path_name) {
    struct tm tm;
    time_t t = st->st_mtime;
    gmtime_r(&t, &tm);
    bool dir = S_ISDIR(st->st_mode);
    sb_printf(sb, "type=%s;", dir ? "dir" : "file");
    if (!dir) sb_printf(sb, "size=%llu;", (unsigned long long)st->st_size);
    sb_printf(sb, "modify=%04d%02d%02d%02d%02d%02d;perm=%s; %s\r\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, dir ? "flcdmpe" : "radfw", name);
    (void)full_path_name;
}

/* ---------- data connection machinery ---------- */

static void close_pasv(client_t *c) {
    if (c->pasv_sock >= 0) {
        net_close_blocking(c->pasv_sock);
        c->pasv_sock = -1;
    }
}

static void data_cleanup(client_t *c, int *close_result) {
    if (c->xf) {
        int r = xfer_close(c->xf);
        if (close_result) *close_result = r;
        c->xf = NULL;
    }
    free(c->mem);
    c->mem = NULL;
    c->mem_len = c->mem_pos = 0;
    if (c->dsock >= 0) {
        net_close_blocking(c->dsock);
        c->dsock = -1;
    }
    close_pasv(c);
    c->have_port = false;
    c->dk = DK_NONE;
    c->dconnected = false;
    if (c->xfer_counted) {
        ui_xfer_end();
        c->xfer_counted = false;
    }
}

/* Called after the command handler has set dk/xf/mem. Sends 150 and arms timers. */
static int data_begin(client_t *c, const char *label, bool upload) {
    if (reply(c, 150, "Opening data connection.") < 0) return -1;
    c->dconnected = false;
    c->dstart_ms = c->dlast_ms = now_ms();
    c->dsock = -1;
    if (c->dk == DK_SEND_FILE || c->dk == DK_RECV_FILE) {
        ui_xfer_begin(label, upload);
        c->xfer_counted = true;
    }
    return 0;
}

static bool data_precheck(client_t *c) {
    if (c->pasv_sock < 0 && !c->have_port) {
        reply(c, 425, "Use PASV or PORT first.");
        return false;
    }
    return true;
}

static void data_finish(client_t *c, int result) {
    int close_res = 0;
    bool was_file = (c->dk == DK_SEND_FILE || c->dk == DK_RECV_FILE);
    dkind_t was = c->dk;
    u64 moved = c->xf ? xfer_total(c->xf) : 0;
    data_cleanup(c, &close_res);
    if (was_file) ui_log("%s: %llu bytes, %s", was == DK_RECV_FILE ? "upload" : "download", (unsigned long long)moved, result == 0 && close_res == 0 ? "ok" : "FAILED");
    if (result == 0 && was_file && close_res != 0 && was == DK_RECV_FILE) result = -EIO;
    if (result == 0) reply(c, 226, "Transfer complete.");
    else if (result == -ETIMEDOUT) reply(c, 426, "Transfer timed out.");
    else reply(c, 451, "Transfer failed: %s", strerror(result < 0 ? -result : EIO));
}

/* returns true if work happened */
static bool data_poll(client_t *c) {
    u64 now = now_ms();
    if (!c->dconnected) {
        if (c->pasv_sock >= 0) {
            struct sockaddr_in a;
            socklen_t al = sizeof(a);
            s32 s = net_accept(c->pasv_sock, (struct sockaddr *)&a, &al);
            if (s >= 0) {
                c->dsock = s;
                c->dconnected = true;
                close_pasv(c);
            } else if (s != -EAGAIN) {
                data_finish(c, s);
                return true;
            }
        } else {
            if (c->dsock < 0) {
                c->dsock = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
                if (c->dsock < 0) {
                    int e = c->dsock;
                    c->dsock = -1;
                    data_finish(c, e);
                    return true;
                }
                net_set_nonblock(c->dsock, true);
            }
            s32 r = net_connect(c->dsock, (struct sockaddr *)&c->port_addr, sizeof(c->port_addr));
            if (r >= 0 || r == -EISCONN) c->dconnected = true;
            else if (r != -EINPROGRESS && r != -EALREADY && r != -EAGAIN) {
                data_finish(c, r);
                return true;
            }
        }
        if (c->dconnected) {
            /* Sending needs a blocking socket (see net_send_all); receiving stays non-blocking. */
            net_set_nonblock(c->dsock, c->dk == DK_RECV_FILE);
            c->dlast_ms = now;
            return true;
        }
        if (since(c->dstart_ms) > DATA_CONNECT_TIMEOUT_MS) {
            ui_log("data connection timed out");
            data_finish(c, -ETIMEDOUT);
            return true;
        }
        return false;
    }

    int r = XF_BUSY;
    switch (c->dk) {
    case DK_SEND_FILE:
        r = xfer_send_step(c->xf, c->dsock);
        break;
    case DK_RECV_FILE:
        r = xfer_recv_step(c->xf, c->dsock);
        break;
    case DK_SEND_BUF:
        if (c->mem_pos >= c->mem_len) {
            r = XF_DONE;
        } else {
            s32 w = net_write_chunk(c->dsock, c->mem + c->mem_pos, (s32)(c->mem_len - c->mem_pos));
            if (w > 0) {
                c->mem_pos += w;
                r = XF_PROGRESS;
            } else if (w == 0 || w == -EAGAIN) {
                r = XF_BUSY;
            } else {
                r = w;
            }
        }
        break;
    default:
        r = XF_DONE;
        break;
    }

    if (r == XF_PROGRESS) {
        c->dlast_ms = now;
        return true;
    }
    if (r == XF_DONE) {
        data_finish(c, 0);
        return true;
    }
    if (r < 0) {
        data_finish(c, r);
        return true;
    }
    if (since(c->dlast_ms) > DATA_STALL_TIMEOUT_MS) {
        ui_log("transfer stalled, aborting");
        data_finish(c, -ETIMEDOUT);
        return true;
    }
    return false;
}

/* ---------- command handlers ---------- */

typedef int (*cmd_fn)(client_t *, char *);

static int path_arg(client_t *c, const char *arg, char *out) {
    return vfs_norm(c->cwd, (arg && *arg) ? arg : ".", out);
}

static int cmd_USER(client_t *c, char *arg) {
    c->user_ok = true;
    c->authed = false;
    if (!password) return reply(c, 331, "Any password will do.");
    return reply(c, 331, "Password required.");
}

static int cmd_PASS(client_t *c, char *arg) {
    if (!c->user_ok) return reply(c, 503, "Login with USER first.");
    if (!password || !strcmp(password, arg)) {
        c->authed = true;
        return reply(c, 230, "Logged in.");
    }
    return reply(c, 530, "Login incorrect.");
}

static int cmd_QUIT(client_t *c, char *arg) {
    reply(c, 221, "Goodbye.");
    c->quit = true;
    return 0;
}

static int cmd_NOOP(client_t *c, char *arg) { return reply(c, 200, "OK."); }
static int cmd_SYST(client_t *c, char *arg) { return reply(c, 215, "UNIX Type: L8"); }

static int cmd_FEAT(client_t *c, char *arg) {
    return reply_raw(c,
        "211-Features:\r\n"
        " UTF8\r\n"
        " MLST type*;size*;modify*;perm*;\r\n"
        " MLSD\r\n"
        " SIZE\r\n"
        " MDTM\r\n"
        " REST STREAM\r\n"
        " EPSV\r\n"
        " EPRT\r\n"
        " PASV\r\n"
        "211 End\r\n");
}

static int cmd_OPTS(client_t *c, char *arg) {
    if (!strncasecmp(arg, "UTF8", 4)) return reply(c, 200, "UTF8 is always on.");
    if (!strncasecmp(arg, "MLST", 4)) return reply(c, 200, "MLST OPTS type;size;modify;perm;");
    return reply(c, 501, "Option not understood.");
}

static int cmd_TYPE(client_t *c, char *arg) {
    char t = (char)toupper((unsigned char)arg[0]);
    if (t == 'A' || t == 'I' || t == 'L') {
        c->type = (t == 'A') ? 'A' : 'I';
        ui_log("TYPE %c", t);
        return reply(c, 200, "Type set.");
    }
    return reply(c, 504, "Type not supported.");
}

static int cmd_MODE(client_t *c, char *arg) {
    if (toupper((unsigned char)arg[0]) == 'S') return reply(c, 200, "Mode stream.");
    return reply(c, 504, "Only stream mode.");
}

static int cmd_STRU(client_t *c, char *arg) {
    if (toupper((unsigned char)arg[0]) == 'F') return reply(c, 200, "Structure file.");
    return reply(c, 504, "Only file structure.");
}

static int cmd_PWD(client_t *c, char *arg) {
    char q[MAXPATHLEN * 2];
    size_t j = 0;
    const char *s;
    for (s = c->cwd; *s && j < sizeof(q) - 2; s++) {
        if (*s == '"') q[j++] = '"';
        q[j++] = *s;
    }
    q[j] = '\0';
    return reply(c, 257, "\"%s\" is the current directory.", q);
}

static int cmd_CWD(client_t *c, char *arg) {
    char v[MAXPATHLEN];
    struct stat st;
    if (path_arg(c, arg, v) < 0) return reply(c, 550, "Path too long.");
    if (vfs_stat(v, &st) < 0 || !S_ISDIR(st.st_mode)) return reply(c, 550, "No such directory.");
    strcpy(c->cwd, v);
    return reply(c, 250, "Directory changed.");
}

static int cmd_CDUP(client_t *c, char *arg) {
    char up[] = "..";
    return cmd_CWD(c, up);
}

static int cmd_SIZE(client_t *c, char *arg) {
    char v[MAXPATHLEN];
    struct stat st;
    if (path_arg(c, arg, v) < 0 || vfs_stat(v, &st) < 0 || S_ISDIR(st.st_mode)) return reply(c, 550, "Not a file.");
    return reply(c, 213, "%llu", (unsigned long long)st.st_size);
}

static int cmd_MDTM(client_t *c, char *arg) {
    char v[MAXPATHLEN];
    struct stat st;
    if (path_arg(c, arg, v) < 0 || vfs_stat(v, &st) < 0) return reply(c, 550, "No such file.");
    struct tm tm;
    time_t t = st.st_mtime;
    gmtime_r(&t, &tm);
    return reply(c, 213, "%04d%02d%02d%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static s32 open_pasv(client_t *c) {
    close_pasv(c);
    c->have_port = false;
    s32 s = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0) return s;
    net_set_nonblock(s, true);
    int tries;
    for (tries = 0; tries < 50; tries++) {
        u16 port = pasv_port_next;
        pasv_port_next = (pasv_port_next >= 40999) ? 40000 : pasv_port_next + 1;
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (net_bind(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
            if (net_listen(s, 1) < 0) break;
            c->pasv_sock = s;
            return port;
        }
    }
    net_close(s);
    return -EADDRINUSE;
}

static int cmd_PASV(client_t *c, char *arg) {
    s32 port = open_pasv(c);
    if (port < 0) return reply(c, 425, "Cannot open passive port.");
    u32 ip = net_gethostip();
    return reply(c, 227, "Entering Passive Mode (%u,%u,%u,%u,%u,%u).", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, (port >> 8) & 255, port & 255);
}

static int cmd_EPSV(client_t *c, char *arg) {
    s32 port = open_pasv(c);
    if (port < 0) return reply(c, 425, "Cannot open passive port.");
    return reply(c, 229, "Entering Extended Passive Mode (|||%d|)", port);
}

static int cmd_PORT(client_t *c, char *arg) {
    unsigned a, b, cc, d, p1, p2;
    if (sscanf(arg, "%u,%u,%u,%u,%u,%u", &a, &b, &cc, &d, &p1, &p2) != 6 || a > 255 || b > 255 || cc > 255 || d > 255 || p1 > 255 || p2 > 255)
        return reply(c, 501, "Bad PORT.");
    close_pasv(c);
    memset(&c->port_addr, 0, sizeof(c->port_addr));
    c->port_addr.sin_family = AF_INET;
    c->port_addr.sin_port = htons((u16)(p1 * 256 + p2));
    c->port_addr.sin_addr.s_addr = htonl((a << 24) | (b << 16) | (cc << 8) | d);
    /* only allow the data connection to go back to the control peer (no bounce attacks) */
    if (c->port_addr.sin_addr.s_addr != c->peer.sin_addr.s_addr) return reply(c, 501, "Address must match control connection.");
    c->have_port = true;
    return reply(c, 200, "PORT ok.");
}

static int cmd_EPRT(client_t *c, char *arg) {
    char delim = arg[0];
    char ip[32];
    unsigned proto, port;
    if (!delim) return reply(c, 501, "Bad EPRT.");
    char fmt[48];
    snprintf(fmt, sizeof(fmt), "%%u%c%%31[^%c]%c%%u", delim, delim, delim);
    if (sscanf(arg + 1, fmt, &proto, ip, &port) != 3 || proto != 1 || port > 65535) return reply(c, 522, "Only IPv4 supported.");
    close_pasv(c);
    memset(&c->port_addr, 0, sizeof(c->port_addr));
    c->port_addr.sin_family = AF_INET;
    c->port_addr.sin_port = htons((u16)port);
    if (!inet_aton(ip, &c->port_addr.sin_addr)) return reply(c, 501, "Bad address.");
    if (c->port_addr.sin_addr.s_addr != c->peer.sin_addr.s_addr) return reply(c, 501, "Address must match control connection.");
    c->have_port = true;
    return reply(c, 200, "EPRT ok.");
}

static int cmd_REST(client_t *c, char *arg) {
    unsigned long long v;
    if (sscanf(arg, "%llu", &v) != 1) return reply(c, 501, "Bad offset.");
    c->rest = v;
    return reply(c, 350, "Restarting at %llu.", v);
}

/* Build listing text for LIST/NLST/MLSD. mode: 'L','N','M' */
static char *build_listing(const char *vpath, char mode, u32 *out_len) {
    sb_t sb = { NULL, 0, 0 };
    struct stat st;
    time_t nowt = time(NULL);
    struct tm now;
    gmtime_r(&nowt, &now);

    if (vfs_stat(vpath, &st) < 0) return NULL;
    char name[MAXPATHLEN];
    if (!S_ISDIR(st.st_mode)) {
        const char *base = strrchr(vpath, '/');
        base = base ? base + 1 : vpath;
        if (mode == 'L') unix_line(&sb, base, &st, &now);
        else if (mode == 'N') sb_printf(&sb, "%s\r\n", base);
        else mlsx_fact(&sb, base, &st, false);
    } else {
        vdir_t *d = vfs_opendir(vpath);
        if (!d) return NULL;
        while (vfs_nextdir(d, name, &st) == 0) {
            if (mode == 'L') unix_line(&sb, name, &st, &now);
            else if (mode == 'N') sb_printf(&sb, "%s\r\n", name);
            else mlsx_fact(&sb, name, &st, false);
        }
        vfs_closedir(d);
    }
    if (!sb.p) sb.p = calloc(1, 1);
    *out_len = sb.len;
    return sb.p;
}

static int do_listing(client_t *c, char *arg, char mode) {
    /* ignore ls-style flags such as "-la" */
    while (*arg == '-') {
        while (*arg && *arg != ' ') arg++;
        while (*arg == ' ') arg++;
    }
    char v[MAXPATHLEN];
    if (path_arg(c, arg, v) < 0) return reply(c, 550, "Path too long.");
    if (!data_precheck(c)) return 0;
    u32 len = 0;
    char *txt = build_listing(v, mode, &len);
    if (!txt) {
        close_pasv(c);
        return reply(c, 550, "Cannot list: %s", strerror(errno ? errno : ENOENT));
    }
    c->mem = txt;
    c->mem_len = len;
    c->mem_pos = 0;
    c->dk = DK_SEND_BUF;
    return data_begin(c, "listing", false);
}

static int cmd_LIST(client_t *c, char *arg) { return do_listing(c, arg, 'L'); }
static int cmd_NLST(client_t *c, char *arg) { return do_listing(c, arg, 'N'); }
static int cmd_MLSD(client_t *c, char *arg) { return do_listing(c, arg, 'M'); }

static int cmd_MLST(client_t *c, char *arg) {
    char v[MAXPATHLEN];
    struct stat st;
    if (path_arg(c, arg, v) < 0 || vfs_stat(v, &st) < 0) return reply(c, 550, "No such file or directory.");
    sb_t sb = { NULL, 0, 0 };
    sb_printf(&sb, "250-Listing %s\r\n ", v);
    mlsx_fact(&sb, v, &st, true);
    sb_printf(&sb, "250 End\r\n");
    int r = sb.p ? reply_raw(c, sb.p) : reply(c, 550, "Out of memory.");
    free(sb.p);
    return r;
}

static void short_name(const char *vpath, char *out, size_t n) {
    const char *b = strrchr(vpath, '/');
    snprintf(out, n, "%.47s", b ? b + 1 : vpath);
}

static int cmd_RETR(client_t *c, char *arg) {
    char v[MAXPATHLEN];
    struct stat st;
    if (path_arg(c, arg, v) < 0) return reply(c, 550, "Path too long.");
    if (!data_precheck(c)) return 0;
    if (vfs_stat(v, &st) < 0 || S_ISDIR(st.st_mode)) {
        close_pasv(c);
        return reply(c, 550, "No such file.");
    }
    FILE *f = vfs_fopen(v, "rb");
    if (!f) {
        close_pasv(c);
        return reply(c, 550, "Cannot open: %s", strerror(errno));
    }
    if (c->rest && fseeko(f, (off_t)c->rest, SEEK_SET) != 0) {
        fclose(f);
        c->rest = 0;
        close_pasv(c);
        return reply(c, 550, "Cannot seek.");
    }
    c->rest = 0;
    c->xf = xfer_open_read(f);
    if (!c->xf) {
        fclose(f);
        close_pasv(c);
        return reply(c, 451, "Out of memory.");
    }
    c->dk = DK_SEND_FILE;
    char nm[48];
    short_name(v, nm, sizeof(nm));
    ui_log("RETR %s", nm);
    return data_begin(c, nm, false);
}

static int store(client_t *c, char *arg, bool append) {
    char v[MAXPATHLEN];
    if (path_arg(c, arg, v) < 0) return reply(c, 550, "Path too long.");
    if (!data_precheck(c)) return 0;
    const char *mode = append ? "ab" : (c->rest ? "r+b" : "wb"); /* resume must not truncate */
    FILE *f = vfs_fopen(v, mode);
    if (!f) {
        close_pasv(c);
        c->rest = 0;
        return reply(c, 550, "Cannot open: %s", strerror(errno));
    }
    if (!append && c->rest && fseeko(f, (off_t)c->rest, SEEK_SET) != 0) {
        fclose(f);
        c->rest = 0;
        close_pasv(c);
        return reply(c, 550, "Cannot seek.");
    }
    c->rest = 0;
    c->xf = xfer_open_write(f);
    if (!c->xf) {
        fclose(f);
        close_pasv(c);
        return reply(c, 451, "Out of memory.");
    }
    c->dk = DK_RECV_FILE;
    char nm[48];
    short_name(v, nm, sizeof(nm));
    ui_log("STOR %s", nm);
    return data_begin(c, nm, true);
}

static int cmd_STOR(client_t *c, char *arg) { return store(c, arg, false); }
static int cmd_APPE(client_t *c, char *arg) { return store(c, arg, true); }

static int cmd_DELE(client_t *c, char *arg) {
    char v[MAXPATHLEN];
    struct stat st;
    if (path_arg(c, arg, v) < 0 || vfs_stat(v, &st) < 0) return reply(c, 550, "No such file.");
    if (S_ISDIR(st.st_mode)) return reply(c, 550, "Is a directory.");
    if (vfs_unlink(v) < 0) return reply(c, 550, "%s", strerror(errno));
    ui_log("DELE %s", v);
    return reply(c, 250, "Deleted.");
}

static int cmd_RMD(client_t *c, char *arg) {
    char v[MAXPATHLEN];
    if (path_arg(c, arg, v) < 0) return reply(c, 550, "Path too long.");
    if (vfs_rmdir(v) < 0) return reply(c, 550, "%s", strerror(errno));
    ui_log("RMD %s", v);
    return reply(c, 250, "Removed.");
}

static int cmd_MKD(client_t *c, char *arg) {
    char v[MAXPATHLEN];
    if (path_arg(c, arg, v) < 0) return reply(c, 550, "Path too long.");
    if (vfs_mkdir(v) < 0) return reply(c, 550, "%s", strerror(errno));
    ui_log("MKD %s", v);
    return reply(c, 257, "\"%s\" created.", v);
}

static int cmd_RNFR(client_t *c, char *arg) {
    char v[MAXPATHLEN];
    struct stat st;
    c->have_rnfr = false;
    if (path_arg(c, arg, v) < 0 || vfs_stat(v, &st) < 0) return reply(c, 550, "No such file.");
    strcpy(c->rnfr, v);
    c->have_rnfr = true;
    return reply(c, 350, "Ready for RNTO.");
}

static int cmd_RNTO(client_t *c, char *arg) {
    char v[MAXPATHLEN];
    if (!c->have_rnfr) return reply(c, 503, "RNFR first.");
    c->have_rnfr = false;
    if (path_arg(c, arg, v) < 0) return reply(c, 550, "Path too long.");
    if (vfs_rename(c->rnfr, v) < 0) return reply(c, 550, "%s", strerror(errno));
    ui_log("RENAME -> %s", v);
    return reply(c, 250, "Renamed.");
}

static int cmd_ALLO(client_t *c, char *arg) { return reply(c, 202, "Not needed."); }

static int cmd_SITE(client_t *c, char *arg) {
    char *sp = strchr(arg, ' ');
    char *rest = "";
    if (sp) { *sp = '\0'; rest = sp + 1; }
    if (!strcasecmp(arg, "CHMOD")) return reply(c, 200, "FAT has no permissions; ignored.");
    if (!strcasecmp(arg, "PASSWD")) {
        if (!*rest) return reply(c, 501, "Usage: SITE PASSWD <password>");
        ftp_set_password(rest);
        return reply(c, 200, "Password set.");
    }
    if (!strcasecmp(arg, "NOPASSWD")) {
        ftp_set_password(NULL);
        return reply(c, 200, "Password removed.");
    }
    if (!strcasecmp(arg, "REMOUNT")) {
        vfs_remount();
        return reply(c, 200, "Remounted.");
    }
    return reply(c, 504, "Unknown SITE command.");
}

static const struct {
    const char *name;
    cmd_fn fn;
    bool needs_auth;
} CMDS[] = {
    { "USER", cmd_USER, false }, { "PASS", cmd_PASS, false }, { "QUIT", cmd_QUIT, false },
    { "NOOP", cmd_NOOP, false }, { "FEAT", cmd_FEAT, false }, { "OPTS", cmd_OPTS, false },
    { "SYST", cmd_SYST, true },  { "TYPE", cmd_TYPE, true },  { "MODE", cmd_MODE, true },
    { "STRU", cmd_STRU, true },  { "PWD", cmd_PWD, true },    { "XPWD", cmd_PWD, true },
    { "CWD", cmd_CWD, true },    { "XCWD", cmd_CWD, true },   { "CDUP", cmd_CDUP, true },
    { "XCUP", cmd_CDUP, true },  { "SIZE", cmd_SIZE, true },  { "MDTM", cmd_MDTM, true },
    { "PASV", cmd_PASV, true },  { "EPSV", cmd_EPSV, true },  { "PORT", cmd_PORT, true },
    { "EPRT", cmd_EPRT, true },  { "REST", cmd_REST, true },  { "LIST", cmd_LIST, true },
    { "NLST", cmd_NLST, true },  { "MLSD", cmd_MLSD, true },  { "MLST", cmd_MLST, true },
    { "RETR", cmd_RETR, true },  { "STOR", cmd_STOR, true },  { "APPE", cmd_APPE, true },
    { "DELE", cmd_DELE, true },  { "RMD", cmd_RMD, true },    { "XRMD", cmd_RMD, true },
    { "MKD", cmd_MKD, true },    { "XMKD", cmd_MKD, true },   { "RNFR", cmd_RNFR, true },
    { "RNTO", cmd_RNTO, true },  { "ALLO", cmd_ALLO, true },  { "SITE", cmd_SITE, true },
};
#define NCMDS ((int)(sizeof(CMDS) / sizeof(CMDS[0])))

static void handle_command(client_t *c, char *line) {
    char *arg = strchr(line, ' ');
    if (arg) { *arg++ = '\0'; while (*arg == ' ') arg++; }
    else arg = line + strlen(line);

    char cmd[8];
    size_t i;
    for (i = 0; i < sizeof(cmd) - 1 && line[i]; i++) cmd[i] = (char)toupper((unsigned char)line[i]);
    cmd[i] = '\0';
    if (line[i]) { reply(c, 500, "Unknown command."); return; }

    c->last_cmd_ms = now_ms();
    int k;
    for (k = 0; k < NCMDS; k++) {
        if (!strcmp(cmd, CMDS[k].name)) {
            if (CMDS[k].needs_auth && !c->authed) { reply(c, 530, "Please login with USER and PASS."); return; }
            CMDS[k].fn(c, arg);
            return;
        }
    }
    reply(c, 502, "Command not implemented.");
}

/* ---------- client lifecycle ---------- */

static void client_free(client_t *c) {
    data_cleanup(c, NULL);
    net_close_blocking(c->sock);
    int i = client_index(c);
    if (i >= 0) clients[i] = NULL;
    free(c);
}

static void control_poll(client_t *c) {
    if (c->dk != DK_NONE) return;
    if (c->off < CMD_BUF - 1) {
        s32 r = net_read(c->sock, c->buf + c->off, CMD_BUF - 1 - c->off);
        if (r > 0) c->off += r;
        else if (r == 0 || r != -EAGAIN) { c->quit = true; return; }
    }
    while (!c->quit && c->dk == DK_NONE) {
        char *nl = memchr(c->buf, '\n', c->off);
        if (!nl) {
            if (c->off >= CMD_BUF - 1) { reply(c, 500, "Line too long."); c->quit = true; }
            break;
        }
        s32 consumed = (s32)(nl - c->buf) + 1;
        s32 ll = consumed - 1;
        char line[CMD_BUF];
        memcpy(line, c->buf, ll);
        line[ll] = '\0';
        if (ll > 0 && line[ll - 1] == '\r') line[--ll] = '\0';
        memmove(c->buf, c->buf + consumed, c->off - consumed);
        c->off -= consumed;
        if (*line) handle_command(c, line);
    }
}

static bool accept_clients(s32 server) {
    for (;;) {
        struct sockaddr_in a;
        socklen_t al = sizeof(a);
        s32 p = net_accept(server, (struct sockaddr *)&a, &al);
        if (p == -EAGAIN) return true;
        if (p < 0) {
            ui_log("accept error %d", p);
            return false;
        }
        net_set_nonblock(p, true);
#ifdef TCP_NODELAY
        { int one = 1; net_setsockopt(p, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); }
#endif
        int slot;
        for (slot = 0; slot < MAX_CLIENTS && clients[slot]; slot++) ;
        client_t *c = (slot < MAX_CLIENTS) ? calloc(1, sizeof(client_t)) : NULL;
        if (!c) {
            const char *msg = "421 Too many connections.\r\n";
            net_send_all(p, msg, (s32)strlen(msg), 1000);
            net_close_blocking(p);
            continue;
        }
        c->sock = p;
        memcpy(&c->peer, &a, sizeof(a));
        c->type = 'I';
        c->pasv_sock = -1;
        c->dsock = -1;
        c->last_cmd_ms = now_ms();
        strcpy(c->cwd, "/");
        clients[slot] = c;
        ui_log("client %s connected", inet_ntoa(a.sin_addr));
        reply(c, 220, "ftpii-ng ready.");
    }
}

bool ftp_poll(s32 server, bool *network_down) {
    static u64 last_accept = 0, last_ctl = 0, last_link = 0;
    bool work = false;
    u64 now = now_ms();
    int i, active = 0, count = 0;

    for (i = 0; i < MAX_CLIENTS; i++)
        if (clients[i] && clients[i]->dk != DK_NONE) active++;

    if (now - last_accept >= (active ? ACCEPT_POLL_MS : 0)) {
        last_accept = now;
        if (!accept_clients(server)) { *network_down = true; return true; }
    }
    if (now - last_link >= LINK_CHECK_MS) {
        last_link = now;
        if (net_gethostip() == 0) { *network_down = true; return true; }
    }

    bool do_ctl = (!active) || (now - last_ctl >= CTL_POLL_BUSY_MS);
    if (do_ctl) last_ctl = now;

    for (i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = clients[i];
        if (!c) continue;
        if (c->dk != DK_NONE) {
            if (data_poll(c)) work = true;
        } else if (do_ctl) {
            control_poll(c);
            if (since(c->last_cmd_ms) > IDLE_TIMEOUT_MS && !c->quit) {
                reply(c, 421, "Idle timeout.");
                c->quit = true;
            }
        }
        if (c->quit) {
            ui_log("client %d disconnected", i);
            client_free(c);
            work = true;
            continue;
        }
        count++;
    }
    ui_set_clients(count);
    return work;
}

void ftp_shutdown(void) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i]) {
            reply(clients[i], 421, "Server shutting down.");
            client_free(clients[i]);
        }
    }
}
