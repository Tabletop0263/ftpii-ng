# ftpii-ng

A rewrite of [ftpii](https://github.com/joedj/ftpii) (Joseph Jordan, zlib license) for current devkitPPC/libogc.
**Altered source version, not the original ftpii.** Original license notice kept in `LICENSE.txt`.

## What's different

- **Pipelined transfers.** A worker thread does all SD/USB I/O while the main thread does network I/O, through an
  8 x 32 KiB ring. Disk and network latency overlap instead of adding up (the old code did read -> send -> read -> send).
- **No per-chunk fcntl** blocking/non-blocking flips (two IOS calls per 32 KB in the old code).
- **No busy-polling.** Idle loop sleeps; accept/control polling is rate-limited during transfers so it stops competing
  with the SD card and network for IOS IPC bandwidth.
- **Reliability:** transfer stall timeout (30 s), data-connection timeout, idle-client timeout (frees slots held by
  dead Wi-Fi clients), Wi-Fi drop detection + automatic re-init, tolerant of bare-LF clients (old code dropped them).
- **Correct resume:** `REST` + `STOR` no longer truncates the file first.
- **Protocol:** `FEAT, MLSD, MLST, EPSV, EPRT, MDTM, OPTS UTF8, STRU, SITE CHMOD`. PORT/EPRT only to the control peer.
- **3DS-ftpd-style screen:** IP:port, live speed bar, session total, log.
- Dropped: DVD / NTFS / NAND / SEEPROM / OTP / ISFS virtual drives (their libs don't exist in modern libogc).
  Drives: `/sd` and `/usb` (FAT).

## Use

`apps/ftpii-ng/` on the SD card, launch from Homebrew Channel, connect to `ftp://<wii-ip>:21`.
HOME / START exits. B remounts SD/USB (or `SITE REMOUNT`).
Password: rename the folder to `ftpii_YourPassword`, pass it as argv[1] via wiiload, or `SITE PASSWD <pw>`.
No password by default = open to your LAN.

## Build

GitHub Actions builds it on every push (`.github/workflows/build.yml`, artifact `ftpii-ng`).
Locally: `export DEVKITPPC=/opt/devkitpro/devkitPPC && make`.
