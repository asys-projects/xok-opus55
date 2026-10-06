#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/wait.h>
#include <exos/exos.h>

int
main (int argc, char **argv)
{
  printf ("init: ExOS starting, env %x pid %d\n", __envid, getpid ());
  DIR *d = opendir ("/bin");
  if (d == NULL)
    {
      perror ("opendir /bin");
      sys_reboot (0, 1);
    }
  struct dirent *de;
  while ((de = readdir (d)) != NULL)
    printf ("  /bin/%s\n", de->d_name);
  closedir (d);

  int fd = open ("/tmp/test.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  printf ("open -> %d\n", fd);
  printf ("write -> %d\n", (int) write (fd, "hello file\n", 11));
  close (fd);
  char buf[64];
  fd = open ("/tmp/test.txt", O_RDONLY);
  int n = read (fd, buf, sizeof (buf) - 1);
  buf[n > 0 ? n : 0] = 0;
  printf ("read back %d: %s", n, buf);
  close (fd);

  pid_t pid = fork ();
  if (pid == 0)
    {
      printf ("child: pid %d\n", getpid ());
      char *av[] = { "hello", "a", "b", NULL };
      execv ("/bin/hello", av);
      perror ("execv");
      _exit (1);
    }
  int status;
  pid_t w = waitpid (pid, &status, 0);
  printf ("init: child %d exited status %d (w %d)\n", pid, WEXITSTATUS (status), w);
  sync ();
  sys_debug (DBG_PRINT_ENVS, 0);
  sys_reboot (0, 1);
  return 0;
}
