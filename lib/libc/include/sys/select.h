#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H
#include <sys/time.h>
#define FD_SETSIZE 64
typedef struct
{
  uint32_t bits[(FD_SETSIZE + 31) / 32];
} fd_set;
#define FD_ZERO(s) do { for (int __i = 0; __i < (FD_SETSIZE + 31) / 32; __i++) (s)->bits[__i] = 0; } while (0)
#define FD_SET(fd, s) ((s)->bits[(fd) / 32] |= 1u << ((fd) % 32))
#define FD_CLR(fd, s) ((s)->bits[(fd) / 32] &= ~(1u << ((fd) % 32)))
#define FD_ISSET(fd, s) (((s)->bits[(fd) / 32] >> ((fd) % 32)) & 1)
int select (int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv);
#endif
