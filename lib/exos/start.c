/*
 * ExOS program startup.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <exos/exos.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

envid_t __envid;
volatile struct Uenv *const __uenv = (volatile struct Uenv *) UAREA;
volatile struct exos_proc *const __proc =
  (volatile struct exos_proc *) (UAREA + UENV_LIBOS_OFF);

extern int main (int argc, char **argv, char **envp);
extern void (*__init_array_start[]) (void);
extern void (*__init_array_end[]) (void);

static char *boot_argv[] = { "init", NULL };
static char *boot_envp[] = { "PATH=/bin", "HOME=/", NULL };

void
exos_panic (const char *fmt, ...)
{
  char buf[256];
  va_list ap;
  int n;

  n = snprintf (buf, sizeof (buf), "exos panic [env %x]: ", __envid);
  va_start (ap, fmt);
  n += vsnprintf (buf + n, sizeof (buf) - n, fmt, ap);
  va_end (ap);
  if (n > (int) sizeof (buf) - 2)
    n = sizeof (buf) - 2;
  buf[n++] = '\n';
  sys_cputs (buf, n);
  _exit (127);
}

void __attribute__ ((noreturn))
__exos_start (uint32_t * sp)
{
  int argc = sp[0];
  char **argv = (char **) sp[1];
  char **envp = (char **) sp[2];
  int is_boot = argv == NULL;

  __envid = sys_getenvid ();
  exos_vm_init ();
  exos_init_upcalls ();
  if (is_boot)
    {
      argc = 1;
      argv = boot_argv;
      envp = boot_envp;
    }
  environ = envp;
  exos_proc_init (is_boot);
  exos_fd_init (is_boot);

  for (void (**f) (void) = __init_array_start; f < __init_array_end; f++)
    (*f) ();

  exit (main (argc, argv, envp));
}
