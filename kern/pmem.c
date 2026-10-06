/*
 * Xok physical memory multiplexing.
 *
 * Xok exposes physical memory by name.  This module takes ownership of
 * every free physical page from NUX's boot allocator and becomes the
 * page allocator of the whole system (NUX included, through
 * nux_set_allocator()), so that applications can ask for specific
 * physical pages and so that the state of every page can be exported
 * read-only to applications (UPPAGES).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "kern.h"
#include <stree.h>
#include <nux/apxh.h>

struct ppage *ppages;
volatile struct ppage_info *ppinfo;
uint32_t npages;

#define PNIL 0xffffffffU

static lock_t pmem_lock;
static uint32_t free_head = PNIL;
static uint32_t nfree;

static void
fl_insert (uint32_t pfn)
{
  ppages[pfn].pk_prev = PNIL;
  ppages[pfn].pk_next = free_head;
  if (free_head != PNIL)
    ppages[free_head].pk_prev = pfn;
  free_head = pfn;
  ppinfo[pfn].pp_state = PP_FREE;
  ppinfo[pfn].pp_owner = 0;
  ppinfo[pfn].pp_refcnt = 0;
  ppinfo[pfn].pp_pinned = 0;
  ppinfo[pfn].pp_bc = 0;
  memset (&ppages[pfn].pk_acl, 0, sizeof (struct cap));
  nfree++;
}

static void
fl_remove (uint32_t pfn)
{
  uint32_t n = ppages[pfn].pk_next, p = ppages[pfn].pk_prev;

  if (p != PNIL)
    ppages[p].pk_next = n;
  else
    free_head = n;
  if (n != PNIL)
    ppages[n].pk_prev = p;
  ppages[pfn].pk_next = ppages[pfn].pk_prev = PNIL;
  nfree--;
}

void
pmem_zero (pfn_t pfn)
{
  void *va = pfn_get (pfn);
  memset (va, 0, PAGE_SIZE);
  pfn_put (pfn, va);
}

void
pmem_copy (pfn_t dst, pfn_t src)
{
  void *d = pfn_get (dst);
  void *s = pfn_get (src);
  memcpy (d, s, PAGE_SIZE);
  pfn_put (src, s);
  pfn_put (dst, d);
}

static void
update_sysinfo (void)
{
  if (sysinfo)
    sysinfo->si_nfreepages = nfree;
}

/*
 * Allocate a page.  If LOW is set return the lowest free page
 * (needed by NUX for real-mode trampolines).  The page is zeroed.
 */
pfn_t
pmem_alloc (int low)
{
  uint32_t pfn;

  spinlock (&pmem_lock);
  if (low)
    {
      for (pfn = 0; pfn < npages; pfn++)
	if (ppinfo[pfn].pp_state == PP_FREE)
	  break;
      if (pfn == npages)
	pfn = PNIL;
    }
  else
    pfn = free_head;

  if (pfn == PNIL)
    {
      spinunlock (&pmem_lock);
      return PFN_INVALID;
    }
  fl_remove (pfn);
  ppinfo[pfn].pp_state = PP_KERNEL;
  update_sysinfo ();
  spinunlock (&pmem_lock);

  pmem_zero (pfn);
  return pfn;
}

/*
 * Allocate a specific page, if free.
 */
pfn_t
pmem_alloc_specific (pfn_t pfn)
{
  if (pfn >= npages)
    return PFN_INVALID;

  spinlock (&pmem_lock);
  if (ppinfo[pfn].pp_state != PP_FREE)
    {
      spinunlock (&pmem_lock);
      return PFN_INVALID;
    }
  fl_remove (pfn);
  ppinfo[pfn].pp_state = PP_KERNEL;
  update_sysinfo ();
  spinunlock (&pmem_lock);

  pmem_zero (pfn);
  return pfn;
}

/*
 * Allocate N physically contiguous pages (for device DMA structures).
 */
pfn_t
pmem_alloc_contig (unsigned n)
{
  uint32_t start, i;

  spinlock (&pmem_lock);
  for (start = 1; start + n <= npages; start++)
    {
      for (i = 0; i < n; i++)
	if (ppinfo[start + i].pp_state != PP_FREE)
	  break;
      if (i == n)
	break;
      start += i;
    }
  if (start + n > npages)
    {
      spinunlock (&pmem_lock);
      return PFN_INVALID;
    }
  for (i = 0; i < n; i++)
    {
      fl_remove (start + i);
      ppinfo[start + i].pp_state = PP_KERNEL;
    }
  update_sysinfo ();
  spinunlock (&pmem_lock);
  for (i = 0; i < n; i++)
    pmem_zero (start + i);
  return start;
}

void
pmem_free (pfn_t pfn)
{
  KASSERT (pfn < npages);
  spinlock (&pmem_lock);
  KASSERT (ppinfo[pfn].pp_state != PP_FREE);
  KASSERT (ppinfo[pfn].pp_state != PP_RESERVED);
  fl_insert (pfn);
  update_sysinfo ();
  spinunlock (&pmem_lock);
}

uint32_t
pmem_nfree (void)
{
  return nfree;
}

bool
pmem_is_ram (pfn_t pfn)
{
  return pfn < npages && ppinfo[pfn].pp_state != PP_RESERVED;
}

/*
 * A user page is free when no process has access to it any more:
 * i.e. when it is neither mapped nor pinned by the kernel.
 */
static void
maybe_release (pfn_t pfn)
{
  volatile struct ppage_info *pi = ppinfo + pfn;

  if (pi->pp_state == PP_USER && pi->pp_refcnt == 0 && pi->pp_pinned == 0)
    pmem_free (pfn);
}

void
pmem_page_ref (pfn_t pfn)
{
  KASSERT (pfn < npages);
  KASSERT (ppinfo[pfn].pp_refcnt < 0xffff);
  ppinfo[pfn].pp_refcnt++;
}

void
pmem_page_unref (pfn_t pfn)
{
  extern void xn_page_unref (pfn_t pfn);

  KASSERT (pfn < npages);
  KASSERT (ppinfo[pfn].pp_refcnt > 0);
  ppinfo[pfn].pp_refcnt--;
  if (ppinfo[pfn].pp_state == PP_BC)
    xn_page_unref (pfn);
  maybe_release (pfn);
}

void
pmem_pin (pfn_t pfn)
{
  KASSERT (pfn < npages);
  KASSERT (ppinfo[pfn].pp_pinned < 0xff);
  ppinfo[pfn].pp_pinned++;
}

void
pmem_unpin (pfn_t pfn)
{
  KASSERT (pfn < npages);
  KASSERT (ppinfo[pfn].pp_pinned > 0);
  ppinfo[pfn].pp_pinned--;
  maybe_release (pfn);
}

/*
 * Allocate a page-aligned, page-granular, zeroed kernel area that can
 * be exported to user space (its pages contain nothing else).
 */
void *
pmem_export_area (size_t size, vaddr_t * kvap)
{
  vaddr_t va;
  size = ROUNDUP (size, PAGE_SIZE);

  va = kva_alloc (size);
  KASSERT (va != VADDR_INVALID);
  for (size_t off = 0; off < size; off += PAGE_SIZE)
    {
      pfn_t pfn = pfn_alloc (0);
      KASSERT (pfn != PFN_INVALID);
      kmap_map (va + off, pfn, HAL_PTE_P | HAL_PTE_W);
    }
  kmap_commit ();
  memset ((void *) va, 0, size);
  if (kvap)
    *kvap = va;
  return (void *) va;
}

pfn_t
pmem_kva_pfn (vaddr_t va)
{
  return kmap_getpfn (va & ~(vaddr_t) PAGE_MASK);
}

static pfn_t
nux_alloc (int low)
{
  return pmem_alloc (low);
}

static void
nux_free (pfn_t pfn)
{
  pmem_free (pfn);
}

void
pmem_init (void)
{
  unsigned order;
  WORD_T *stree;
  uint32_t i;
  vaddr_t va;

  spinlock_init (&pmem_lock);

  npages = hal_physmem_maxrampfn () + 1;

  /* Allocate our tables with NUX's boot allocator first. */
  ppinfo = pmem_export_area (npages * sizeof (struct ppage_info), &va);
  ppages = (struct ppage *) kmalloc (npages * sizeof (struct ppage));
  KASSERT (ppages != NULL);
  memset (ppages, 0, npages * sizeof (struct ppage));

  /* Classify pages: RAM pages are kernel-owned unless free. */
  for (i = 0; i < npages; i++)
    {
      ppinfo[i].pp_state = PP_RESERVED;
      ppages[i].pk_next = ppages[i].pk_prev = PNIL;
    }
  for (unsigned r = 0; r < hal_physmem_numregions (); r++)
    {
      struct apxh_region *reg = hal_physmem_region (r);
      if (reg == NULL || reg->type != APXH_REGION_RAM)
	continue;
      for (uint64_t p = reg->pfn; p < reg->pfn + reg->len && p < npages;
	   p++)
	ppinfo[p].pp_state = PP_KERNEL;
    }
  /* Pinned non-RAM regions (low memory holes) override RAM regions. */
  for (unsigned r = 0; r < hal_physmem_numregions (); r++)
    {
      struct apxh_region *reg = hal_physmem_region (r);
      if (reg == NULL || reg->type == APXH_REGION_RAM)
	continue;
      for (uint64_t p = reg->pfn; p < reg->pfn + reg->len && p < npages;
	   p++)
	ppinfo[p].pp_state = PP_RESERVED;
    }

  /* Steal every free page from the NUX boot allocator. */
  stree = hal_physmem_stree (&order);
  KASSERT (stree != NULL);
  for (i = npages; i-- > 0;)
    {
      if (stree_getbit (stree, order, i))
	{
	  stree_clrbit (stree, order, i);
	  fl_insert (i);
	}
    }

  nux_set_allocator (nux_alloc, nux_free);
  kprintf ("xok: physical memory: %u pages, %u free (%u KB)\n",
	   npages, nfree, nfree * 4);
}
