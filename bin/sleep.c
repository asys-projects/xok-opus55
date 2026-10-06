#include <stdlib.h>
#include <unistd.h>

int
main (int argc, char **argv)
{
  if (argc > 1)
    {
      double s = 0;
      char *p = argv[1];
      long whole = strtol (p, &p, 10);
      s = whole;
      if (*p == '.')
	{
	  long frac = 0, div = 1;
	  for (p++; *p >= '0' && *p <= '9' && div < 1000000; p++)
	    frac = frac * 10 + (*p - '0'), div *= 10;
	  s += (double) frac / div;
	}
      usleep ((unsigned long) (s * 1000000));
    }
  return 0;
}
