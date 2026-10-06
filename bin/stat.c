#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

int
main (int argc, char **argv)
{
  int r = 0;
  for (int i = 1; i < argc; i++)
    {
      struct stat st;
      if (stat (argv[i], &st) < 0)
	{
	  fprintf (stderr, "stat: %s: %s\n", argv[i], strerror (errno));
	  r = 1;
	  continue;
	}
      printf ("  File: %s\n  Size: %ld  Blocks: %ld  Mode: %o  Uid: %u  Gid: %u\n"
	      "  Device: %u  Inode: %u (block %u, slot %u)\n", argv[i],
	      (long) st.st_size, (long) st.st_blocks, st.st_mode, st.st_uid,
	      st.st_gid, st.st_dev, st.st_ino, st.st_ino / 16, st.st_ino % 16);
    }
  return r;
}
