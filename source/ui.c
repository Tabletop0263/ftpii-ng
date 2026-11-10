/* ftpii-ng -- based on ftpii by Joseph Jordan (zlib license, see LICENSE.txt)
 *
 * Power-efficient GX-based GUI with console text overlay.
 *
 * Design goals:
 *   * Cool-looking: dark themed panels, header bar, progress bar, status colors.
 *   * Power-efficient: only redraws when state changes (dirty flag) or a
 *     transfer is active. Idle frames burn almost no CPU because we sleep on
 *     VIDEO_WaitVSync() between redraws.
 *   * Fast and reliable: no blocking, no select()/fd_set, just VSync pacing
 *     plus dirty-flag gating.
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ui.h"

/* -------------------------------------------------------------------------- */
/* Configuration                                                              */
/* -------------------------------------------------------------------------- */

#define LOG_LINES 10
#define LOG_W 68

#define REDRAW_INTERVAL_MS 250   /* 4 Hz while transferring */

/* -------------------------------------------------------------------------- */
/* Colors (0xRRGGBB)                                                          */
/* -------------------------------------------------------------------------- */

#define C_BG          0x1a1a2e   /* deep navy background            */
#define C_PANEL       0x23233a   /* slightly lighter panel          */
#define C_HEADER      0x0f0f1a   /* header bar                      */
#define C_BAR_BG      0x333344   /* progress bar background         */
#define C_BAR_FILL    0x4da6ff   /* progress bar fill (blue)        */
#define C_BORDER      0x404055   /* panel border                    */
#define C_HIGHLIGHT   0x7b61ff   /* accent purple                   */

/* -------------------------------------------------------------------------- */
/* GX / video state                                                           */
/* -------------------------------------------------------------------------- */

static GXRModeObj *gx_rmode = NULL;
static void *gx_xfb = NULL;
static bool gx_ready = false;

/* -------------------------------------------------------------------------- */
/* UI model                                                                   */
/* -------------------------------------------------------------------------- */

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
static u64 xfer_bytes = 0;
static u64 total_bytes = 0;
static u64 last_bytes = 0;
static u64 last_draw = 0;
static float speed = 0.0f;
static bool dirty = true;

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static void fmt_size(char *out, size_t n, double v) {
    if (v >= 1073741824.0) snprintf(out, n, "%.2f GB", v / 1073741824.0);
    else if (v >= 1048576.0) snprintf(out, n, "%.2f MB", v / 1048576.0);
    else if (v >= 1024.0) snprintf(out, n, "%.1f KB", v / 1024.0);
    else snprintf(out, n, "%.0f B", v);
}

/* Fill a rectangle in the current GX context (ortho 2D, screen pixels). */
static void gx_fill_rect(int x, int y, int w, int h, u32 color) {
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position2s16(x, y);
    GX_Color4u8((color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff, 0xff);
    GX_Position2s16(x + w, y);
    GX_Color4u8((color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff, 0xff);
    GX_Position2s16(x + w, y + h);
    GX_Color4u8((color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff, 0xff);
    GX_Position2s16(x, y + h);
    GX_Color4u8((color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff, 0xff);
    GX_End();
}

/* Draw a 1-pixel outline rectangle. */
static void gx_draw_rect_outline(int x, int y, int w, int h, u32 color) {
    u8 r = (color >> 16) & 0xff, g = (color >> 8) & 0xff, b = color & 0xff;
    GX_Begin(GX_LINES, GX_VTXFMT0, 8);
    GX_Position2s16(x, y);
    GX_Color4u8(r, g, b, 0xff);
    GX_Position2s16(x + w - 1, y);
    GX_Color4u8(r, g, b, 0xff);
    GX_Position2s16(x, y + h - 1);
    GX_Color4u8(r, g, b, 0xff);
    GX_Position2s16(x + w - 1, y + h - 1);
    GX_Color4u8(r, g, b, 0xff);
    GX_Position2s16(x, y);
    GX_Color4u8(r, g, b, 0xff);
    GX_Position2s16(x, y + h - 1);
    GX_Color4u8(r, g, b, 0xff);
    GX_Position2s16(x + w - 1, y);
    GX_Color4u8(r, g, b, 0xff);
    GX_Position2s16(x + w - 1, y + h - 1);
    GX_Color4u8(r, g, b, 0xff);
    GX_End();
}

/* -------------------------------------------------------------------------- */
/* GX init / shutdown                                                         */
/* -------------------------------------------------------------------------- */

static bool gx_init(void) {
    if (gx_ready) return true;

    VIDEO_Init();
    gx_rmode = VIDEO_GetPreferredMode(NULL);
    if (!gx_rmode) return false;

    gx_xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(gx_rmode));
    if (!gx_xfb) return false;

    VIDEO_Configure(gx_rmode);
    VIDEO_SetNextFramebuffer(gx_xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (gx_rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    /* Console on top of the framebuffer (text overlay). */
    console_init(gx_xfb, 20, 20,
                 gx_rmode->fbWidth - 40, gx_rmode->xfbHeight - 40,
                 gx_rmode->fbWidth * VI_DISPLAY_PIX_SZ);

    GX_Init(NULL, 0);
    { GXColor clear = { 0x1a, 0x1a, 0x2e, 0xff }; GX_SetCopyClear(clear, 0x00ffffff); }
    GX_SetDispCopyYScale((f32)gx_rmode->xfbHeight / (f32)gx_rmode->efbHeight);
    GX_SetScissor(0, 0, gx_rmode->fbWidth, gx_rmode->efbHeight);
    GX_SetDispCopySrc(0, 0, gx_rmode->fbWidth, gx_rmode->efbHeight);
    GX_SetDispCopyDst(gx_rmode->fbWidth, gx_rmode->xfbHeight);
    GX_SetCopyFilter(0, 0, 0, 0);
    GX_SetPixelFmt(GX_PF_RGB565_Z16, GX_ZC_LINEAR);

    /* Orthographic 2D projection matching screen pixels. */
    GX_SetViewport(0, 0, gx_rmode->fbWidth, gx_rmode->efbHeight, 0, 1);
    Mtx44 proj;
    guOrtho(proj, 0, gx_rmode->fbWidth, 0, gx_rmode->efbHeight, 0, 1);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_S16, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

    GX_SetZMode(FALSE, GX_ALWAYS, FALSE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
    GX_SetAlphaUpdate(FALSE);
    GX_SetColorUpdate(TRUE);

    gx_ready = true;
    return true;
}

static void gx_shutdown(void) {
    if (!gx_ready) return;
    gx_ready = false;
    gx_xfb = NULL;
}

/* Begin a GX frame: clear screen, set up ortho. */
static void gx_begin_frame(void) {
    GX_SetDrawDone();
    GX_DrawDone();
    GX_SetViewport(0, 0, gx_rmode->fbWidth, gx_rmode->efbHeight, 0, 1);
    GX_InvalidateTexAll();

    /* Clear to background color. */
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    u8 r = (C_BG >> 16) & 0xff, g = (C_BG >> 8) & 0xff, b = C_BG & 0xff;
    GX_Position2s16(0, 0);
    GX_Color4u8(r, g, b, 0xff);
    GX_Position2s16(gx_rmode->fbWidth, 0);
    GX_Color4u8(r, g, b, 0xff);
    GX_Position2s16(gx_rmode->fbWidth, gx_rmode->efbHeight);
    GX_Color4u8(r, g, b, 0xff);
    GX_Position2s16(0, gx_rmode->efbHeight);
    GX_Color4u8(r, g, b, 0xff);
    GX_End();
}

/* End frame: send to XFB and wait for VSync. */
static void gx_end_frame(void) {
    GX_DrawDone();
    GX_CopyDisp(gx_xfb, TRUE);
    GX_DrawDone();
    VIDEO_SetNextFramebuffer(gx_xfb);
    VIDEO_Flush();
    VIDEO_WaitVSync();
}

/* -------------------------------------------------------------------------- */
/* Drawing                                                                    */
/* -------------------------------------------------------------------------- */

static void draw_header(int w) {
    int hh = 56;
    gx_fill_rect(0, 0, w, hh, C_HEADER);
    gx_fill_rect(0, hh, w, 4, C_HIGHLIGHT); /* accent underline */
}

static void draw_panel(int x, int y, int w, int h) {
    gx_fill_rect(x, y, w, h, C_PANEL);
    gx_draw_rect_outline(x, y, w, h, C_BORDER);
}

static void draw_progress_bar(int x, int y, int w, int h, int fill_px) {
    gx_fill_rect(x, y, w, h, C_BAR_BG);
    if (fill_px > 0) {
        gx_fill_rect(x, y, fill_px, h, C_BAR_FILL);
    }
    gx_draw_rect_outline(x, y, w, h, C_BORDER);
}

static void draw_all(void) {
    int w = gx_rmode->fbWidth;
    int h = gx_rmode->efbHeight;

    gx_begin_frame();

    /* Full background. */
    gx_fill_rect(0, 0, w, h, C_BG);

    /* Header bar. */
    draw_header(w);

    /* Main content panel (below header). */
    int panel_x = 40, panel_y = 70, panel_w = w - 80, panel_h = h - 70 - 40;
    draw_panel(panel_x, panel_y, panel_w, panel_h);

    /* Progress bar (if transferring). */
    if (active_xfers > 0) {
        int bar_w = panel_w - 40;
        int bar_h = 20;
        int bar_x = panel_x + 20;
        int bar_y = panel_y + panel_h - bar_h - 20;
        int fill = 0;
        if (speed > 0.0f) {
            fill = (int)((speed / (6.0f * 1048576.0f)) * (float)bar_w);
            if (fill < 0) fill = 0;
            if (fill > bar_w) fill = bar_w;
        }
        draw_progress_bar(bar_x, bar_y, bar_w, bar_h, fill);
    }

    gx_end_frame();
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void ui_init(void) {
    gx_init();
    last_draw = gettime();
    dirty = true;
    draw_all();
    consoleClear();
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
    xfer_bytes = 0;
    active_xfers++;
    dirty = true;
}

void ui_xfer_end(void) {
    if (active_xfers > 0) active_xfers--;
    dirty = true;
}

void ui_add_bytes(u32 n) {
    total_bytes += n;
    xfer_bytes += n;
    dirty = true;
}

void ui_tick(void) {
    u64 now = gettime();
    u32 ms = ticks_to_millisecs(now - last_draw);
    bool transferring = active_xfers > 0;

    if (!transferring) {
        /* Idle: only redraw when something changed. */
        if (!dirty) return;
    } else {
        /* Transferring: throttle to 4 Hz, but always redraw if dirty. */
        if (ms < REDRAW_INTERVAL_MS && !dirty) return;
    }

    double inst = (double)(total_bytes - last_bytes) * 1000.0 / (double)(ms ? ms : 1);
    speed = speed * 0.4f + (float)inst * 0.6f;
    if (!transferring) speed = 0.0f;
    last_bytes = total_bytes;
    last_draw = now;
    dirty = false;

    /* --- GX geometry pass --- */
    draw_all();

    /* --- Console text overlay pass --- */
    consoleClear();

    /* Header text. */
    printf("\x1b[37;1mftpii-ng\x1b[0m  fast, cool FTP for Wii\n");
    printf("\x1b[36mftp://%s:%u\x1b[0m\n", ip_str, port_no);

    /* Devices + clients. */
    printf("SD: %s   USB: %s   Clients: %d\n",
           sd_ok ? "\x1b[32mok\x1b[0m" : "\x1b[33m--\x1b[0m",
           usb_ok ? "\x1b[32mok\x1b[0m" : "\x1b[33m--\x1b[0m",
           nclients);

    /* Status line with color. */
    const char *status_color = "\x1b[37m";
    if (strstr(status, "error") || strstr(status, "Error")) status_color = "\x1b[31m";
    else if (strstr(status, "Ready")) status_color = "\x1b[32m";
    printf("Status: %s%s\x1b[0m\n", status_color, status);

    /* Transfer row. */
    if (transferring) {
        printf("\n");
        printf("Transfer: %s %s\n",
               xfer_up ? "\x1b[35mUP  \x1b[0m" : "\x1b[36mDOWN\x1b[0m", xfer_name);

        char sp[24], xb[24];
        fmt_size(sp, sizeof(sp), speed);
        fmt_size(xb, sizeof(xb), (double)xfer_bytes);
        printf("Speed: %s/s   Transferred: %s\n", sp, xb);
    } else {
        printf("\nidle\n\n");
    }

    /* Session total. */
    char tot[24];
    fmt_size(tot, sizeof(tot), (double)total_bytes);
    printf("Session total: %s\n\n", tot);

    /* Log. */
    u32 shown = log_count < LOG_LINES ? log_count : LOG_LINES;
    u32 i;
    for (i = 0; i < shown; i++) {
        u32 idx = (log_count - shown + i) % LOG_LINES;
        printf("%s\n", log_lines[idx]);
    }

    /* Footer hint. */
    printf("\x1b[30;1mHOME/START: exit    B: remount SD/USB\x1b[0m\n");
}
