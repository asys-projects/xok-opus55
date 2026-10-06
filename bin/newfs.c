/*
 * newfs: create a C-FFS on a disk: XN format, then the library file
 * system installs its own types (templates with their UDFs) and root.
 *   newfs disk
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exos/exos.h>
#include <exos/fs.h>

int
main (int argc, char **argv)
{
  if (argc != 2)
    {
      fprintf (stderr, "usage: newfs disk\n");
      return 2;
    }
  unsigned dev = atoi (argv[1]);
  if (dev >= sysinfo_page->si_ndisk)
    {
      fprintf (stderr, "newfs: no disk %u\n", dev);
      return 1;
    }
  for (int i = 0; i < NMOUNT; i++)
    if (mounttab->m[i].used && mounttab->m[i].dev == dev)
      {
	fprintf (stderr, "newfs: disk %u is mounted\n", dev);
	return 1;
      }
  int r = cffs_mkfs (dev);
  if (r < 0)
    {
      fprintf (stderr, "newfs: failed (%d)\n", r);
      return 1;
    }
  printf ("newfs: disk %u: %u blocks, C-FFS created\n", dev,
	  xn_cat_super (dev)->s_nblocks);
  return 0;
}
