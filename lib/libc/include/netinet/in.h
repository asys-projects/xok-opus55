#ifndef _NETINET_IN_H
#define _NETINET_IN_H
#include <sys/types.h>
#include <sys/socket.h>
typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;
struct in_addr
{
  in_addr_t s_addr;
};
struct sockaddr_in
{
  sa_family_t sin_family;
  in_port_t sin_port;
  struct in_addr sin_addr;
  uint8_t sin_zero[8];
};
#define INADDR_ANY ((in_addr_t) 0)
#define INADDR_BROADCAST ((in_addr_t) 0xffffffff)
#define INADDR_LOOPBACK ((in_addr_t) 0x7f000001)
#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define INET_ADDRSTRLEN 16
static inline uint16_t htons (uint16_t x) { return (x << 8) | (x >> 8); }
static inline uint16_t ntohs (uint16_t x) { return (x << 8) | (x >> 8); }
static inline uint32_t htonl (uint32_t x) { return __builtin_bswap32 (x); }
static inline uint32_t ntohl (uint32_t x) { return __builtin_bswap32 (x); }
#endif
