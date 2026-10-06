#include <stdio.h>
#include <exos/exos.h>

int
main (void)
{
  uint64_t s = exos_time_ns () / 1000000000ULL;
  int nproc = 0;
  for (unsigned i = 1; i < NENV; i++)
    if (((volatile struct envinfo *) UENVINFO)[i].e_status != ENV_FREE)
      nproc++;
  printf ("up %llu:%02llu:%02llu, %d environments, %u CPUs\n", s / 3600,
	  (s / 60) % 60, s % 60, nproc, sysinfo_page->si_ncpu);
  return 0;
}
