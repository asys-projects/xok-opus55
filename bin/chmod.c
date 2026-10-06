#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

int
main (int argc, char **argv)
{
  int r = 0;
  if (argc < 3)
    {
      fprintf (stderr, "usage: chmod mode file...\n");
      return 2;
    }
  mode_t m = strtol (argv[1], NULL, 8);
  for (int i = 2; i < argc; i++)
    if (chmod (argv[i], m) < 0)
      {
	fprintf (stderr, "chmod: %s: %s\n", argv[i], strerror (errno));
	r = 1;
      }
  return r;
}
