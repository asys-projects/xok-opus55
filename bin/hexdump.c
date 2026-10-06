#include <stdio.h>
#include <ctype.h>

int
main (int argc, char **argv)
{
  FILE *f = argc > 1 ? fopen (argv[1], "r") : stdin;
  unsigned char b[16];
  size_t n;
  long off = 0;
  if (f == NULL)
    {
      perror (argv[1]);
      return 1;
    }
  while ((n = fread (b, 1, 16, f)) > 0)
    {
      printf ("%08lx ", off);
      for (size_t i = 0; i < 16; i++)
	if (i < n)
	  printf (" %02x", b[i]);
	else
	  printf ("   ");
      printf ("  |");
      for (size_t i = 0; i < n; i++)
	putchar (isprint (b[i]) ? b[i] : '.');
      printf ("|\n");
      off += n;
    }
  return 0;
}
