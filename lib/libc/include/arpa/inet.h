#ifndef _ARPA_INET_H
#define _ARPA_INET_H
#include <netinet/in.h>
in_addr_t inet_addr (const char *s);
int inet_aton (const char *s, struct in_addr *a);
char *inet_ntoa (struct in_addr a);
const char *inet_ntop (int af, const void *src, char *dst, socklen_t size);
int inet_pton (int af, const char *src, void *dst);
#endif
