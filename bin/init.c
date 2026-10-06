/*
 * init: the first ExOS process.  Mounts the root file system (C-FFS on
 * XN), runs /etc/rc, then keeps a shell on the console and periodically
 * flushes the file system's dirty blocks.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <exos/exos.h>
#include <exos/fs.h>

static pid_t
spawn (char *const argv[])
{
  pid_t pid = fork ();
  if (pid == 0)
    {
      execv (argv[0], argv);
      fprintf (stderr, "init: cannot run %s\n", argv[0]);
      _exit (127);
    }
  return pid;
}

int
main (int argc, char **argv)
{
  pid_t shell = -1;
  int ticks = 0, r;

  printf ("init: ExOS starting (env %x)\n", __envid);
  if ((r = cffs_mount ()) < 0)
    {
      printf ("init: no root file system (%d); halting\n", r);
      sys_reboot (EXOS_CAP, 1);
      return 1;
    }
  setenv ("PATH", "/bin", 1);
  setenv ("HOME", "/", 1);
  signal (SIGINT, SIG_IGN);

  if (access ("/etc/rc", R_OK) == 0)
    {
      char *rc[] = { "/bin/sh", "/etc/rc", NULL };
      pid_t p = spawn (rc);
      int st;
      while (p > 0 && waitpid (p, &st, 0) != p)
	;
    }

  for (;;)
    {
      int st;
      pid_t p;

      if (shell < 0)
	{
	  char *sh[] = { "/bin/sh", NULL };
	  shell = spawn (sh);
	}
      while ((p = waitpid (-1, &st, WNOHANG)) > 0)
	if (p == shell)
	  shell = -1;
      usleep (200000);
      if (++ticks % 25 == 0)
	sync ();
    }
}
