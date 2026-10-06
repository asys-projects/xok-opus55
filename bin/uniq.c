#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main (int argc, char **argv)
{
  int cflag = argc > 1 && !strcmp (argv[1], "-c");
  char *prev = NULL, *line = NULL;
  size_t cap = 0;
  long cnt = 0;
  while (getline (&line, &cap, stdin) > 0)
    {
      if (prev && !strcmp (prev, line))
	{
	  cnt++;
	  continue;
	}
      if (prev)
	{
	  if (cflag)
	    printf ("%7ld ", cnt);
	  fputs (prev, stdout);
	  free (prev);
	}
      prev = strdup (line);
      cnt = 1;
    }
  if (prev)
    {
      if (cflag)
	printf ("%7ld ", cnt);
      fputs (prev, stdout);
    }
  return 0;
}
