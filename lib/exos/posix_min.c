/* Temporary minimal POSIX layer (console only). */
#include <exos/exos.h>
#include <unistd.h>
#include <errno.h>

void exos_proc_init (int is_boot) {}
void exos_fd_init (int is_boot) {}
void exos_sig_deliver (void) {}
void exos_fault_signal (int sig, struct utf *utf)
{
  exos_panic ("signal %d: va %x eip %x err %x", sig, utf->utf_va, utf->utf_eip, utf->utf_err);
}

ssize_t
write (int fd, const void *buf, size_t n)
{
  sys_cputs (buf, n);
  return n;
}

ssize_t
read (int fd, void *buf, size_t n)
{
  int c;
  while ((c = sys_cgetc ()) < 0)
    sys_yield (-1);
  ((char *) buf)[0] = c;
  return 1;
}

int close (int fd) { return 0; }
off_t lseek (int fd, off_t o, int w) { errno = ESPIPE; return -1; }
int open (const char *p, int f, ...) { errno = ENOENT; return -1; }
int raise (int sig) { _exit (128 + sig); }

void
_exit (int status)
{
  sys_env_free (0, 0);
  for (;;)
    ;
}
