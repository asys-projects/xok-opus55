/*
 * Xok environments: creation, destruction and address spaces.
 *
 * Xok guards the hardware-defined page tables: applications cannot
 * write them directly, but every mapping is set through system calls
 * that check the caller's access rights to the physical page.  In
 * exchange the page tables are exposed read-only (UVPT) in a format
 * identical to the hardware's, including the software-available bits
 * and arbitrary values in non-present entries, so that library OSes
 * can implement copy-on-write, paging, etc.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "env.h"
#include <nux/cpumask.h>

struct env envs[NENV];
static volatile struct envinfo *envinfo_area;
static uint32_t env_gen[NENV];

/* A zeroed page mapped (read-only) where an env has no PTE mirror. */
static pfn_t zero_pfn;

void
env_init (void)
{
  vaddr_t kva;

  _Static_assert (sizeof (struct Uenv) == PAGE_SIZE, "Uenv size");
  _Static_assert (sizeof (struct envinfo) == 512, "envinfo size");

  envinfo_area = pmem_export_area (NENV * sizeof (struct envinfo), &kva);

  for (unsigned i = 0; i < NENV; i++)
    {
      struct env *e = envs + i;
      memset (e, 0, sizeof (*e));
      e->status = ENV_FREE;
      e->cpu = -1;
      e->info = envinfo_area + i;
      /* U-areas are permanently assigned to slots: other envs may
         keep read-only mappings of them (UENVS). */
      e->upfn = pfn_alloc (0);
      KASSERT (e->upfn != PFN_INVALID);
      e->u = kva_map (e->upfn, HAL_PTE_P | HAL_PTE_W);
      KASSERT (e->u != NULL);
    }
  zero_pfn = pfn_alloc (0);
  KASSERT (zero_pfn != PFN_INVALID);
}

struct env *
env_lookup (envid_t id)
{
  struct env *e;

  if (id <= 0)
    return NULL;
  e = envs + ENVX (id);
  if (e->status == ENV_FREE || e->id != id)
    return NULL;
  return e;
}

/*
 * Get environment ID with control rights: either the caller itself (id
 * 0 or own id) or an environment whose guard is dominated by the
 * caller's capability KE.
 */
int
env_control (struct env *cur, unsigned ke, envid_t id, struct env **out)
{
  struct env *e;
  int r;

  if (id == 0 || id == cur->id)
    {
      *out = cur;
      return 0;
    }
  e = env_lookup (id);
  if (e == NULL || e->status == ENV_DYING)
    return -E_BAD_ENV;
  r = cap_check (cur, ke, (const struct cap *) &e->info->e_envcap, CAP_W);
  if (r < 0)
    return r;
  *out = e;
  return 0;
}

/*
 * PTE helpers.
 */
static unsigned
pte2prot (xpte_t pte)
{
  unsigned p = HAL_PTE_U;

  if (pte & PTE_P)
    p |= HAL_PTE_P;
  if (pte & PTE_W)
    p |= HAL_PTE_W;
  if (!(pte & PTE_NX))
    p |= HAL_PTE_X;
  if (pte & PTE_AVAIL0)
    p |= HAL_PTE_AVL0;
  if (pte & PTE_AVAIL1)
    p |= HAL_PTE_AVL1;
  if (pte & PTE_AVAIL2)
    p |= HAL_PTE_AVL2;
  return p;
}

static xpte_t *
vpt_slot (struct env *e, vaddr_t va, bool alloc, pfn_t * pfnp)
{
  unsigned idx = va >> 21;
  pfn_t pfn;

  KASSERT (idx < VPT_NPAGES);
  if (e->vpt == NULL)
    {
      if (!alloc)
	return NULL;
      e->vpt = (pfn_t *) kmem_alloc (0, VPT_NPAGES * sizeof (pfn_t));
      KASSERT (e->vpt != NULL);
      for (unsigned i = 0; i < VPT_NPAGES; i++)
	e->vpt[i] = PFN_INVALID;
    }
  pfn = e->vpt[idx];
  if (pfn == PFN_INVALID)
    {
      if (!alloc)
	return NULL;
      pfn = pfn_alloc (0);
      if (pfn == PFN_INVALID)
	return NULL;
      e->vpt[idx] = pfn;
      /* Make it visible in the env's UVPT window. */
      env_export_map (e, UVPT + idx * PAGE_SIZE, pfn);
    }
  *pfnp = pfn;
  return (xpte_t *) pfn_get (pfn) + ((va >> PAGE_SHIFT) & 511);
}

static void
vpt_put (pfn_t pfn, xpte_t * p)
{
  pfn_put (pfn, (void *) ((uintptr_t) p & ~(uintptr_t) PAGE_MASK));
}

xpte_t
env_getpte (struct env *e, vaddr_t va)
{
  pfn_t pfn;
  xpte_t *p, v;

  p = vpt_slot (e, va, false, &pfn);
  if (p == NULL)
    return 0;
  v = *p;
  vpt_put (pfn, p);
  return v;
}

static int
vpt_set (struct env *e, vaddr_t va, xpte_t v)
{
  pfn_t pfn;
  xpte_t *p;

  p = vpt_slot (e, va, v != 0, &pfn);
  if (p == NULL)
    return v == 0 ? 0 : -E_NO_MEM;
  *p = v;
  vpt_put (pfn, p);
  return 0;
}

/*
 * Make TLBs coherent after changes to E's page tables.  This must be
 * complete before any page unmapped from E is reused.
 */
void
env_tlb_commit (struct env *e)
{
  hal_tlbop_t op = e->umap.tlbop;
  cpumask_t mask, me;

  if (op == HAL_TLBOP_NONE)
    return;
  __atomic_clear (&e->umap.tlbop, __ATOMIC_RELEASE);

  me = (cpumask_t) 1 << cpu_id ();
  mask = atomic_cpumask (&e->umap.cpumask);
  if (mask & ~me)
    cpu_tlbflush_broadcast_sync ();
  else if (mask & me)
    hal_cpu_tlbop (op);
}

/*
 * Map PFN at VA in E's address space with the protection described by
 * the hardware-format PTE flags.  Takes a reference on PFN, drops the
 * one held on any page previously mapped there.
 */
int
env_map (struct env *e, vaddr_t va, pfn_t pfn, xpte_t pte)
{
  pfn_t opfn = PFN_INVALID;
  xpte_t mirror;
  int r;

  KASSERT ((va & PAGE_MASK) == 0 && va < UXOK_BASE && va != UAREA);
  KASSERT (pte & PTE_P);

  mirror = PPN2PTE (pfn) | (pte & (PTE_P | PTE_W | PTE_AVAIL | PTE_NX))
    | PTE_U;
  r = vpt_set (e, va, mirror);
  if (r < 0)
    return r;
  if (!umap_map (&e->umap, va, pfn, pte2prot (pte), &opfn))
    {
      vpt_set (e, va, 0);
      return -E_NO_MEM;
    }
  pmem_page_ref (pfn);
  e->info->e_npages++;
  if (opfn != PFN_INVALID)
    {
      env_tlb_commit (e);
      e->info->e_npages--;
      pmem_page_unref (opfn);
    }
  return 0;
}

/*
 * Remove the mapping at VA.  KEEP is the non-present value the library
 * OS wants to store in the exported page table.
 */
int
env_unmap (struct env *e, vaddr_t va, xpte_t keep)
{
  pfn_t opfn;

  KASSERT ((va & PAGE_MASK) == 0 && va < UXOK_BASE && va != UAREA);
  KASSERT (!(keep & PTE_P));

  opfn = umap_unmap (&e->umap, va);
  if (vpt_set (e, va, keep) < 0)
    return -E_NO_MEM;
  if (opfn != PFN_INVALID)
    {
      env_tlb_commit (e);
      e->info->e_npages--;
      pmem_page_unref (opfn);
    }
  return 0;
}

/*
 * Copy the hardware accessed/dirty bits of VA into the exported table.
 */
int
env_vpt_refresh (struct env *e, vaddr_t va)
{
  hal_l1p_t l1p;
  hal_l1e_t l1e;
  pfn_t pfn;
  unsigned prot;
  xpte_t m;

  m = env_getpte (e, va);
  if (!(m & PTE_P))
    return 0;
  if (!hal_umap_getl1p (&e->umap.hal, va, false, &l1p))
    return 0;
  l1e = hal_l1e_get (l1p);
  hal_l1e_unbox (l1e, &pfn, &prot);
  m &= ~(PTE_A | PTE_D);
  if (prot & HAL_PTE_A)
    m |= PTE_A;
  if (prot & HAL_PTE_D)
    m |= PTE_D;
  return vpt_set (e, va, m);
}

/*
 * Map a kernel page read-only (kernel-exported structures) or the
 * u-area.  No reference counting: these pages belong to the kernel.
 */
void
env_export_map (struct env *e, vaddr_t va, pfn_t pfn)
{
  pfn_t opfn;
  unsigned prot = HAL_PTE_P | HAL_PTE_U;

  if (va == UAREA)
    prot |= HAL_PTE_W;
  if (!umap_map (&e->umap, va, pfn, prot, &opfn))
    kpanic ("out of memory mapping exported page");
  if (opfn != PFN_INVALID)
    env_tlb_commit (e);
}

/*
 * Resolve a fault in the kernel-exported region.  Returns true if the
 * fault was handled.
 */
bool
export_fault (struct env *e, vaddr_t va)
{
  pfn_t pfn = PFN_INVALID;

  va &= ~(vaddr_t) PAGE_MASK;
  if (va >= UVPT && va < UVPT + UVPT_SIZE)
    {
      unsigned idx = (va - UVPT) / PAGE_SIZE;
      if (e->vpt != NULL && e->vpt[idx] != PFN_INVALID)
	pfn = e->vpt[idx];
      else
	pfn = zero_pfn;
    }
  else if (va >= UENVINFO && va < UENVINFO + UENVINFO_SIZE)
    pfn = pmem_kva_pfn ((vaddr_t) envinfo_area + (va - UENVINFO));
  else if (va >= UENVS && va < UENVS + UENVS_SIZE)
    pfn = envs[(va - UENVS) / PAGE_SIZE].upfn;
  else if (va >= USYSINFO && va < USYSINFO + USYSINFO_SIZE)
    pfn = pmem_kva_pfn ((vaddr_t) sysinfo + (va - USYSINFO));
  else if (va >= UPPAGES && va < UPPAGES + UPPAGES_SIZE)
    {
      size_t off = va - UPPAGES;
      if (off < ROUNDUP (npages * sizeof (struct ppage_info), PAGE_SIZE))
	pfn = pmem_kva_pfn ((vaddr_t) ppinfo + off);
      else
	pfn = zero_pfn;
    }
  else
    {
      extern pfn_t xn_export_pfn (vaddr_t va);
      if (va >= UBC && va < UXOK_TOP)
	pfn = xn_export_pfn (va);
    }

  if (pfn == PFN_INVALID)
    return false;
  env_export_map (e, va, pfn);
  return true;
}

static void
env_setup_common (struct env *e, envid_t parent)
{
  unsigned x = e - envs;

  env_gen[x] = (env_gen[x] + 1) & 0x3fffff;
  if (env_gen[x] == 0)
    env_gen[x] = 1;
  e->id = (env_gen[x] << ENVX_BITS) | x;
  e->cpu = -1;
  e->need_prologue = false;
  e->in_epilogue = false;
  e->interrupted = false;
  e->penalty = 0;
  e->priv = 0;
  e->fpu_valid = false;
  e->vpt = NULL;
  e->wk = NULL;
  e->ipc_peer = 0;
  e->mr = NULL;
  memset (e->u, 0, PAGE_SIZE);
  memset ((void *) e->info, 0, sizeof (struct envinfo));
  e->info->e_id = e->id;
  e->info->e_parent = parent;
  e->info->e_cpu = -1;
}

int
env_alloc (struct env *parent, const struct cap *guard, struct env **out)
{
  struct env *e = NULL;

  for (unsigned i = 1; i < NENV; i++)
    if (envs[i].status == ENV_FREE)
      {
	e = envs + i;
	break;
      }
  if (e == NULL)
    return -E_NO_FREE_ENV;

  env_setup_common (e, parent ? parent->id : 0);
  umap_init (&e->umap);
  e->status = ENV_NOT_RUNNABLE;
  e->info->e_status = ENV_NOT_RUNNABLE;
  memcpy ((void *) &e->info->e_envcap, guard, sizeof (struct cap));
  if (parent)
    memcpy ((void *) e->info->e_caps, (void *) parent->info->e_caps,
	    sizeof (e->info->e_caps));

  uctxt_init (&e->tf, 0, 0, 0);
  env_export_map (e, UAREA, e->upfn);

  *out = e;
  return 0;
}

/*
 * The boot environment: the user program loaded by the boot loader
 * becomes the first environment.  It owns the root capability.
 */
struct env *
env_bootstrap (void)
{
  struct env *e = envs + 1;
  uaddr_t va;
  hal_l1e_t l1e;
  pfn_t pfn;
  unsigned prot;

  env_setup_common (e, 0);
  umap_bootstrap (&e->umap);
  e->status = ENV_RUNNABLE;
  e->info->e_status = ENV_RUNNABLE;
  memcpy ((void *) &e->info->e_envcap, &cap_root, sizeof (struct cap));
  memcpy ((void *) &e->info->e_caps[CAP_ROOT], &cap_root,
	  sizeof (struct cap));
  memcpy ((void *) &e->info->e_caps[CAP_USER], &cap_root,
	  sizeof (struct cap));

  if (!uctxt_bootstrap (&e->tf))
    kpanic ("no boot user program");

  /* Account for the mappings created by the boot loader. */
  va = 0;
  while ((va = hal_umap_next (&e->umap.hal, va, NULL, &l1e))
	 != UADDR_INVALID)
    {
      xpte_t m;

      hal_l1e_unbox (l1e, &pfn, &prot);
      if (!(prot & HAL_PTE_P))
	continue;
      KASSERT (pfn < npages);
      ppinfo[pfn].pp_state = PP_USER;
      ppinfo[pfn].pp_owner = e->id;
      memcpy (&ppages[pfn].pk_acl, &cap_root, sizeof (struct cap));
      pmem_page_ref (pfn);
      e->info->e_npages++;
      m = PPN2PTE (pfn) | PTE_P | PTE_U;
      if (prot & HAL_PTE_W)
	m |= PTE_W;
      if (!(prot & HAL_PTE_X))
	m |= PTE_NX;
      if (vpt_set (e, va, m) < 0)
	kpanic ("out of memory");
    }
  env_export_map (e, UAREA, e->upfn);
  return e;
}

/*
 * Tear down an environment's address space and resources.  E must not
 * be running on any CPU.
 */
void
env_reap (struct env *e)
{
  uaddr_t va;
  hal_l1e_t l1e;
  pfn_t pfn;
  unsigned prot;

  KASSERT (e->cpu == -1);
  KASSERT (e->umap.cpumask == 0);

  sched_env_freed (e);
  ipc_env_freed (e);
  wk_free (e);
  msgring_free (e);
  net_env_freed (e);
  xn_env_freed (e);
  fpu_switch_out (e);

  va = 0;
  while ((va = hal_umap_next (&e->umap.hal, va, NULL, &l1e))
	 != UADDR_INVALID)
    {
      hal_l1e_unbox (l1e, &pfn, &prot);
      if (!(prot & HAL_PTE_P))
	continue;
      if (va >= UXOK_BASE || va == UAREA)
	continue;
      pmem_page_unref (pfn);
    }
  umap_free (&e->umap);

  if (e->vpt)
    {
      for (unsigned i = 0; i < VPT_NPAGES; i++)
	if (e->vpt[i] != PFN_INVALID)
	  pfn_free (e->vpt[i]);
      kmem_free (0, (vaddr_t) e->vpt, VPT_NPAGES * sizeof (pfn_t));
      e->vpt = NULL;
    }

  e->status = ENV_FREE;
  e->info->e_status = ENV_FREE;
  e->info->e_npages = 0;
}

/*
 * Destroy E.  If E is running on another CPU it is marked dying, and
 * that CPU reaps it at its next kernel entry.
 */
void
env_destroy (struct env *e)
{
  if (e->status == ENV_FREE || e->status == ENV_DYING)
    return;

  e->status = ENV_DYING;
  e->info->e_status = ENV_DYING;
  if (e->cpu != -1 && e->cpu != (int) cpu_id ())
    {
      /* Will be reaped by its CPU. */
      pcpus[e->cpu].ipi_pending |= IPI_RESCHED;
      cpu_ipi (e->cpu);
      return;
    }
  if (e->cpu == (int) cpu_id ())
    sched_switch_away ();
  env_reap (e);
}
