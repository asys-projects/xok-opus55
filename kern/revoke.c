/*
 * Visible resource revocation.
 *
 * "Exokernels expose revocation policies to applications" [ENG95]: when
 * physical memory runs low, the kernel asks an environment (the one
 * holding most pages) to give pages back, with a revocation upcall
 * telling how many.  The library chooses what to release.  If it does
 * not comply within a grace period the kernel applies the abort
 * protocol: it breaks the environment's mappings of buffer cache pages
 * in the range the library declared as revocable (its caches: XN can
 * then reclaim the pages, and the library can map them again later) and
 * records what it took in the u-area's repossession vector.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "env.h"

#define REVOKE_PAGES 64
#define REVOKE_GRACE_NSEC (500ULL * 1000 * 1000)

static uint64_t last_request;

static uint32_t
low_watermark (void)
{
  uint32_t w = npages / 64;
  return w < 64 ? 64 : w;
}

int
revoke_request (struct env *e, unsigned n)
{
  if (e == NULL || e->status == ENV_FREE || e->status == ENV_DYING)
    return -E_BAD_ENV;
  if (e->revoke_pending)
    return 0;
  e->revoke_pending = true;
  e->revoke_delivered = false;
  e->revoke_deadline = ktime () + REVOKE_GRACE_NSEC;
  e->u->u_revoke_npages = n;
  /* A sleeping environment must run to handle the request. */
  if (e->status == ENV_WAITING)
    {
      wk_free (e);
      e->status = ENV_RUNNABLE;
      e->info->e_status = ENV_RUNNABLE;
    }
  sched_kick_idle ();
  return 0;
}

/* Called after page allocations. */
void
revoke_check_memory (void)
{
  struct env *victim = NULL;
  uint64_t now;

  if (pmem_nfree () >= low_watermark ())
    return;
  now = ktime ();
  if (now - last_request < REVOKE_GRACE_NSEC)
    return;
  for (unsigned i = 1; i < NENV; i++)
    {
      struct env *e = envs + i;
      if (e->status == ENV_FREE || e->status == ENV_DYING
	  || e->revoke_pending)
	continue;
      if (victim == NULL || e->info->e_npages > victim->info->e_npages)
	victim = e;
    }
  if (victim)
    {
      last_request = now;
      revoke_request (victim, REVOKE_PAGES);
    }
}

/* The abort protocol. */
static void
repossess (struct env *e)
{
  uaddr_t va = 0;
  hal_l1e_t l1e;
  pfn_t pfn;
  unsigned prot, n = 0;

  while ((va = hal_umap_next (&e->umap.hal, va, NULL, &l1e))
	 != UADDR_INVALID && n < e->u->u_revoke_npages)
    {
      hal_l1e_unbox (l1e, &pfn, &prot);
      if (!(prot & HAL_PTE_P) || va >= UXOK_BASE || va == UAREA)
	continue;
      /* Only where the library said its caches are. */
      if (va < e->u->u_revoke_lo || va >= e->u->u_revoke_hi)
	continue;
      if (pfn >= npages || ppinfo[pfn].pp_state != PP_BC)
	continue;
      env_unmap (e, va, 0);
      if (e->u->u_nrepossessed < 16)
	e->u->u_repossessed[e->u->u_nrepossessed] = va;
      e->u->u_nrepossessed++;
      n++;
    }
  kprintf ("xok: env %x did not release memory: %u mappings repossessed\n",
	   e->id, n);
}

void
revoke_timer (void)
{
  uint64_t now = ktime ();

  for (unsigned i = 1; i < NENV; i++)
    {
      struct env *e = envs + i;
      if (e->status == ENV_FREE || !e->revoke_pending)
	continue;
      if (e->u->u_revoke_npages == 0)
	{
	  e->revoke_pending = false;
	  continue;
	}
      if (now >= e->revoke_deadline)
	{
	  if (e->cpu == -1 || e->cpu == (int) cpu_id ())
	    repossess (e);
	  e->u->u_revoke_npages = 0;
	  e->revoke_pending = false;
	}
    }
}
