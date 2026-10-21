/* ftpii-ng -- based on ftpii by Joseph Jordan (zlib license, see LICENSE.txt) */
#ifndef VFS_H
#define VFS_H

#include <gccore.h>
#include <stdio.h>
#include <sys/param.h>
#include <sys/stat.h>

/* Virtual layout:  /  ->  /sd  /usb   (each maps to sd:/  usb:/) */

void vfs_init(void);
void vfs_remount(void);
void vfs_shutdown(void);
bool vfs_sd_mounted(void);
bool vfs_usb_mounted(void);

/* Resolve arg against cwd into a clean absolute virtual path. 0 on success. */
int vfs_norm(const char *cwd, const char *arg, char *out);

int vfs_stat(const char *vpath, struct stat *st);
FILE *vfs_fopen(const char *vpath, const char *mode);
int vfs_unlink(const char *vpath);
int vfs_rmdir(const char *vpath);
int vfs_mkdir(const char *vpath);
int vfs_rename(const char *from, const char *to);

typedef struct vdir vdir_t;
vdir_t *vfs_opendir(const char *vpath);
/* 0 on success, -1 at end. name must hold MAXPATHLEN bytes. */
int vfs_nextdir(vdir_t *d, char *name, struct stat *st);
void vfs_closedir(vdir_t *d);

#endif
