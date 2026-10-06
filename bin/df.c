/* df: XN disks, from the exported free maps. */
#include <stdio.h>
#include <exos/exos.h>
#include <xok/xn.h>

int
main (void)
{
  printf ("disk  driver          blocks      used      free  xn\n");
  for (unsigned d = 0; d < sysinfo_page->si_ndisk; d++)
    {
      volatile struct diskinfo *di = &sysinfo_page->si_disk[d];
      unsigned used = 0, total = 0, freeb = 0;
      if (di->d_xn)
	{
	  volatile struct xn_super *s = xn_cat_super (d);
	  volatile uint32_t *fm = xn_freemap (d);
	  total = s->s_nblocks;
	  for (unsigned b = 0; b < total; b++)
	    if ((fm[b / 32] >> (b % 32)) & 1)
	      freeb++;
	  used = total - freeb;
	}
      printf ("blk%u  %-12s %9u %9u %9u  %s\n", d, (const char *) di->d_name,
	      di->d_xn ? total : di->d_nblocks, used, freeb,
	      di->d_xn ? "yes" : "no");
    }
  return 0;
}
