#include <stdio.h>
#include <string.h>
int
main (int argc, char **argv)
{
  if (argc < 2)
    return 1;
  char *s = argv[1], *e = s + strlen (s);
  while (e > s + 1 && e[-1] == '/')
    *--e = 0;
  char *b = strrchr (s, '/');
  printf ("%s\n", b && b[1] ? b + 1 : s);
  return 0;
}
