#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

int
main (int argc, char **argv)
{
  struct stat st;
  char dest[512];
  const char *to;
  int r = 0;

  if (argc < 3)
    {
      fprintf (stderr, "usage: mv from... to\n");
      return 2;
    }
  for (int i = 1; i < argc - 1; i++)
    {
      to = argv[argc - 1];
      if (stat (to, &st) == 0 && S_ISDIR (st.st_mode))
	{
	  const char *b = strrchr (argv[i], '/');
	  snprintf (dest, sizeof (dest), "%s/%s", to, b ? b + 1 : argv[i]);
	  to = dest;
	}
      if (rename (argv[i], to) < 0)
	{
	  fprintf (stderr, "mv: %s: %s\n", argv[i], strerror (errno));
	  r = 1;
	}
    }
  return r;
}
