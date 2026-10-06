/*
 * ExOS libc: formatted output.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

struct out
{
  void (*put) (struct out *, char);
  char *buf;
  size_t size, len;
  FILE *f;
};

static void
put_buf (struct out *o, char c)
{
  if (o->len + 1 < o->size)
    o->buf[o->len] = c;
  o->len++;
}

static void
put_file (struct out *o, char c)
{
  fputc (c, o->f);
  o->len++;
}

static void
emit_str (struct out *o, const char *s, size_t n, int width, int left,
	  char pad)
{
  int fill = width > (int) n ? width - (int) n : 0;
  if (!left)
    while (fill-- > 0)
      o->put (o, pad);
  for (size_t i = 0; i < n; i++)
    o->put (o, s[i]);
  if (left)
    while (fill-- > 0)
      o->put (o, ' ');
}

static int
fmt_core (struct out *o, const char *fmt, va_list ap)
{
  for (; *fmt; fmt++)
    {
      if (*fmt != '%')
	{
	  o->put (o, *fmt);
	  continue;
	}
      fmt++;
      int left = 0, plus = 0, space = 0, alt = 0, zero = 0;
      for (;; fmt++)
	{
	  if (*fmt == '-')
	    left = 1;
	  else if (*fmt == '+')
	    plus = 1;
	  else if (*fmt == ' ')
	    space = 1;
	  else if (*fmt == '#')
	    alt = 1;
	  else if (*fmt == '0')
	    zero = 1;
	  else
	    break;
	}
      int width = 0, prec = -1;
      if (*fmt == '*')
	{
	  width = va_arg (ap, int);
	  if (width < 0)
	    {
	      left = 1;
	      width = -width;
	    }
	  fmt++;
	}
      else
	while (*fmt >= '0' && *fmt <= '9')
	  width = width * 10 + (*fmt++ - '0');
      if (*fmt == '.')
	{
	  fmt++;
	  prec = 0;
	  if (*fmt == '*')
	    {
	      prec = va_arg (ap, int);
	      fmt++;
	    }
	  else
	    while (*fmt >= '0' && *fmt <= '9')
	      prec = prec * 10 + (*fmt++ - '0');
	}
      int lng = 0;
      while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 'j'
	     || *fmt == 't')
	{
	  if (*fmt == 'l')
	    lng++;
	  else if (*fmt == 'j')
	    lng = 2;
	  fmt++;
	}

      char tmp[72];
      const char *s;
      size_t n;
      uint64_t uv = 0;
      int neg = 0, base = 10, upper = 0;

      switch (*fmt)
	{
	case '%':
	  o->put (o, '%');
	  continue;
	case 'c':
	  tmp[0] = (char) va_arg (ap, int);
	  emit_str (o, tmp, 1, width, left, ' ');
	  continue;
	case 's':
	  s = va_arg (ap, const char *);
	  if (s == NULL)
	    s = "(null)";
	  n = prec >= 0 ? strnlen (s, prec) : strlen (s);
	  emit_str (o, s, n, width, left, ' ');
	  continue;
	case 'd':
	case 'i':
	  {
	    int64_t v;
	    if (lng >= 2)
	      v = va_arg (ap, long long);
	    else if (lng == 1)
	      v = va_arg (ap, long);
	    else
	      v = va_arg (ap, int);
	    if (v < 0)
	      {
		neg = 1;
		uv = (uint64_t) (-(v + 1)) + 1;
	      }
	    else
	      uv = v;
	    break;
	  }
	case 'p':
	  uv = (uintptr_t) va_arg (ap, void *);
	  base = 16;
	  alt = 1;
	  break;
	case 'x':
	case 'X':
	case 'o':
	case 'u':
	  if (lng >= 2)
	    uv = va_arg (ap, unsigned long long);
	  else if (lng == 1)
	    uv = va_arg (ap, unsigned long);
	  else
	    uv = va_arg (ap, unsigned int);
	  if (*fmt == 'x')
	    base = 16;
	  else if (*fmt == 'X')
	    base = 16, upper = 1;
	  else if (*fmt == 'o')
	    base = 8;
	  break;
	default:
	  o->put (o, '%');
	  if (*fmt)
	    o->put (o, *fmt);
	  else
	    fmt--;
	  continue;
	}

      /* Integer conversion. */
      char *p = tmp + sizeof (tmp);
      const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
      int nd = 0;
      while (uv)
	{
	  *--p = digits[uv % base];
	  uv /= base;
	  nd++;
	}
      if (prec < 0)
	prec = 1;
      else
	zero = 0;
      while (nd < prec)
	{
	  *--p = '0';
	  nd++;
	}
      char prefix[3];
      int np = 0;
      if (neg)
	prefix[np++] = '-';
      else if (plus && (*fmt == 'd' || *fmt == 'i'))
	prefix[np++] = '+';
      else if (space && (*fmt == 'd' || *fmt == 'i'))
	prefix[np++] = ' ';
      if (alt && base == 16)
	{
	  prefix[np++] = '0';
	  prefix[np++] = upper ? 'X' : 'x';
	}
      else if (alt && base == 8 && *p != '0')
	prefix[np++] = '0';
      int total = np + nd;
      if (zero && !left)
	{
	  for (int i = 0; i < np; i++)
	    o->put (o, prefix[i]);
	  while (total < width)
	    {
	      o->put (o, '0');
	      total++;
	    }
	  for (int i = 0; i < nd; i++)
	    o->put (o, p[i]);
	}
      else
	{
	  if (!left)
	    while (total < width)
	      {
		o->put (o, ' ');
		total++;
	      }
	  for (int i = 0; i < np; i++)
	    o->put (o, prefix[i]);
	  for (int i = 0; i < nd; i++)
	    o->put (o, p[i]);
	  if (left)
	    while (total < width)
	      {
		o->put (o, ' ');
		total++;
	      }
	}
    }
  return o->len;
}

int
vsnprintf (char *s, size_t n, const char *fmt, va_list ap)
{
  struct out o = {.put = put_buf,.buf = s,.size = n,.len = 0 };
  fmt_core (&o, fmt, ap);
  if (n)
    s[o.len < n ? o.len : n - 1] = 0;
  return o.len;
}

int
snprintf (char *s, size_t n, const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  int r = vsnprintf (s, n, fmt, ap);
  va_end (ap);
  return r;
}

int
vsprintf (char *s, const char *fmt, va_list ap)
{
  return vsnprintf (s, (size_t) 1 << 30, fmt, ap);
}

int
sprintf (char *s, const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  int r = vsprintf (s, fmt, ap);
  va_end (ap);
  return r;
}

int
vfprintf (FILE *f, const char *fmt, va_list ap)
{
  struct out o = {.put = put_file,.f = f };
  fmt_core (&o, fmt, ap);
  return o.len;
}

int
fprintf (FILE *f, const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  int r = vfprintf (f, fmt, ap);
  va_end (ap);
  return r;
}

int
vprintf (const char *fmt, va_list ap)
{
  return vfprintf (stdout, fmt, ap);
}

int
printf (const char *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  int r = vfprintf (stdout, fmt, ap);
  va_end (ap);
  return r;
}

int
dprintf (int fd, const char *fmt, ...)
{
  char buf[512];
  va_list ap;
  va_start (ap, fmt);
  int r = vsnprintf (buf, sizeof (buf), fmt, ap);
  va_end (ap);
  write (fd, buf, r < (int) sizeof (buf) ? r : (int) sizeof (buf) - 1);
  return r;
}
