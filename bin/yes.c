#include <stdio.h>
int
main (int argc, char **argv)
{
  const char *s = argc > 1 ? argv[1] : "y";
  for (;;)
    if (printf ("%s\n", s) < 0)
      return 1;
}
