/* sysinfo: what Xok exposes about the machine. */
#include <stdio.h>
#include <exos/exos.h>

int
main (void)
{
  volatile struct sysinfo *si = sysinfo_page;
  printf ("CPUs: %u, time slice %u us, %u slices per CPU\n", si->si_ncpu,
	  si->si_tick_nsec / 1000, si->si_nquanta);
  printf ("memory: %u pages, %u free\n", si->si_nppages, si->si_nfreepages);
  for (unsigned c = 0; c < si->si_ncpu; c++)
    {
      int used = 0;
      for (int q = 0; q < NQUANTA; q++)
	if (si->si_qvec[c][q].q_env)
	  used++;
      printf ("cpu%u: env %x slice %u, %d slices allocated, idle %llu\n", c,
	      si->si_cpu[c].c_env, si->si_cpu[c].c_q, used,
	      si->si_cpu[c].c_idle_ticks);
    }
  for (unsigned d = 0; d < si->si_ndisk; d++)
    printf ("disk%u: %s, %u sectors%s\n", d, (const char *) si->si_disk[d].d_name,
	    si->si_disk[d].d_nsectors, si->si_disk[d].d_xn ? ", XN" : "");
  for (unsigned n = 0; n < si->si_nnet; n++)
    {
      volatile struct netinfo *ni = &si->si_net[n];
      printf ("net%u: %s, %02x:%02x:%02x:%02x:%02x:%02x, link %s, rx %u tx %u "
	      "dropped %u\n", n, (const char *) ni->n_name, ni->n_mac[0],
	      ni->n_mac[1], ni->n_mac[2], ni->n_mac[3], ni->n_mac[4],
	      ni->n_mac[5], ni->n_link ? "up" : "down", ni->n_rxpkts,
	      ni->n_txpkts, ni->n_rxdrop);
    }
  return 0;
}
