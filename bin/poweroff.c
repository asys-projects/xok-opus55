/* poweroff / reboot / halt (root only: needs the root capability). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <exos/exos.h>

int
main (int argc, char **argv)
{
  const char *b = strrchr (argv[0], '/');
  b = b ? b + 1 : argv[0];
  sync ();
  int r = sys_reboot (EXOS_CAP, strcmp (b, "reboot") != 0);
  fprintf (stderr, "%s: permission denied (%d)\n", b, r);
  return 1;
}
