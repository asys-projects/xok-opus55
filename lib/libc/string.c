/*
 * ExOS libc: string functions.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>

void *
memcpy (void *d, const void *s, size_t n)
{
  void *ret = d;
  if ((((uintptr_t) d | (uintptr_t) s | n) & 3) == 0)
    {
      size_t cnt = n / 4;
      asm volatile ("cld; rep movsl":"+D" (d), "+S" (s), "+c" (cnt)
		    ::"memory");
    }
  else
    asm volatile ("cld; rep movsb":"+D" (d), "+S" (s), "+c" (n)::"memory");
  return ret;
}

void *
memmove (void *d, const void *s, size_t n)
{
  unsigned char *dp = d;
  const unsigned char *sp = s;

  if (dp == sp || n == 0)
    return d;
  if (dp < sp || dp >= sp + n)
    return memcpy (d, s, n);
  dp += n;
  sp += n;
  while (n--)
    *--dp = *--sp;
  return d;
}

void *
memset (void *d, int c, size_t n)
{
  void *ret = d;
  if ((((uintptr_t) d | n) & 3) == 0)
    {
      uint32_t v = (uint8_t) c;
      v |= v << 8;
      v |= v << 16;
      n /= 4;
      asm volatile ("cld; rep stosl":"+D" (d), "+c" (n):"a" (v):"memory");
    }
  else
    asm volatile ("cld; rep stosb":"+D" (d), "+c" (n):"a" (c):"memory");
  return ret;
}

int
memcmp (const void *a, const void *b, size_t n)
{
  const unsigned char *x = a, *y = b;
  for (size_t i = 0; i < n; i++)
    if (x[i] != y[i])
      return x[i] - y[i];
  return 0;
}

void *
memchr (const void *s, int c, size_t n)
{
  const unsigned char *p = s;
  for (size_t i = 0; i < n; i++)
    if (p[i] == (unsigned char) c)
      return (void *) (p + i);
  return NULL;
}

size_t
strlen (const char *s)
{
  size_t n = 0;
  while (s[n])
    n++;
  return n;
}

size_t
strnlen (const char *s, size_t m)
{
  size_t n = 0;
  while (n < m && s[n])
    n++;
  return n;
}

char *
strcpy (char *d, const char *s)
{
  char *r = d;
  while ((*d++ = *s++))
    ;
  return r;
}

char *
strncpy (char *d, const char *s, size_t n)
{
  size_t i;
  for (i = 0; i < n && s[i]; i++)
    d[i] = s[i];
  for (; i < n; i++)
    d[i] = 0;
  return d;
}

size_t
strlcpy (char *d, const char *s, size_t n)
{
  size_t l = strlen (s);
  if (n)
    {
      size_t c = l < n - 1 ? l : n - 1;
      memcpy (d, s, c);
      d[c] = 0;
    }
  return l;
}

size_t
strlcat (char *d, const char *s, size_t n)
{
  size_t dl = strnlen (d, n);
  if (dl == n)
    return n + strlen (s);
  return dl + strlcpy (d + dl, s, n - dl);
}

char *
strcat (char *d, const char *s)
{
  strcpy (d + strlen (d), s);
  return d;
}

char *
strncat (char *d, const char *s, size_t n)
{
  char *p = d + strlen (d);
  while (n-- && *s)
    *p++ = *s++;
  *p = 0;
  return d;
}

int
strcmp (const char *a, const char *b)
{
  while (*a && *a == *b)
    a++, b++;
  return (unsigned char) *a - (unsigned char) *b;
}

int
strncmp (const char *a, const char *b, size_t n)
{
  for (; n; n--, a++, b++)
    {
      if (*a != *b)
	return (unsigned char) *a - (unsigned char) *b;
      if (!*a)
	return 0;
    }
  return 0;
}

int
strcasecmp (const char *a, const char *b)
{
  while (*a && tolower (*a) == tolower (*b))
    a++, b++;
  return tolower ((unsigned char) *a) - tolower ((unsigned char) *b);
}

int
strncasecmp (const char *a, const char *b, size_t n)
{
  for (; n; n--, a++, b++)
    {
      int x = tolower ((unsigned char) *a), y = tolower ((unsigned char) *b);
      if (x != y)
	return x - y;
      if (!x)
	return 0;
    }
  return 0;
}

char *
strchr (const char *s, int c)
{
  for (;; s++)
    {
      if (*s == (char) c)
	return (char *) s;
      if (!*s)
	return NULL;
    }
}

char *
strrchr (const char *s, int c)
{
  const char *r = NULL;
  for (;; s++)
    {
      if (*s == (char) c)
	r = s;
      if (!*s)
	return (char *) r;
    }
}

char *
strstr (const char *h, const char *n)
{
  size_t l = strlen (n);
  if (!l)
    return (char *) h;
  for (; *h; h++)
    if (*h == *n && !strncmp (h, n, l))
      return (char *) h;
  return NULL;
}

char *
strdup (const char *s)
{
  size_t l = strlen (s) + 1;
  char *d = malloc (l);
  if (d)
    memcpy (d, s, l);
  return d;
}

char *
strndup (const char *s, size_t n)
{
  size_t l = strnlen (s, n);
  char *d = malloc (l + 1);
  if (d)
    {
      memcpy (d, s, l);
      d[l] = 0;
    }
  return d;
}

size_t
strspn (const char *s, const char *a)
{
  size_t n = 0;
  while (s[n] && strchr (a, s[n]))
    n++;
  return n;
}

size_t
strcspn (const char *s, const char *r)
{
  size_t n = 0;
  while (s[n] && !strchr (r, s[n]))
    n++;
  return n;
}

char *
strpbrk (const char *s, const char *a)
{
  s += strcspn (s, a);
  return *s ? (char *) s : NULL;
}

char *
strtok_r (char *s, const char *d, char **save)
{
  if (s == NULL)
    s = *save;
  if (s == NULL)
    return NULL;
  s += strspn (s, d);
  if (!*s)
    {
      *save = NULL;
      return NULL;
    }
  char *e = s + strcspn (s, d);
  if (*e)
    {
      *e = 0;
      *save = e + 1;
    }
  else
    *save = NULL;
  return s;
}

char *
strtok (char *s, const char *d)
{
  static char *save;
  return strtok_r (s, d, &save);
}

char *
strsep (char **sp, const char *d)
{
  char *s = *sp, *e;
  if (s == NULL)
    return NULL;
  e = s + strcspn (s, d);
  if (*e)
    {
      *e = 0;
      *sp = e + 1;
    }
  else
    *sp = NULL;
  return s;
}

static const char *const errstr[] = {
  [0] = "Success",
  [1] = "Operation not permitted",
  [2] = "No such file or directory",
  [3] = "No such process",
  [4] = "Interrupted system call",
  [5] = "I/O error",
  [6] = "No such device or address",
  [7] = "Argument list too long",
  [8] = "Exec format error",
  [9] = "Bad file descriptor",
  [10] = "No child processes",
  [11] = "Resource temporarily unavailable",
  [12] = "Out of memory",
  [13] = "Permission denied",
  [14] = "Bad address",
  [16] = "Device or resource busy",
  [17] = "File exists",
  [18] = "Cross-device link",
  [19] = "No such device",
  [20] = "Not a directory",
  [21] = "Is a directory",
  [22] = "Invalid argument",
  [23] = "Too many open files in system",
  [24] = "Too many open files",
  [25] = "Not a terminal",
  [27] = "File too large",
  [28] = "No space left on device",
  [29] = "Illegal seek",
  [30] = "Read-only file system",
  [32] = "Broken pipe",
  [34] = "Result out of range",
  [36] = "File name too long",
  [38] = "Function not implemented",
  [39] = "Directory not empty",
  [88] = "Not a socket",
  [98] = "Address in use",
  [104] = "Connection reset",
  [107] = "Not connected",
  [110] = "Timed out",
  [111] = "Connection refused",
};

char *
strerror (int e)
{
  static char buf[32];
  if (e >= 0 && e < (int) (sizeof (errstr) / sizeof (errstr[0]))
      && errstr[e])
    return (char *) errstr[e];
  extern int snprintf (char *, size_t, const char *, ...);
  snprintf (buf, sizeof (buf), "Unknown error %d", e);
  return buf;
}
