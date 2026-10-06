/*
 * mount: list mounted file systems, or mount a disk's C-FFS.
 *   mount [disk dir]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <exos/exos.h>
#include <exos/fs.h>
#include <exos/fd.h>

int
main (int argc, char **argv)
{
  if (argc == 1)
    {
      for (int i = 0; i < NMOUNT; i++)
	if (mounttab->m[i].used)
	  printf ("disk%u on %s (c-ffs, superblock %u)\n", mounttab->m[i].dev,
		  (const char *) mounttab->m[i].path, mounttab->m[i].superblk);
      return 0;
    }
  if (argc != 3)
    {
      fprintf (stderr, "usage: mount [disk dir]\n");
      return 2;
    }
  char abs[256];
  if (exos_abspath (argv[2], abs, sizeof (abs)) < 0)
    return 1;
  int r = cffs_mount_at (atoi (argv[1]), abs);
  if (r < 0)
    {
      fprintf (stderr, "mount: failed (%d)\n", r);
      return 1;
    }
  return 0;
}
