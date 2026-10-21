/* ftpii-ng -- based on ftpii by Joseph Jordan (zlib license, see LICENSE.txt) */
#ifndef FTP_H
#define FTP_H

#include <gccore.h>

void ftp_set_password(const char *pw);
/* Returns true if any work happened this pass. Sets *network_down if the link dropped. */
bool ftp_poll(s32 server, bool *network_down);
void ftp_shutdown(void);

#endif
