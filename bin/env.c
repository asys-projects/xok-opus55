#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
main (int argc, char **argv)
{
  int i = 1;
  for (; i < argc && strchr (argv[i], '='); i++)
    {
      char *eq = strchr (argv[i], '=');
      *eq = 0;
      setenv (argv[i], eq + 1, 1);
    }
  if (i < argc)
    {
      execvp (argv[i], argv + i);
      perror (argv[i]);
      return 127;
    }
  for (char **e = environ; e && *e; e++)
    printf ("%s\n", *e);
  return 0;
}
