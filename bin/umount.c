#include <stdio.h>
#include <exos/exos.h>
#include <exos/fs.h>
#include <exos/fd.h>

int
main (int argc, char **argv)
{
  char abs[256];
  if (argc != 2 || exos_abspath (argv[1], abs, sizeof (abs)) < 0)
    {
      fprintf (stderr, "usage: umount dir\n");
      return 2;
    }
  if (cffs_unmount (abs) < 0)
    {
      fprintf (stderr, "umount: %s: not mounted\n", abs);
      return 1;
    }
  return 0;
}
