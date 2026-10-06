#include <stdio.h>
#include <time.h>

int
main (void)
{
  time_t t = time (NULL);
  char buf[64];
  strftime (buf, sizeof (buf), "%a %b %d %H:%M:%S %Z %Y", gmtime (&t));
  printf ("%s\n", buf);
  return 0;
}
