/*
 * ExOS libc: buffered standard I/O over file descriptors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define F_READ 1
#define F_WRITE 2
#define F_STATIC 4

static unsigned char stdin_buf[BUFSIZ], stdout_buf[BUFSIZ];
static FILE _stdin = {.fd = 0,.flags = F_READ | F_STATIC,.unget = -1,
  .buf = stdin_buf,.bufsize = BUFSIZ
};
static FILE _stdout = {.fd = 1,.flags = F_WRITE | F_STATIC,.unget = -1,
  .buf = stdout_buf,.bufsize = BUFSIZ,.linebuf = 1
};
static FILE _stderr = {.fd = 2,.flags = F_WRITE | F_STATIC,.unget = -1,
  .buf = NULL,.bufsize = 0
};

FILE *stdin = &_stdin, *stdout = &_stdout, *stderr = &_stderr;

#define MAXFILES 32
static FILE *files[MAXFILES];

static int
parse_mode (const char *mode, int *flags)
{
  int o = 0;
  switch (mode[0])
    {
    case 'r':
      o = O_RDONLY;
      *flags = F_READ;
      break;
    case 'w':
      o = O_WRONLY | O_CREAT | O_TRUNC;
      *flags = F_WRITE;
      break;
    case 'a':
      o = O_WRONLY | O_CREAT | O_APPEND;
      *flags = F_WRITE;
      break;
    default:
      return -1;
    }
  if (strchr (mode, '+'))
    {
      o = (o & ~O_ACCMODE) | O_RDWR;
      *flags = F_READ | F_WRITE;
    }
  return o;
}

FILE *
fdopen (int fd, const char *mode)
{
  int flags;
  FILE *f;

  if (parse_mode (mode, &flags) < 0)
    {
      errno = EINVAL;
      return NULL;
    }
  f = calloc (1, sizeof (*f));
  if (f == NULL)
    return NULL;
  f->buf = malloc (BUFSIZ);
  if (f->buf == NULL)
    {
      free (f);
      return NULL;
    }
  f->bufsize = BUFSIZ;
  f->fd = fd;
  f->flags = flags;
  f->unget = -1;
  for (int i = 0; i < MAXFILES; i++)
    if (files[i] == NULL)
      {
	files[i] = f;
	break;
      }
  return f;
}

FILE *
fopen (const char *path, const char *mode)
{
  int flags, o = parse_mode (mode, &flags), fd;
  FILE *f;

  if (o < 0)
    {
      errno = EINVAL;
      return NULL;
    }
  fd = open (path, o, 0666);
  if (fd < 0)
    return NULL;
  f = fdopen (fd, mode);
  if (f == NULL)
    close (fd);
  return f;
}

int
fflush (FILE *f)
{
  if (f == NULL)
    {
      fflush (stdout);
      fflush (stderr);
      for (int i = 0; i < MAXFILES; i++)
	if (files[i])
	  fflush (files[i]);
      return 0;
    }
  if ((f->flags & F_WRITE) && f->wpos)
    {
      size_t off = 0;
      while (off < f->wpos)
	{
	  ssize_t r = write (f->fd, f->buf + off, f->wpos - off);
	  if (r <= 0)
	    {
	      f->err = 1;
	      f->wpos = 0;
	      return EOF;
	    }
	  off += r;
	}
      f->wpos = 0;
    }
  return 0;
}

int
fclose (FILE *f)
{
  int r = fflush (f);
  if (close (f->fd) < 0)
    r = EOF;
  for (int i = 0; i < MAXFILES; i++)
    if (files[i] == f)
      files[i] = NULL;
  if (!(f->flags & F_STATIC))
    {
      free (f->buf);
      free (f);
    }
  return r;
}

int
fputc (int c, FILE *f)
{
  unsigned char ch = c;

  if (f->bufsize == 0)
    {
      if (write (f->fd, &ch, 1) != 1)
	{
	  f->err = 1;
	  return EOF;
	}
      return ch;
    }
  if (f->wpos >= f->bufsize && fflush (f) == EOF)
    return EOF;
  f->buf[f->wpos++] = ch;
  if ((f->linebuf && ch == '\n') || f->wpos >= f->bufsize)
    if (fflush (f) == EOF)
      return EOF;
  return ch;
}

int
putc (int c, FILE *f)
{
  return fputc (c, f);
}

int
putchar (int c)
{
  return fputc (c, stdout);
}

int
fputs (const char *s, FILE *f)
{
  if (f->bufsize == 0)
    {
      size_t n = strlen (s);
      return write (f->fd, s, n) == (ssize_t) n ? 0 : EOF;
    }
  while (*s)
    if (fputc (*s++, f) == EOF)
      return EOF;
  return 0;
}

int
puts (const char *s)
{
  if (fputs (s, stdout) == EOF)
    return EOF;
  return fputc ('\n', stdout) == EOF ? EOF : 0;
}

size_t
fwrite (const void *p, size_t sz, size_t n, FILE *f)
{
  const unsigned char *c = p;
  size_t total = sz * n;

  if (total == 0)
    return 0;
  if (f->bufsize == 0 || total >= f->bufsize)
    {
      if (fflush (f) == EOF)
	return 0;
      size_t off = 0;
      while (off < total)
	{
	  ssize_t r = write (f->fd, c + off, total - off);
	  if (r <= 0)
	    {
	      f->err = 1;
	      break;
	    }
	  off += r;
	}
      return off / sz;
    }
  for (size_t i = 0; i < total; i++)
    if (fputc (c[i], f) == EOF)
      return i / sz;
  return n;
}

static int
fill (FILE *f)
{
  ssize_t r;

  if (f->flags & F_WRITE)
    fflush (f);
  if (f == stdin)
    fflush (stdout);
  r = read (f->fd, f->buf, f->bufsize);
  if (r <= 0)
    {
      if (r == 0)
	f->eof = 1;
      else
	f->err = 1;
      return EOF;
    }
  f->rpos = 0;
  f->rend = r;
  return 0;
}

int
fgetc (FILE *f)
{
  if (f->unget >= 0)
    {
      int c = f->unget;
      f->unget = -1;
      return c;
    }
  if (f->rpos >= f->rend && fill (f) == EOF)
    return EOF;
  return f->buf[f->rpos++];
}

int
getc (FILE *f)
{
  return fgetc (f);
}

int
getchar (void)
{
  return fgetc (stdin);
}

int
ungetc (int c, FILE *f)
{
  if (c == EOF)
    return EOF;
  f->unget = (unsigned char) c;
  f->eof = 0;
  return c;
}

char *
fgets (char *s, int n, FILE *f)
{
  int i = 0, c;

  if (n <= 0)
    return NULL;
  while (i < n - 1)
    {
      c = fgetc (f);
      if (c == EOF)
	break;
      s[i++] = c;
      if (c == '\n')
	break;
    }
  if (i == 0)
    return NULL;
  s[i] = 0;
  return s;
}

ssize_t
getline (char **line, size_t *n, FILE *f)
{
  size_t i = 0;
  int c;

  if (*line == NULL || *n == 0)
    {
      *n = 128;
      *line = malloc (*n);
      if (*line == NULL)
	return -1;
    }
  while ((c = fgetc (f)) != EOF)
    {
      if (i + 2 >= *n)
	{
	  char *nl = realloc (*line, *n * 2);
	  if (nl == NULL)
	    return -1;
	  *line = nl;
	  *n *= 2;
	}
      (*line)[i++] = c;
      if (c == '\n')
	break;
    }
  if (i == 0)
    return -1;
  (*line)[i] = 0;
  return i;
}

size_t
fread (void *p, size_t sz, size_t n, FILE *f)
{
  unsigned char *d = p;
  size_t total = sz * n, got = 0;

  if (total == 0)
    return 0;
  if (f->unget >= 0)
    {
      d[got++] = f->unget;
      f->unget = -1;
    }
  while (got < total)
    {
      if (f->rpos < f->rend)
	{
	  size_t m = f->rend - f->rpos;
	  if (m > total - got)
	    m = total - got;
	  memcpy (d + got, f->buf + f->rpos, m);
	  f->rpos += m;
	  got += m;
	  continue;
	}
      if (total - got >= f->bufsize)
	{
	  ssize_t r = read (f->fd, d + got, total - got);
	  if (r <= 0)
	    {
	      if (r == 0)
		f->eof = 1;
	      else
		f->err = 1;
	      break;
	    }
	  got += r;
	  continue;
	}
      if (fill (f) == EOF)
	break;
    }
  return got / sz;
}

int
fseek (FILE *f, long off, int whence)
{
  fflush (f);
  if (whence == SEEK_CUR)
    off -= (long) (f->rend - f->rpos);
  f->rpos = f->rend = 0;
  f->unget = -1;
  f->eof = 0;
  return lseek (f->fd, off, whence) < 0 ? -1 : 0;
}

long
ftell (FILE *f)
{
  long pos = lseek (f->fd, 0, SEEK_CUR);
  if (pos < 0)
    return -1;
  return pos - (long) (f->rend - f->rpos) + (long) f->wpos;
}

void
rewind (FILE *f)
{
  fseek (f, 0, SEEK_SET);
  f->err = 0;
}

int
feof (FILE *f)
{
  return f->eof;
}

int
ferror (FILE *f)
{
  return f->err;
}

void
clearerr (FILE *f)
{
  f->eof = f->err = 0;
}

int
fileno (FILE *f)
{
  return f->fd;
}

int
setvbuf (FILE *f, char *buf, int mode, size_t size)
{
  fflush (f);
  if (mode == _IONBF)
    f->bufsize = 0;
  else
    {
      if (f->bufsize == 0)
	{
	  f->buf = malloc (BUFSIZ);
	  f->bufsize = f->buf ? BUFSIZ : 0;
	}
      f->linebuf = mode == _IOLBF;
    }
  return 0;
}

void
setbuf (FILE *f, char *buf)
{
  setvbuf (f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

void
perror (const char *s)
{
  if (s && *s)
    fprintf (stderr, "%s: %s\n", s, strerror (errno));
  else
    fprintf (stderr, "%s\n", strerror (errno));
}
