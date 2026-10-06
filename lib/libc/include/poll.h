#ifndef _POLL_H
#define _POLL_H
struct pollfd
{
  int fd;
  short events;
  short revents;
};
#define POLLIN 1
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 16
#define POLLNVAL 32
typedef unsigned int nfds_t;
int poll (struct pollfd *fds, nfds_t n, int timeout);
#endif
