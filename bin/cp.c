#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static int
copy (const char *from, const char *to)
{
  static char buf[8192];
  char dest[512];
  struct stat st;
  int in, out;
  ssize_t n;

  if (stat (to, &st) == 0 && S_ISDIR (st.st_mode))
    {
      const char *b = strrchr (from, '/');
      snprintf (dest, sizeof (dest), "%s/%s", to, b ? b + 1 : from);
      to = dest;
    }
  if ((in = open (from, O_RDONLY)) < 0)
    {
      fprintf (stderr, "cp: %s: %s\n", from, strerror (errno));
      return 1;
    }
  fstat (in, &st);
  if ((out = open (to, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777)) < 0)
    {
      fprintf (stderr, "cp: %s: %s\n", to, strerror (errno));
      close (in);
      return 1;
    }
  while ((n = read (in, buf, sizeof (buf))) > 0)
    if (write (out, buf, n) != n)
      {
	fprintf (stderr, "cp: %s: %s\n", to, strerror (errno));
	close (in);
	close (out);
	return 1;
      }
  close (in);
  close (out);
  return n < 0;
}

int
main (int argc, char **argv)
{
  int r = 0;
  if (argc < 3)
    {
      fprintf (stderr, "usage: cp from... to\n");
      return 2;
    }
  for (int i = 1; i < argc - 1; i++)
    r |= copy (argv[i], argv[argc - 1]);
  return r;
}
