#ifndef _STDLIB_H
#define _STDLIB_H
#include <stddef.h>
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff
void *malloc (size_t n);
void *calloc (size_t n, size_t s);
void *realloc (void *p, size_t n);
void free (void *p);
void exit (int status) __attribute__ ((noreturn));
void abort (void) __attribute__ ((noreturn));
int atexit (void (*f) (void));
int atoi (const char *s);
long atol (const char *s);
long strtol (const char *s, char **end, int base);
unsigned long strtoul (const char *s, char **end, int base);
long long strtoll (const char *s, char **end, int base);
unsigned long long strtoull (const char *s, char **end, int base);
int abs (int x);
long labs (long x);
void qsort (void *base, size_t n, size_t sz, int (*cmp) (const void *, const void *));
void *bsearch (const void *key, const void *base, size_t n, size_t sz, int (*cmp) (const void *, const void *));
int rand (void);
void srand (unsigned s);
char *getenv (const char *name);
int setenv (const char *name, const char *val, int overwrite);
int unsetenv (const char *name);
int system (const char *cmd);
#endif
