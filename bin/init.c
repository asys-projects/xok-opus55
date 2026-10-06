#include <stdio.h>
#include <exos/exos.h>

int
main (int argc, char **argv)
{
  printf ("init: hello from ExOS, env %x\n", __envid);
  for (int i = 0; i < 3; i++)
    {
      printf ("init: yield %d\n", i);
      sys_yield (-1);
    }
  volatile unsigned long x = 0;
  for (unsigned long i = 0; i < 300000000UL; i++)
    x += i;
  printf ("busy done: prologues %u epilogues %u excess %u yields %u\n",
	  __uenv->u_prologue_count, __uenv->u_epilogue_count,
	  __uenv->u_excess_count, __uenv->u_yield_count);
  /* Stack growth via fault upcall. */
  volatile char *p = (char *) (USTACKTOP - 3 * PGSIZE);
  *p = 1;
  printf ("stack fault handled: faults %u\n", __uenv->u_fault_count);
  double d = 1.5;
  for (int i = 0; i < 10; i++)
    d = d * 1.5;
  printf ("fpu: %d\n", (int) d);
  sys_reboot (0, 1);
  return 0;
}
