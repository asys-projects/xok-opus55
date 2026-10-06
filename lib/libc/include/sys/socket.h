#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H
#include <sys/types.h>
typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;
struct sockaddr
{
  sa_family_t sa_family;
  char sa_data[14];
};
#define AF_UNSPEC 0
#define AF_INET 2
#define PF_INET AF_INET
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_ERROR 4
#define SO_KEEPALIVE 9
#define SO_RCVBUF 8
#define SO_SNDBUF 7
#define SO_RCVTIMEO 20
#define MSG_PEEK 2
#define MSG_DONTWAIT 0x40
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2
#define SOMAXCONN 16
int socket (int domain, int type, int proto);
int bind (int fd, const struct sockaddr *a, socklen_t len);
int listen (int fd, int backlog);
int accept (int fd, struct sockaddr *a, socklen_t *len);
int connect (int fd, const struct sockaddr *a, socklen_t len);
ssize_t send (int fd, const void *buf, size_t n, int flags);
ssize_t recv (int fd, void *buf, size_t n, int flags);
ssize_t sendto (int fd, const void *buf, size_t n, int flags,
		const struct sockaddr *a, socklen_t len);
ssize_t recvfrom (int fd, void *buf, size_t n, int flags, struct sockaddr *a,
		  socklen_t *len);
int shutdown (int fd, int how);
int setsockopt (int fd, int level, int opt, const void *v, socklen_t len);
int getsockopt (int fd, int level, int opt, void *v, socklen_t *len);
int getsockname (int fd, struct sockaddr *a, socklen_t *len);
int getpeername (int fd, struct sockaddr *a, socklen_t *len);
#endif
