#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

int
main (int argc, char **argv)
{
  int r = 0;
  for (int i = 1; i < argc; i++)
    if (rmdir (argv[i]) < 0)
      {
	fprintf (stderr, "rmdir: %s: %s\n", argv[i], strerror (errno));
	r = 1;
      }
  return r;
}
