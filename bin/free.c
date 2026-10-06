/* free: physical memory, from Xok's exported page information. */
#include <stdio.h>
#include <exos/exos.h>

int
main (void)
{
  unsigned n = sysinfo_page->si_nppages, st[5] = { 0 }, pinned = 0;
  for (unsigned i = 0; i < n; i++)
    {
      volatile struct ppage_info *p = &ppages_info[i];
      if (p->pp_state < 5)
	st[p->pp_state]++;
      if (p->pp_pinned)
	pinned++;
    }
  printf ("pages      total     free   kernel     user   buffer   pinned\n");
  printf ("        %8u %8u %8u %8u %8u %8u\n", n, st[PP_FREE], st[PP_KERNEL],
	  st[PP_USER], st[PP_BC], pinned);
  printf ("KB      %8u %8u %8u %8u %8u\n", n * 4, st[PP_FREE] * 4,
	  st[PP_KERNEL] * 4, st[PP_USER] * 4, st[PP_BC] * 4);
  return 0;
}
