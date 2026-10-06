#include <stdio.h>
#include <string.h>
int
main (int argc, char **argv)
{
  if (argc < 2)
    return 1;
  char *b = strrchr (argv[1], '/');
  if (b == NULL)
    printf (".\n");
  else if (b == argv[1])
    printf ("/\n");
  else
    {
      *b = 0;
      printf ("%s\n", argv[1]);
    }
  return 0;
}
