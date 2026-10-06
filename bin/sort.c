#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int rflag;

static int
cmp (const void *a, const void *b)
{
  int c = strcmp (*(char *const *) a, *(char *const *) b);
  return rflag ? -c : c;
}

int
main (int argc, char **argv)
{
  int i = 1;
  if (argc > 1 && !strcmp (argv[1], "-r"))
    rflag = 1, i++;
  FILE *f = i < argc ? fopen (argv[i], "r") : stdin;
  if (f == NULL)
    {
      perror (argv[i]);
      return 1;
    }
  char **lines = NULL, *line = NULL;
  size_t n = 0, cap = 0, lcap = 0;
  while (getline (&line, &lcap, f) > 0)
    {
      if (n == cap)
	lines = realloc (lines, (cap = cap ? cap * 2 : 64) * sizeof (char *));
      lines[n++] = strdup (line);
    }
  qsort (lines, n, sizeof (char *), cmp);
  for (size_t k = 0; k < n; k++)
    fputs (lines[k], stdout);
  return 0;
}
