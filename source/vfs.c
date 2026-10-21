/* ftpii-ng -- based on ftpii by Joseph Jordan (zlib license, see LICENSE.txt) */
#include <dirent.h>
#include <errno.h>
#include <fat.h>
#include <ogc/usbstorage.h>
#include <sdcard/wiisd_io.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ui.h"
#include "vfs.h"

typedef struct {
    const char *name;
    const DISC_INTERFACE *disc;
    bool mounted;
} dev_t_;

static dev_t_ devs[] = {
    { "sd", &__io_wiisd, false },
    { "usb", &__io_usbstorage, false },
};
#define NDEVS ((int)(sizeof(devs) / sizeof(devs[0])))

struct vdir {
    DIR *d;
    char real[MAXPATHLEN]; /* real directory path, for per-entry stat */
    int root_idx;          /* >=0: synthetic root listing position, -1: real dir */
};

static void report(void) {
    ui_set_devices(devs[0].mounted, devs[1].mounted);
}

static void mount_dev(int i) {
    if (devs[i].mounted) return;
    if (fatMountSimple(devs[i].name, devs[i].disc)) {
        devs[i].mounted = true;
        ui_log("mounted /%s", devs[i].name);
    }
}

static void unmount_dev(int i) {
    if (!devs[i].mounted) return;
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "%s:", devs[i].name);
    fatUnmount(prefix);
    devs[i].disc->shutdown();
    devs[i].mounted = false;
}

void vfs_init(void) {
    int i;
    for (i = 0; i < NDEVS; i++) mount_dev(i);
    report();
}

void vfs_remount(void) {
    int i;
    for (i = 0; i < NDEVS; i++) unmount_dev(i);
    for (i = 0; i < NDEVS; i++) mount_dev(i);
    report();
}

void vfs_shutdown(void) {
    int i;
    for (i = 0; i < NDEVS; i++) unmount_dev(i);
}

bool vfs_sd_mounted(void) { return devs[0].mounted; }
bool vfs_usb_mounted(void) { return devs[1].mounted; }

int vfs_norm(const char *cwd, const char *arg, char *out) {
    char tmp[MAXPATHLEN * 2];
    if (arg[0] == '/') snprintf(tmp, sizeof(tmp), "%s", arg);
    else snprintf(tmp, sizeof(tmp), "%s/%s", cwd, arg);

    char *comps[64];
    int n = 0;
    char *save = NULL;
    char *tok;
    for (tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (!*tok || !strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) {
            if (n) n--;
            continue;
        }
        if (n >= 64) return -1;
        comps[n++] = tok;
    }
    if (n == 0) {
        strcpy(out, "/");
        return 0;
    }
    size_t len = 0;
    int i;
    for (i = 0; i < n; i++) {
        size_t l = strlen(comps[i]);
        if (len + 1 + l + 1 > MAXPATHLEN) return -1;
        out[len++] = '/';
        memcpy(out + len, comps[i], l);
        len += l;
    }
    out[len] = '\0';
    return 0;
}

/* 0: mapped into real; 1: virtual root; -1: unknown / unmounted device */
static int map(const char *v, char *real) {
    if (!strcmp(v, "/")) return 1;
    const char *p = v + 1;
    const char *slash = strchr(p, '/');
    size_t dl = slash ? (size_t)(slash - p) : strlen(p);
    int i;
    for (i = 0; i < NDEVS; i++) {
        if (strlen(devs[i].name) == dl && !strncmp(p, devs[i].name, dl)) {
            if (!devs[i].mounted) return -1;
            snprintf(real, MAXPATHLEN, "%s:/%s", devs[i].name, slash ? slash + 1 : "");
            return 0;
        }
    }
    return -1;
}

int vfs_stat(const char *vpath, struct stat *st) {
    char real[MAXPATHLEN];
    int m = map(vpath, real);
    if (m < 0) { errno = ENOENT; return -1; }
    if (m == 1) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFDIR | 0755;
        st->st_mtime = time(NULL);
        return 0;
    }
    return stat(real, st);
}

FILE *vfs_fopen(const char *vpath, const char *mode) {
    char real[MAXPATHLEN];
    if (map(vpath, real) != 0) { errno = ENOENT; return NULL; }
    return fopen(real, mode);
}

int vfs_unlink(const char *vpath) {
    char real[MAXPATHLEN];
    if (map(vpath, real) != 0) { errno = EPERM; return -1; }
    return unlink(real);
}

int vfs_rmdir(const char *vpath) {
    char real[MAXPATHLEN];
    if (map(vpath, real) != 0) { errno = EPERM; return -1; }
    return rmdir(real);
}

int vfs_mkdir(const char *vpath) {
    char real[MAXPATHLEN];
    if (map(vpath, real) != 0) { errno = EPERM; return -1; }
    return mkdir(real, 0777);
}

int vfs_rename(const char *from, const char *to) {
    char a[MAXPATHLEN], b[MAXPATHLEN];
    if (map(from, a) != 0 || map(to, b) != 0) { errno = EPERM; return -1; }
    return rename(a, b);
}

vdir_t *vfs_opendir(const char *vpath) {
    char real[MAXPATHLEN];
    int m = map(vpath, real);
    if (m < 0) { errno = ENOENT; return NULL; }
    vdir_t *d = calloc(1, sizeof(vdir_t));
    if (!d) { errno = ENOMEM; return NULL; }
    if (m == 1) {
        d->root_idx = 0;
        return d;
    }
    d->root_idx = -1;
    d->d = opendir(real);
    snprintf(d->real, sizeof(d->real), "%s", real);
    if (!d->d) {
        free(d);
        return NULL;
    }
    return d;
}

int vfs_nextdir(vdir_t *d, char *name, struct stat *st) {
    if (d->root_idx >= 0) {
        while (d->root_idx < NDEVS) {
            int i = d->root_idx++;
            if (!devs[i].mounted) continue;
            strcpy(name, devs[i].name);
            memset(st, 0, sizeof(*st));
            st->st_mode = S_IFDIR | 0755;
            st->st_mtime = time(NULL);
            return 0;
        }
        return -1;
    }
    for (;;) {
        struct dirent *e = readdir(d->d);
        if (!e) return -1;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(name, MAXPATHLEN, "%s", e->d_name);
        char full[MAXPATHLEN * 2];
        size_t rl = strlen(d->real);
        snprintf(full, sizeof(full), "%s%s%s", d->real, (rl && d->real[rl - 1] == '/') ? "" : "/", name);
        if (stat(full, st) != 0) {
            /* unreadable entry: still list it, as an empty file */
            memset(st, 0, sizeof(*st));
            st->st_mode = S_IFREG | 0644;
        }
        return 0;
    }
}

void vfs_closedir(vdir_t *d) {
    if (!d) return;
    if (d->d) closedir(d->d);
    free(d);
}
