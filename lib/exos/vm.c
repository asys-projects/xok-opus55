/*
 * ExOS virtual memory.
 *
 * Xok exposes physical pages and the (hardware) page table; all
 * policy is here: lazily allocated stacks, the heap, copy-on-write
 * (implemented with a software PTE bit and the read-only exported
 * page table), anonymous mappings.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <exos/exos.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

extern char _end[];
static uintptr_t brk_cur, brk_mapped;

int
exos_page_alloc (uintptr_t va, xpte_t perm)
{
  return sys_self_insert_pte (EXOS_CAP, (perm | PTE_P | PTE_U) & ~PTE_FRAME,
			      va);
}

int
exos_page_unmap (uintptr_t va)
{
  return sys_self_insert_pte (EXOS_CAP, 0, va);
}

int
exos_range_alloc (uintptr_t va, size_t len, xpte_t perm)
{
  for (uintptr_t a = PGROUNDDOWN (va); a < va + len; a += PGSIZE)
    {
      int r = exos_page_alloc (a, perm);
      if (r < 0)
	return r;
    }
  return 0;
}

int
exos_range_unmap (uintptr_t va, size_t len)
{
  for (uintptr_t a = PGROUNDDOWN (va); a < va + len; a += PGSIZE)
    if (exos_mapped (a))
      exos_page_unmap (a);
  return 0;
}

/*
 * Copy-on-write fault at VA: copy the page to a fresh one, or, if we
 * hold the only reference, simply make it writable.
 */
#define COW_TMP (UMMAP_TOP - PGSIZE)

int
exos_cow_fault (uintptr_t va)
{
  xpte_t pte;
  uintptr_t page = PGROUNDDOWN (va);
  ppn_t ppn;
  int r;

  pte = exos_pte (page);
  if (!(pte & PTE_P) || !(pte & PTE_COW))
    return -1;
  ppn = PTE_PPN (pte);
  if (ppages_info[ppn].pp_refcnt == 1 && ppages_info[ppn].pp_state == PP_USER
      && sys_mod_pte_range (EXOS_CAP, 0, 0, page, 1, PTE_W, PTE_COW) == 0)
    return 0;

  r = sys_self_insert_pte (EXOS_CAP, PTE_P | PTE_W | PTE_U, COW_TMP);
  if (r < 0)
    return r;
  memcpy ((void *) COW_TMP, (void *) page, PGSIZE);
  r = sys_self_insert_pte (EXOS_CAP, (vpt[COW_TMP >> PGSHIFT] & PTE_FRAME)
			   | PTE_P | PTE_W | PTE_U
			   | (pte & (PTE_AVAIL & ~PTE_COW)), page);
  sys_self_insert_pte (EXOS_CAP, 0, COW_TMP);
  return r;
}

/*
 * Make sure a user buffer is present (and writable, if WRITE) before
 * handing it to the kernel, which never resolves faults for us.
 */
int
exos_touch (const void *buf, size_t len, int write)
{
  uintptr_t a, end = (uintptr_t) buf + len;

  for (a = PGROUNDDOWN ((uintptr_t) buf); a < end; a += PGSIZE)
    {
      xpte_t pte = exos_pte (a);
      if (!(pte & PTE_P))
	{
	  if (a >= USTACKBOT && a < USTACKTOP)
	    {
	      if (exos_page_alloc (a, PTE_W) < 0)
		return -1;
	      continue;
	    }
	  return -1;
	}
      if (write && !(pte & PTE_W))
	{
	  if (!(pte & PTE_COW) || exos_cow_fault (a) < 0)
	    return -1;
	}
    }
  return 0;
}

void *
sbrk (intptr_t inc)
{
  uintptr_t old = brk_cur, nbrk = brk_cur + inc;

  if (inc < 0)
    {
      if (nbrk < (uintptr_t) _end)
	{
	  errno = EINVAL;
	  return (void *) -1;
	}
      brk_cur = nbrk;
      return (void *) old;
    }
  if (nbrk > UHEAP_TOP || nbrk < old)
    {
      errno = ENOMEM;
      return (void *) -1;
    }
  while (brk_mapped < nbrk)
    {
      if (!exos_mapped (brk_mapped)
	  && exos_page_alloc (brk_mapped, PTE_W) < 0)
	{
	  errno = ENOMEM;
	  return (void *) -1;
	}
      brk_mapped += PGSIZE;
    }
  brk_cur = nbrk;
  return (void *) old;
}

/*
 * Anonymous mappings: a simple first-fit allocator of the mmap area.
 */
#define NMMAP 64
static struct
{
  uintptr_t va;
  size_t len;
} mmaps[NMMAP];

void *
exos_mmap_alloc (size_t len)
{
  uintptr_t va = UMMAP;

  len = PGROUNDUP (len);
again:
  for (int i = 0; i < NMMAP; i++)
    if (mmaps[i].len && va < mmaps[i].va + mmaps[i].len
	&& va + len > mmaps[i].va)
      {
	va = mmaps[i].va + mmaps[i].len;
	goto again;
      }
  if (va + len > COW_TMP)
    return NULL;
  for (int i = 0; i < NMMAP; i++)
    if (mmaps[i].len == 0)
      {
	if (exos_range_alloc (va, len, PTE_W) < 0)
	  {
	    exos_range_unmap (va, len);
	    return NULL;
	  }
	mmaps[i].va = va;
	mmaps[i].len = len;
	return (void *) va;
      }
  return NULL;
}

void
exos_mmap_regions (void (*fn) (uintptr_t, size_t, void *), void *arg)
{
  for (int i = 0; i < NMMAP; i++)
    if (mmaps[i].len)
      fn (mmaps[i].va, mmaps[i].len, arg);
}

void
exos_mmap_free (void *p, size_t len)
{
  for (int i = 0; i < NMMAP; i++)
    if (mmaps[i].va == (uintptr_t) p)
      {
	exos_range_unmap (mmaps[i].va, mmaps[i].len);
	mmaps[i].len = 0;
	return;
      }
}

void
exos_vm_init (void)
{
  brk_cur = (uintptr_t) _end;
  brk_mapped = PGROUNDUP (brk_cur);
  /* Exception stack. */
  for (uintptr_t a = XSTACKTOP - XSTACKSIZE; a < XSTACKTOP; a += PGSIZE)
    if (!exos_mapped (a))
      exos_page_alloc (a, PTE_W | PTE_NOFORK);
}
