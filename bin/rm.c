#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

static int rflag, fflag;

static int
rm (const char *path)
{
  struct stat st;
  if (stat (path, &st) < 0)
    {
      if (fflag)
	return 0;
      fprintf (stderr, "rm: %s: %s\n", path, strerror (errno));
      return 1;
    }
  if (S_ISDIR (st.st_mode))
    {
      if (!rflag)
	{
	  fprintf (stderr, "rm: %s: is a directory\n", path);
	  return 1;
	}
      DIR *d = opendir (path);
      struct dirent *de;
      char names[64][128];
      int n;
      do
	{
	  n = 0;
	  rewinddir (d);
	  while ((de = readdir (d)) != NULL && n < 64)
	    strncpy (names[n++], de->d_name, 127);
	  for (int i = 0; i < n; i++)
	    {
	      char full[512];
	      snprintf (full, sizeof (full), "%s/%s", path, names[i]);
	      if (rm (full))
		{
		  closedir (d);
		  return 1;
		}
	    }
	}
      while (n == 64);
      closedir (d);
      if (rmdir (path) < 0)
	{
	  fprintf (stderr, "rm: %s: %s\n", path, strerror (errno));
	  return 1;
	}
      return 0;
    }
  if (unlink (path) < 0)
    {
      fprintf (stderr, "rm: %s: %s\n", path, strerror (errno));
      return 1;
    }
  return 0;
}

int
main (int argc, char **argv)
{
  int c, r = 0;
  while ((c = getopt (argc, argv, "rf")) != -1)
    if (c == 'r')
      rflag = 1;
    else if (c == 'f')
      fflag = 1;
  for (int i = optind; i < argc; i++)
    r |= rm (argv[i]);
  return r;
}
