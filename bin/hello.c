#include <stdio.h>
#include <unistd.h>
#include <exos/exos.h>

int
main (int argc, char **argv)
{
  printf ("hello from %s (pid %d, ppid %d)", argv[0], getpid (), getppid ());
  for (int i = 1; i < argc; i++)
    printf (" %s", argv[i]);
  printf ("\n");
  /* Map a page in the second gigabyte, never touched by the parent. */
  int r = exos_page_alloc (0x60000000, PTE_W);
  printf ("map 0x60000000 -> %d pte %llx\n", r, exos_pte (0x60000000));
  *(volatile int *) 0x60000000 = 42;
  printf ("wrote 0x60000000: %d\n", *(volatile int *) 0x60000000);
  return argc;
}
