#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

int
main (int argc, char **argv)
{
  int sig = SIGTERM, i = 1, r = 0;
  if (argc > 1 && argv[1][0] == '-')
    {
      sig = atoi (argv[1] + 1);
      if (!strcmp (argv[1] + 1, "KILL"))
	sig = SIGKILL;
      else if (!strcmp (argv[1] + 1, "INT"))
	sig = SIGINT;
      else if (!strcmp (argv[1] + 1, "TERM"))
	sig = SIGTERM;
      i++;
    }
  for (; i < argc; i++)
    if (kill (atoi (argv[i]), sig) < 0)
      {
	perror (argv[i]);
	r = 1;
      }
  return r;
}
