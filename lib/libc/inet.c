/*
 * ExOS libc: Internet address conversion.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

int
inet_aton (const char *s, struct in_addr *a)
{
  uint32_t v = 0;
  for (int i = 0; i < 4; i++)
    {
      unsigned n = 0, digits = 0;
      while (isdigit ((unsigned char) *s))
	{
	  n = n * 10 + (*s++ - '0');
	  digits++;
	}
      if (!digits || n > 255)
	return 0;
      v = (v << 8) | n;
      if (i < 3 && *s++ != '.')
	return 0;
    }
  if (*s)
    return 0;
  a->s_addr = htonl (v);
  return 1;
}

in_addr_t
inet_addr (const char *s)
{
  struct in_addr a;
  return inet_aton (s, &a) ? a.s_addr : (in_addr_t) - 1;
}

const char *
inet_ntop (int af, const void *src, char *dst, socklen_t size)
{
  const uint8_t *b = src;
  if (af != AF_INET)
    return NULL;
  snprintf (dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
  return dst;
}

int
inet_pton (int af, const char *src, void *dst)
{
  if (af != AF_INET)
    return -1;
  return inet_aton (src, dst);
}

char *
inet_ntoa (struct in_addr a)
{
  static char buf[16];
  return (char *) inet_ntop (AF_INET, &a, buf, sizeof (buf));
}
