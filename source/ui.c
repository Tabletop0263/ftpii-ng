/* ftpii-ng -- based on ftpii by Joseph Jordan (zlib license, see LICENSE.txt) */
#include <ogc/lwp_watchdog.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ui.h"

#define LOG_LINES 12
#define LOG_W 72

static char log_lines[LOG_LINES][LOG_W + 1];
static u32 log_count = 0;
static char ip_str[24] = "--";
static u16 port_no = 21;
static char status[64] = "Starting...";
static int nclients = 0;
static int active_xfers = 0;
static bool sd_ok = false, usb_ok = false;
static char xfer_name[48] = "";
static bool xfer_up = false;
static u64 total_bytes = 0;
static u64 last_bytes = 0;
static u64 last_draw = 0;
static float speed = 0.0f;
static bool dirty = true;

static void fmt_size(char *out, size_t n, double v) {
    if (v >= 1073741824.0) snprintf(out, n, "%.2f GB", v / 1073741824.0);
    else if (v >= 1048576.0) snprintf(out, n, "%.2f MB", v / 1048576.0);
    else if (v >= 1024.0) snprintf(out, n, "%.1f KB", v / 1024.0);
    else snprintf(out, n, "%.0f B", v);
}

void ui_init(void) {
    last_draw = gettime();
}

void ui_log(const char *fmt, ...) {
    char tmp[LOG_W + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    size_t l = strlen(tmp);
    while (l && (tmp[l - 1] == '\n' || tmp[l - 1] == '\r')) tmp[--l] = '\0';
    strcpy(log_lines[log_count % LOG_LINES], tmp);
    log_count++;
    dirty = true;
}

void ui_set_ip(const char *ip, u16 port) {
    snprintf(ip_str, sizeof(ip_str), "%s", ip);
    port_no = port;
    dirty = true;
}

void ui_set_status(const char *s) {
    snprintf(status, sizeof(status), "%s", s);
    dirty = true;
}

void ui_set_clients(int n) {
    if (n != nclients) dirty = true;
    nclients = n;
}

void ui_set_devices(bool sd, bool usb) {
    sd_ok = sd;
    usb_ok = usb;
    dirty = true;
}

void ui_xfer_begin(const char *name, bool upload) {
    snprintf(xfer_name, sizeof(xfer_name), "%s", name);
    xfer_up = upload;
    active_xfers++;
    dirty = true;
}

void ui_xfer_end(void) {
    if (active_xfers > 0) active_xfers--;
    dirty = true;
}

void ui_add_bytes(u32 n) {
    total_bytes += n;
}

void ui_tick(void) {
    u64 now = gettime();
    u32 ms = ticks_to_millisecs(now - last_draw);
    if (ms < 500) return;
    if (!dirty && active_xfers == 0) return;

    double inst = (double)(total_bytes - last_bytes) * 1000.0 / (double)(ms ? ms : 1);
    speed = speed * 0.4f + (float)inst * 0.6f;
    if (active_xfers == 0) speed = 0.0f;
    last_bytes = total_bytes;
    last_draw = now;
    dirty = false;

    char sp[24], tot[24];
    fmt_size(sp, sizeof(sp), speed);
    fmt_size(tot, sizeof(tot), (double)total_bytes);

    printf("\x1b[2J\x1b[1;1H");
    printf("\x1b[36;1mftpii-ng 1.0\x1b[37;0m  fast FTP for Wii\n");
    printf("ftp://%s:%u\n", ip_str, port_no);
    printf("SD: %s   USB: %s   Clients: %d\n", sd_ok ? "ok" : "--", usb_ok ? "ok" : "--", nclients);
    printf("%s\n\n", status);

    if (active_xfers > 0) {
        printf("%s %s\n", xfer_up ? "UP  " : "DOWN", xfer_name);
        int fill = (int)(speed / (6.0f * 1048576.0f) * 30.0f);
        if (fill > 30) fill = 30;
        char bar[32];
        int i;
        for (i = 0; i < 30; i++) bar[i] = i < fill ? '#' : '-';
        bar[30] = '\0';
        printf("[%s] %s/s\n", bar, sp);
    } else {
        printf("idle\n\n");
    }
    printf("session total: %s\n\n", tot);

    u32 shown = log_count < LOG_LINES ? log_count : LOG_LINES;
    u32 i;
    for (i = 0; i < shown; i++) {
        u32 idx = (log_count - shown + i) % LOG_LINES;
        printf("%s\n", log_lines[idx]);
    }
    printf("\x1b[24;1HHOME/START: exit    B: remount SD/USB");
}
