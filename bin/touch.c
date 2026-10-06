#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

extern int utime (const char *path, const void *times);

int
main (int argc, char **argv)
{
  int r = 0;
  for (int i = 1; i < argc; i++)
    {
      if (access (argv[i], F_OK) == 0)
	{
	  utime (argv[i], NULL);
	  continue;
	}
      int fd = open (argv[i], O_WRONLY | O_CREAT, 0666);
      if (fd < 0)
	{
	  fprintf (stderr, "touch: %s: %s\n", argv[i], strerror (errno));
	  r = 1;
	}
      else
	close (fd);
    }
  return r;
}
