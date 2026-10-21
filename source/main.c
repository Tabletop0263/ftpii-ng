/* ftpii-ng -- based on ftpii by Joseph Jordan (zlib license, see LICENSE.txt) */
#include <gccore.h>
#include <libgen.h>
#include <network.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <wiiuse/wpad.h>

#include "ftp.h"
#include "net.h"
#include "ui.h"
#include "vfs.h"

#define FTP_PORT 21
#define APP_DIR_PREFIX "ftpii_"

static volatile bool quit_flag = false;
static volatile bool power_flag = false;

static void on_reset(u32 irq, void *ctx) { quit_flag = true; }
static void on_power(void) { quit_flag = true; power_flag = true; }
static void on_wpad_power(s32 chan) { on_power(); }

static void video_init(void) {
    VIDEO_Init();
    GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
    void *xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    console_init(xfb, 20, 20, rmode->fbWidth - 40, rmode->xfbHeight - 40, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_ClearFrameBuffer(rmode, xfb, COLOR_BLACK);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
}

static void password_from_path(const char *exe) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", exe);
    char *dir = basename(dirname(tmp));
    if (dir && !strncasecmp(dir, APP_DIR_PREFIX, strlen(APP_DIR_PREFIX)))
        ftp_set_password(dir + strlen(APP_DIR_PREFIX));
}

static void poll_input(void) {
    WPAD_ScanPads();
    PAD_ScanPads();
    u32 w = WPAD_ButtonsDown(0);
    u32 p = PAD_ButtonsDown(0);
    if ((w & WPAD_BUTTON_HOME) || (p & PAD_BUTTON_START)) quit_flag = true;
    if ((w & WPAD_BUTTON_B) || (p & PAD_BUTTON_B)) {
        ui_log("remounting drives...");
        vfs_remount();
    }
}

int main(int argc, char **argv) {
    video_init();
    PAD_Init();
    WPAD_Init();
    SYS_SetResetCallback(on_reset);
    SYS_SetPowerCallback(on_power);
    WPAD_SetPowerButtonCallback(on_wpad_power);
    ui_init();

    if (argc > 1) ftp_set_password(argv[1]);
    else if (argc == 1 && argv[0]) password_from_path(argv[0]);

    vfs_init();

    s32 server = -1;
    bool network_down = true;
    while (!quit_flag) {
        if (network_down) {
            if (server >= 0) { net_close(server); server = -1; }
            ftp_shutdown();
            if (!net_start(&quit_flag)) break;
            server = net_create_server(FTP_PORT);
            if (server < 0) { usleep(500000); continue; }
            ui_set_ip(net_ip_string(), FTP_PORT);
            ui_set_status("Ready.");
            ui_log("listening on %s:%d", net_ip_string(), FTP_PORT);
            network_down = false;
        }
        bool work = ftp_poll(server, &network_down);
        poll_input();
        ui_tick();
        if (!work) usleep(2000);
    }

    ftp_shutdown();
    if (server >= 0) net_close(server);
    vfs_shutdown();
    if (power_flag) SYS_ResetSystem(SYS_POWEROFF, 0, 0);
    exit(0);
    return 0;
}
