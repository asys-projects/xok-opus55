#ifndef _NETDB_H
#define _NETDB_H
#include <netinet/in.h>
struct hostent
{
  char *h_name;
  char **h_aliases;
  int h_addrtype;
  int h_length;
  char **h_addr_list;
};
#define h_addr h_addr_list[0]
struct hostent *gethostbyname (const char *name);
extern int h_errno;
#endif
