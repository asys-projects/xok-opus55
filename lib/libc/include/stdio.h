#ifndef _STDIO_H
#define _STDIO_H
#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

#define EOF (-1)
#define BUFSIZ 1024
#define FILENAME_MAX 256
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

typedef struct _FILE
{
  int fd;
  int flags;
  int unget;
  unsigned char *buf;
  size_t bufsize;
  size_t rpos, rend;		/* Read buffer window. */
  size_t wpos;			/* Write buffer fill. */
  int linebuf;
  int eof, err;
} FILE;

extern FILE *stdin, *stdout, *stderr;

FILE *fopen (const char *path, const char *mode);
FILE *fdopen (int fd, const char *mode);
int fclose (FILE *f);
int fflush (FILE *f);
size_t fread (void *p, size_t sz, size_t n, FILE *f);
size_t fwrite (const void *p, size_t sz, size_t n, FILE *f);
int fgetc (FILE *f);
int getc (FILE *f);
int getchar (void);
int ungetc (int c, FILE *f);
char *fgets (char *s, int n, FILE *f);
int fputc (int c, FILE *f);
int putc (int c, FILE *f);
int putchar (int c);
int fputs (const char *s, FILE *f);
int puts (const char *s);
int fseek (FILE *f, long off, int whence);
long ftell (FILE *f);
void rewind (FILE *f);
int feof (FILE *f);
int ferror (FILE *f);
void clearerr (FILE *f);
int fileno (FILE *f);
void setbuf (FILE *f, char *buf);
int setvbuf (FILE *f, char *buf, int mode, size_t size);
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

int printf (const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
int fprintf (FILE *f, const char *fmt, ...) __attribute__ ((format (printf, 2, 3)));
int sprintf (char *s, const char *fmt, ...) __attribute__ ((format (printf, 2, 3)));
int snprintf (char *s, size_t n, const char *fmt, ...) __attribute__ ((format (printf, 3, 4)));
int dprintf (int fd, const char *fmt, ...) __attribute__ ((format (printf, 2, 3)));
int vprintf (const char *fmt, va_list ap);
int vfprintf (FILE *f, const char *fmt, va_list ap);
int vsprintf (char *s, const char *fmt, va_list ap);
int vsnprintf (char *s, size_t n, const char *fmt, va_list ap);
int sscanf (const char *s, const char *fmt, ...);
void perror (const char *s);
int remove (const char *path);
int rename (const char *a, const char *b);
ssize_t getline (char **line, size_t *n, FILE *f);
#endif
