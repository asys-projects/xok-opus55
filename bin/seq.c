#include <stdio.h>
#include <stdlib.h>
int
main (int argc, char **argv)
{
  long a = 1, b;
  if (argc == 2)
    b = atol (argv[1]);
  else if (argc == 3)
    a = atol (argv[1]), b = atol (argv[2]);
  else
    return 1;
  for (long i = a; i <= b; i++)
    printf ("%ld\n", i);
  return 0;
}
