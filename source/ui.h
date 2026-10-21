/* ftpii-ng -- based on ftpii by Joseph Jordan (zlib license, see LICENSE.txt) */
#ifndef UI_H
#define UI_H

#include <gccore.h>

void ui_init(void);
void ui_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_set_ip(const char *ip, u16 port);
void ui_set_status(const char *s);
void ui_set_clients(int n);
void ui_set_devices(bool sd, bool usb);
void ui_xfer_begin(const char *name, bool upload);
void ui_xfer_end(void);
void ui_add_bytes(u32 n);
void ui_tick(void);

#endif
