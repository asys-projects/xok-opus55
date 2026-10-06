#include <stdio.h>

int
main (int argc, char **argv)
{
  if (argc != 3)
    {
      fprintf (stderr, "usage: cmp file1 file2\n");
      return 2;
    }
  FILE *a = fopen (argv[1], "r"), *b = fopen (argv[2], "r");
  if (!a || !b)
    {
      perror (a ? argv[2] : argv[1]);
      return 2;
    }
  long off = 0;
  for (;;)
    {
      int x = fgetc (a), y = fgetc (b);
      if (x != y)
	{
	  if (x == EOF || y == EOF)
	    printf ("cmp: EOF on %s\n", x == EOF ? argv[1] : argv[2]);
	  else
	    printf ("%s %s differ: byte %ld\n", argv[1], argv[2], off + 1);
	  return 1;
	}
      if (x == EOF)
	return 0;
      off++;
    }
}
