#include <stdio.h>
#include <string.h>

int
main (int argc, char **argv)
{
  int nl = 1, i = 1;
  if (argc > 1 && !strcmp (argv[1], "-n"))
    {
      nl = 0;
      i++;
    }
  for (; i < argc; i++)
    printf ("%s%s", argv[i], i < argc - 1 ? " " : "");
  if (nl)
    printf ("\n");
  return 0;
}
