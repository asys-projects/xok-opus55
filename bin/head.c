#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main (int argc, char **argv)
{
  int n = 10, i = 1, ch, lines = 0;
  if (argc > 2 && !strcmp (argv[1], "-n"))
    n = atoi (argv[2]), i = 3;
  else if (argc > 1 && argv[1][0] == '-')
    n = atoi (argv[1] + 1), i = 2;
  FILE *f = i < argc ? fopen (argv[i], "r") : stdin;
  if (f == NULL)
    {
      perror (argv[i]);
      return 1;
    }
  while (lines < n && (ch = fgetc (f)) != EOF)
    {
      putchar (ch);
      if (ch == '\n')
	lines++;
    }
  return 0;
}
