#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

int
main (int argc, char **argv)
{
  int r = 0, p = 0, i = 1;
  if (argc > 1 && !strcmp (argv[1], "-p"))
    p = 1, i++;
  for (; i < argc; i++)
    {
      if (p)
	{
	  char buf[256];
	  strncpy (buf, argv[i], sizeof (buf) - 1);
	  buf[sizeof (buf) - 1] = 0;
	  for (char *s = buf + 1; *s; s++)
	    if (*s == '/')
	      {
		*s = 0;
		mkdir (buf, 0777);
		*s = '/';
	      }
	  struct stat st;
	  if (stat (buf, &st) == 0)
	    continue;
	}
      if (mkdir (argv[i], 0777) < 0)
	{
	  fprintf (stderr, "mkdir: %s: %s\n", argv[i], strerror (errno));
	  r = 1;
	}
    }
  return r;
}
