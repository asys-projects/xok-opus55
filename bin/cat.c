#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

static int
copy (int fd)
{
  static char buf[4096];
  ssize_t n;
  while ((n = read (fd, buf, sizeof (buf))) > 0)
    if (write (1, buf, n) != n)
      return 1;
  return n < 0;
}

int
main (int argc, char **argv)
{
  int r = 0;
  if (argc == 1)
    return copy (0);
  for (int i = 1; i < argc; i++)
    {
      int fd = strcmp (argv[i], "-") ? open (argv[i], O_RDONLY) : 0;
      if (fd < 0)
	{
	  fprintf (stderr, "cat: %s: %s\n", argv[i], strerror (errno));
	  r = 1;
	  continue;
	}
      r |= copy (fd);
      if (fd)
	close (fd);
    }
  return r;
}
