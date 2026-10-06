/*
 * ExOS libc: general utilities.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>

int errno;

unsigned long long
strtoull (const char *s, char **end, int base)
{
  unsigned long long v = 0;
  const char *p = s;
  int neg = 0, any = 0;

  while (isspace ((unsigned char) *p))
    p++;
  if (*p == '+' || *p == '-')
    neg = *p++ == '-';
  if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')
      && isxdigit ((unsigned char) p[2]))
    {
      p += 2;
      base = 16;
    }
  else if (base == 0)
    base = p[0] == '0' ? 8 : 10;
  for (;; p++)
    {
      int d;
      if (isdigit ((unsigned char) *p))
	d = *p - '0';
      else if (isalpha ((unsigned char) *p))
	d = tolower ((unsigned char) *p) - 'a' + 10;
      else
	break;
      if (d >= base)
	break;
      v = v * base + d;
      any = 1;
    }
  if (end)
    *end = (char *) (any ? p : s);
  return neg ? -v : v;
}

long long
strtoll (const char *s, char **end, int base)
{
  return (long long) strtoull (s, end, base);
}

unsigned long
strtoul (const char *s, char **end, int base)
{
  return (unsigned long) strtoull (s, end, base);
}

long
strtol (const char *s, char **end, int base)
{
  return (long) strtoull (s, end, base);
}

int
atoi (const char *s)
{
  return (int) strtol (s, NULL, 10);
}

long
atol (const char *s)
{
  return strtol (s, NULL, 10);
}

int
abs (int x)
{
  return x < 0 ? -x : x;
}

long
labs (long x)
{
  return x < 0 ? -x : x;
}

static void
swap (char *a, char *b, size_t sz)
{
  while (sz--)
    {
      char t = *a;
      *a++ = *b;
      *b++ = t;
    }
}

static void
qsort_r1 (char *base, size_t n, size_t sz,
	  int (*cmp) (const void *, const void *))
{
  while (n > 1)
    {
      if (n < 8)
	{
	  for (size_t i = 1; i < n; i++)
	    for (size_t j = i; j > 0 && cmp (base + (j - 1) * sz,
					     base + j * sz) > 0; j--)
	      swap (base + (j - 1) * sz, base + j * sz, sz);
	  return;
	}
      swap (base, base + (n / 2) * sz, sz);
      size_t last = 0;
      for (size_t i = 1; i < n; i++)
	if (cmp (base + i * sz, base) < 0)
	  swap (base + (++last) * sz, base + i * sz, sz);
      swap (base, base + last * sz, sz);
      if (last < n - last - 1)
	{
	  qsort_r1 (base, last, sz, cmp);
	  base += (last + 1) * sz;
	  n = n - last - 1;
	}
      else
	{
	  qsort_r1 (base + (last + 1) * sz, n - last - 1, sz, cmp);
	  n = last;
	}
    }
}

void
qsort (void *base, size_t n, size_t sz,
       int (*cmp) (const void *, const void *))
{
  qsort_r1 (base, n, sz, cmp);
}

void *
bsearch (const void *key, const void *base, size_t n, size_t sz,
	 int (*cmp) (const void *, const void *))
{
  size_t lo = 0, hi = n;
  while (lo < hi)
    {
      size_t mid = (lo + hi) / 2;
      const void *p = (const char *) base + mid * sz;
      int c = cmp (key, p);
      if (c == 0)
	return (void *) p;
      if (c < 0)
	hi = mid;
      else
	lo = mid + 1;
    }
  return NULL;
}

static unsigned long rand_state = 1;

int
rand (void)
{
  rand_state = rand_state * 1103515245 + 12345;
  return (rand_state >> 1) & RAND_MAX;
}

void
srand (unsigned s)
{
  rand_state = s;
}

/*
 * Environment.
 */
char **environ;
static int environ_owned;

char *
getenv (const char *name)
{
  size_t l = strlen (name);
  if (environ == NULL)
    return NULL;
  for (char **e = environ; *e; e++)
    if (!strncmp (*e, name, l) && (*e)[l] == '=')
      return *e + l + 1;
  return NULL;
}

static int
env_count (void)
{
  int n = 0;
  if (environ)
    while (environ[n])
      n++;
  return n;
}

int
unsetenv (const char *name)
{
  size_t l = strlen (name);
  int n = env_count ();
  for (int i = 0; i < n; i++)
    if (!strncmp (environ[i], name, l) && environ[i][l] == '=')
      {
	memmove (environ + i, environ + i + 1, (n - i) * sizeof (char *));
	n--;
	i--;
      }
  return 0;
}

int
setenv (const char *name, const char *val, int overwrite)
{
  size_t l = strlen (name);
  int n = env_count ();
  char *s;

  if (getenv (name) && !overwrite)
    return 0;
  s = malloc (l + strlen (val) + 2);
  if (s == NULL)
    return -1;
  sprintf (s, "%s=%s", name, val);
  for (int i = 0; i < n; i++)
    if (!strncmp (environ[i], name, l) && environ[i][l] == '=')
      {
	environ[i] = s;
	return 0;
      }
  char **ne = malloc ((n + 2) * sizeof (char *));
  if (ne == NULL)
    return -1;
  if (n)
    memcpy (ne, environ, n * sizeof (char *));
  ne[n] = s;
  ne[n + 1] = NULL;
  if (environ_owned)
    free (environ);
  environ = ne;
  environ_owned = 1;
  return 0;
}

/*
 * Exit handling.
 */
#define NATEXIT 32
static void (*atexit_fns[NATEXIT]) (void);
static int natexit;

int
atexit (void (*f) (void))
{
  if (natexit >= NATEXIT)
    return -1;
  atexit_fns[natexit++] = f;
  return 0;
}

void
exit (int status)
{
  while (natexit > 0)
    atexit_fns[--natexit] ();
  fflush (NULL);
  _exit (status);
}

void
abort (void)
{
  extern int raise (int);
  fflush (NULL);
  raise (6);
  _exit (134);
}

void
__assert_fail (const char *e, const char *f, int l)
{
  fprintf (stderr, "assertion \"%s\" failed: %s:%d\n", e, f, l);
  abort ();
}

/*
 * getopt.
 */
char *optarg;
int optind = 1, opterr = 1, optopt;
static int optpos;

int
getopt (int argc, char *const argv[], const char *opts)
{
  const char *p;
  int c;

  if (optind >= argc || argv[optind] == NULL || argv[optind][0] != '-'
      || argv[optind][1] == 0)
    return -1;
  if (!strcmp (argv[optind], "--"))
    {
      optind++;
      return -1;
    }
  if (optpos == 0)
    optpos = 1;
  c = argv[optind][optpos++];
  p = strchr (opts, c);
  if (p == NULL || c == ':')
    {
      optopt = c;
      if (opterr)
	fprintf (stderr, "%s: illegal option -- %c\n", argv[0], c);
      if (argv[optind][optpos] == 0)
	{
	  optind++;
	  optpos = 0;
	}
      return '?';
    }
  if (p[1] == ':')
    {
      if (argv[optind][optpos])
	optarg = argv[optind] + optpos;
      else if (optind + 1 < argc)
	optarg = argv[++optind];
      else
	{
	  optopt = c;
	  optind++;
	  optpos = 0;
	  if (opterr)
	    fprintf (stderr, "%s: option requires an argument -- %c\n",
		     argv[0], c);
	  return opts[0] == ':' ? ':' : '?';
	}
      optind++;
      optpos = 0;
      return c;
    }
  if (argv[optind][optpos] == 0)
    {
      optind++;
      optpos = 0;
    }
  return c;
}

/*
 * A small sscanf supporting %d %i %u %x %s %c %n and literals.
 */
int
sscanf (const char *s, const char *fmt, ...)
{
  __builtin_va_list ap;
  int n = 0;
  const char *start = s;

  __builtin_va_start (ap, fmt);
  for (; *fmt; fmt++)
    {
      if (isspace ((unsigned char) *fmt))
	{
	  while (isspace ((unsigned char) *s))
	    s++;
	  continue;
	}
      if (*fmt != '%')
	{
	  if (*s != *fmt)
	    break;
	  s++;
	  continue;
	}
      fmt++;
      int width = 0;
      while (isdigit ((unsigned char) *fmt))
	width = width * 10 + (*fmt++ - '0');
      int lng = 0;
      while (*fmt == 'l' || *fmt == 'h')
	lng += *fmt++ == 'l';
      char *end;
      if (*fmt == 'n')
	{
	  *__builtin_va_arg (ap, int *) = s - start;
	  continue;
	}
      if (*fmt != 'c')
	while (isspace ((unsigned char) *s))
	  s++;
      if (*s == 0)
	break;
      switch (*fmt)
	{
	case 'd':
	case 'i':
	case 'u':
	case 'x':
	  {
	    int base = *fmt == 'x' ? 16 : *fmt == 'i' ? 0 : 10;
	    long v = strtol (s, &end, base);
	    if (end == s)
	      goto out;
	    s = end;
	    if (lng)
	      *__builtin_va_arg (ap, long *) = v;
	    else
	      *__builtin_va_arg (ap, int *) = v;
	    n++;
	    break;
	  }
	case 's':
	  {
	    char *d = __builtin_va_arg (ap, char *);
	    int i = 0;
	    while (*s && !isspace ((unsigned char) *s)
		   && (width == 0 || i < width))
	      d[i++] = *s++;
	    d[i] = 0;
	    n++;
	    break;
	  }
	case 'c':
	  *__builtin_va_arg (ap, char *) = *s++;
	  n++;
	  break;
	default:
	  goto out;
	}
    }
out:
  __builtin_va_end (ap);
  return n;
}
