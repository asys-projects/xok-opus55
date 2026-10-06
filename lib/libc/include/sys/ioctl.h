#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H
int ioctl (int fd, unsigned long req, ...);
#define FIONBIO 0x5421
#define FIONREAD 0x541b
#define TIOCGWINSZ 0x5413
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
#endif
