#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

int
main (int argc, char **argv)
{
  int fds[16], n = 0, app = 0, i = 1;
  if (argc > 1 && !strcmp (argv[1], "-a"))
    app = 1, i++;
  for (; i < argc && n < 16; i++)
    fds[n++] = open (argv[i], O_WRONLY | O_CREAT | (app ? O_APPEND : O_TRUNC), 0644);
  char buf[4096];
  ssize_t r;
  while ((r = read (0, buf, sizeof (buf))) > 0)
    {
      write (1, buf, r);
      for (int k = 0; k < n; k++)
	if (fds[k] >= 0)
	  write (fds[k], buf, r);
    }
  return 0;
}
