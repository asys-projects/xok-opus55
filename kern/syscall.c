/*
 * Xok system call interface.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "env.h"
#include <xok/syscall.h>
#include <xok/net.h>

void utf_to_tf (const struct utf *utf, uctxt_t * tf, uint32_t priv);
void power_reset (int poweroff);

/* Device subsystems (net.c, xn.c, blk.c). */
int sys_net_xmit (struct env *e, unsigned card, uaddr_t recs, unsigned n,
		  uaddr_t notify);
int sys_dpf_insert (struct env *e, unsigned k, uaddr_t atoms, unsigned n,
		    unsigned ring);
int sys_dpf_delete (struct env *e, unsigned k, unsigned fid);
int sys_dpf_ref (struct env *e, unsigned k, unsigned fid, unsigned ke,
		 envid_t id);
int sys_dpf_pktring (struct env *e, unsigned k, unsigned fid, unsigned ring);
int sys_pktring_setring (struct env *e, uaddr_t ents, unsigned n);
int sys_pktring_modring (struct env *e, unsigned ring, unsigned idx,
			 uaddr_t ent);
int sys_pktring_delring (struct env *e, unsigned ring);
int sys_disk_request (struct env *e, unsigned k, unsigned dev,
		      uint32_t sector, unsigned nsect, uint32_t ppn,
		      unsigned write, uaddr_t done);
int sys_xn (struct env *e, unsigned num, unsigned long a1, unsigned long a2,
	    unsigned long a3, unsigned long a4, unsigned long a5,
	    unsigned long a6);

/*
 * Insert a mapping in E's address space, on behalf of CUR presenting
 * capability K for the physical page.
 */
static int
pte_insert (struct env *cur, struct env *e, unsigned k, vaddr_t va,
	    xpte_t pte)
{
  struct cap *c;
  pfn_t pfn, ppn;
  bool fresh = false;
  unsigned perm;
  int r;

  if ((va & PAGE_MASK) || va >= UXOK_BASE || va == UAREA)
    return -E_INVAL;
  if (!(pte & PTE_P))
    return env_unmap (e, va, pte);

  c = env_cap (cur, k);
  if (c == NULL)
    return -E_CAP_INVALID;
  perm = CAP_R | ((pte & PTE_W) ? CAP_W : 0);
  ppn = PTE_PPN (pte);
  if (ppn >= npages)
    return -E_INVAL;

  if (ppn == 0)
    {
      pfn = pmem_alloc (0);
      if (pfn == PFN_INVALID)
	return -E_NO_MEM;
      fresh = true;
    }
  else
    switch (ppinfo[ppn].pp_state)
      {
      case PP_FREE:
	pfn = pmem_alloc_specific (ppn);
	if (pfn == PFN_INVALID)
	  return -E_NOT_FREE;
	fresh = true;
	break;
      case PP_USER:
	if (!cap_grants (c, &ppages[ppn].pk_acl, perm))
	  return -E_CAP_INSUFF;
	pfn = ppn;
	break;
      case PP_RESERVED:
	/* Device memory: root capability only. */
	if (c->c_len != 0)
	  return -E_CAP_INSUFF;
	pfn = ppn;
	break;
      default:
	return -E_INVAL;
      }

  if (fresh)
    {
      ppinfo[pfn].pp_state = PP_USER;
      ppinfo[pfn].pp_owner = cur->id;
      memcpy (&ppages[pfn].pk_acl, c, sizeof (struct cap));
      revoke_check_memory ();
    }
  if (ppinfo[pfn].pp_state == PP_RESERVED)
    {
      /* Not reference counted. */
      pfn_t opfn;
      xpte_t m = env_getpte (e, va);
      if (m & PTE_P)
	env_unmap (e, va, 0);
      if (!umap_map (&e->umap, va, pfn,
		     HAL_PTE_P | HAL_PTE_U |
		     ((pte & PTE_W) ? HAL_PTE_W : 0), &opfn))
	return -E_NO_MEM;
      env_tlb_commit (e);
      return 0;
    }
  r = env_map (e, va, pfn, pte);
  if (r < 0 && fresh)
    pmem_free (pfn);
  return r;
}

static int
pte_modify (struct env *cur, struct env *e, unsigned k, vaddr_t va,
	    uint32_t set, uint32_t clr)
{
  const xpte_t mod = PTE_P | PTE_W | PTE_AVAIL;
  struct cap *c;
  xpte_t m, n;
  pfn_t pfn;

  if ((va & PAGE_MASK) || va >= UXOK_BASE || va == UAREA)
    return -E_INVAL;
  m = env_getpte (e, va);
  if (!(m & PTE_P))
    return 0;
  n = (m & ~(clr & mod)) | (set & mod);
  if (n == m)
    return 0;
  if (!(n & PTE_P))
    return env_unmap (e, va, n);
  pfn = PTE_PPN (m);
  if ((n & PTE_W) && !(m & PTE_W))
    {
      c = env_cap (cur, k);
      if (c == NULL)
	return -E_CAP_INVALID;
      if (ppinfo[pfn].pp_state != PP_USER
	  || !cap_grants (c, &ppages[pfn].pk_acl, CAP_W))
	return -E_CAP_INSUFF;
    }
  return env_map (e, va, pfn, n);
}

static int
sys_cputs (struct env *e, uaddr_t s, size_t len)
{
  char buf[128];

  while (len > 0)
    {
      size_t n = MIN (len, sizeof (buf));
      if (copyin (buf, s, n) < 0)
	return -E_FAULT;
      for (size_t i = 0; i < n; i++)
	putchar (buf[i]);
      s += n;
      len -= n;
    }
  return 0;
}

static int
sys_cap_forge (struct env *e, unsigned k, unsigned slot, uaddr_t ucap)
{
  struct cap *c = env_cap (e, k), n;

  if (c == NULL)
    return -E_CAP_INVALID;
  if (slot >= ENV_NCAPS)
    return -E_INVAL;
  if (copyin (&n, ucap, sizeof (n)) < 0)
    return -E_FAULT;
  if (n.c_len > CAP_NAMELEN)
    return -E_INVAL;
  n.c_valid = 1;
  n.c_pad = 0;
  if (!(c->c_perm & CAP_GRANT) || !cap_dominates (c, &n))
    return -E_CAP_INSUFF;
  memcpy ((void *) &e->info->e_caps[slot], &n, sizeof (n));
  return 0;
}

static int
sys_cap_grant (struct env *e, unsigned ke, envid_t id, unsigned k,
	       unsigned slot)
{
  struct env *t;
  struct cap *c;
  int r;

  if (slot >= ENV_NCAPS)
    return -E_INVAL;
  c = env_cap (e, k);
  if (c == NULL)
    return -E_CAP_INVALID;
  if (!(c->c_perm & CAP_GRANT))
    return -E_CAP_INSUFF;
  r = env_control (e, ke, id, &t);
  if (r < 0)
    return r;
  memcpy ((void *) &t->info->e_caps[slot], c, sizeof (*c));
  return 0;
}

static void
debug_envs (void)
{
  for (unsigned i = 0; i < NENV; i++)
    {
      struct env *e = envs + i;
      if (e->status == ENV_FREE)
	continue;
      kprintf ("env %08x parent %08x status %u cpu %d pages %u quanta %u\n",
	       e->id, e->info->e_parent, e->status, e->cpu,
	       e->info->e_npages, e->info->e_nquanta);
    }
  kprintf ("free pages: %u\n", pmem_nfree ());
}

void
syscall (struct env *e, unsigned long num, unsigned long a1,
	 unsigned long a2, unsigned long a3, unsigned long a4,
	 unsigned long a5, unsigned long a6)
{
  struct env *t;
  int r = 0;

  switch (num)
    {
    case SYS_null:
      r = 0;
      break;

    case SYS_cputs:
      r = sys_cputs (e, a1, a2);
      break;

    case SYS_cgetc:
      r = cons_getc ();
      break;

    case SYS_gettime:
      {
	uint64_t now = ktime ();
	r = copyout (a1, &now, sizeof (now));
	break;
      }

    case SYS_reboot:
      {
	struct cap *c = env_cap (e, a1);
	if (c == NULL)
	  r = -E_CAP_INVALID;
	else if (c->c_len != 0)
	  r = -E_CAP_INSUFF;
	else
	  power_reset (a2);
	break;
      }

    case SYS_getenvid:
      r = e->id;
      break;

    case SYS_env_alloc:
      {
	struct cap *c = env_cap (e, a1);
	if (c == NULL)
	  {
	    r = -E_CAP_INVALID;
	    break;
	  }
	r = env_alloc (e, c, &t);
	if (r == 0)
	  r = t->id;
	break;
      }

    case SYS_env_free:
      r = env_control (e, a1, a2, &t);
      if (r == 0)
	{
	  e->tf.eax = 0;
	  env_destroy (t);
	  return;
	}
      break;

    case SYS_env_set_status:
      r = env_control (e, a1, a2, &t);
      if (r < 0)
	break;
      if (a3 != ENV_RUNNABLE && a3 != ENV_NOT_RUNNABLE)
	{
	  r = -E_INVAL;
	  break;
	}
      if (t->status == ENV_IPC_WAIT)
	{
	  r = -E_BUSY;
	  break;
	}
      if (t->status == ENV_WAITING)
	wk_free (t);
      t->status = a3;
      t->info->e_status = a3;
      if (a3 == ENV_RUNNABLE)
	sched_kick_idle ();
      break;

    case SYS_env_set_tf:
      {
	struct utf utf;
	r = env_control (e, a1, a2, &t);
	if (r < 0)
	  break;
	if (t == e || t->cpu != -1)
	  {
	    r = -E_BUSY;
	    break;
	  }
	if (copyin (&utf, a3, sizeof (utf)) < 0)
	  {
	    r = -E_FAULT;
	    break;
	  }
	utf_to_tf (&utf, &t->tf, t->priv);
	break;
      }

    case SYS_env_setpriv:
      {
	struct cap *c = env_cap (e, a1);
	if (c == NULL)
	  {
	    r = -E_CAP_INVALID;
	    break;
	  }
	if (c->c_len != 0)
	  {
	    r = -E_CAP_INSUFF;
	    break;
	  }
	r = env_control (e, a1, a2, &t);
	if (r < 0)
	  break;
	t->priv = a3;
	if (a3 & ENV_PRIV_IO)
	  t->tf.eflags |= 0x3000;
	else
	  t->tf.eflags &= ~0x3000;
	break;
      }

    case SYS_cap_forge:
      r = sys_cap_forge (e, a1, a2, a3);
      break;

    case SYS_cap_grant:
      r = sys_cap_grant (e, a1, a2, a3, a4);
      break;

    case SYS_cap_clear:
      if (a1 >= ENV_NCAPS)
	r = -E_INVAL;
      else
	memset ((void *) &e->info->e_caps[a1], 0, sizeof (struct cap));
      break;

    case SYS_insert_pte:
      r = env_control (e, a5, a6, &t);
      if (r == 0)
	r = pte_insert (e, t, a1, a4, (xpte_t) a2 | ((xpte_t) a3 << 32));
      break;

    case SYS_insert_pte_range:
      {
	xpte_t ptes[32];
	unsigned n = a3, done = 0;
	r = env_control (e, a5, a6, &t);
	if (r < 0)
	  break;
	if (n > 1024)
	  {
	    r = -E_INVAL;
	    break;
	  }
	while (done < n && r >= 0)
	  {
	    unsigned m = MIN (n - done, 32);
	    if (copyin (ptes, a2 + done * sizeof (xpte_t),
			m * sizeof (xpte_t)) < 0)
	      {
		r = -E_FAULT;
		break;
	      }
	    for (unsigned i = 0; i < m && r >= 0; i++)
	      r = pte_insert (e, t, a1, a4 + (done + i) * PAGE_SIZE, ptes[i]);
	    done += m;
	  }
	break;
      }

    case SYS_mod_pte_range:
      r = env_control (e, a2, a3, &t);
      if (r < 0)
	break;
      if (a5 > 1024 * 1024)
	{
	  r = -E_INVAL;
	  break;
	}
      for (unsigned i = 0; i < a5 && r >= 0; i++)
	r = pte_modify (e, t, a1, a4 + i * PAGE_SIZE, a6 & 0xffff, a6 >> 16);
      break;

    case SYS_vpt_refresh:
      if ((a1 & PAGE_MASK) || a2 > 1024 * 1024)
	{
	  r = -E_INVAL;
	  break;
	}
      for (unsigned i = 0; i < a2 && r >= 0; i++)
	{
	  vaddr_t va = a1 + i * PAGE_SIZE;
	  if (va >= UXOK_BASE)
	    break;
	  r = env_vpt_refresh (e, va);
	}
      break;

    case SYS_ppage_acl:
      {
	struct cap g;
	if (a2 >= npages || ppinfo[a2].pp_state != PP_USER)
	  {
	    r = -E_INVAL;
	    break;
	  }
	r = cap_check (e, a1, &ppages[a2].pk_acl, CAP_W);
	if (r < 0)
	  break;
	if (copyin (&g, a3, sizeof (g)) < 0)
	  {
	    r = -E_FAULT;
	    break;
	  }
	if (g.c_len > CAP_NAMELEN)
	  {
	    r = -E_INVAL;
	    break;
	  }
	g.c_valid = 1;
	memcpy (&ppages[a2].pk_acl, &g, sizeof (g));
	break;
      }

    case SYS_quantum_alloc:
      r = sched_quantum_alloc (e, a1, (int) a2, a3, a4);
      break;

    case SYS_quantum_free:
      r = sched_quantum_free (e, a1, (int) a2, a3);
      break;

    case SYS_quantum_set:
      r = sched_quantum_set (e, a1, (int) a2, a3, a4);
      break;

    case SYS_cpu_revoke:
      r = sched_cpu_revoke (e, a1, a2, a3);
      break;

    case SYS_yield:
      {
	envid_t id = (envid_t) a1;
	e->tf.eax = 0;
	t = (id > 0) ? env_lookup (id) : NULL;
	sched_yield_to (t);
	return;
      }

    case SYS_wkpred:
      r = wk_install (e, a1, a2);
      if (r < 0)
	break;
      if (wk_eval (e))
	{
	  wk_free (e);
	  r = 0;
	  break;
	}
      e->tf.eax = 0;
      e->status = ENV_WAITING;
      e->info->e_status = ENV_WAITING;
      sched_yield_to (NULL);
      return;

    case SYS_ipc_call:
      {
	uint32_t args[5] = { a2, a3, a4, a5, e->id };
	r = ipc_call (e, a1, args);
	if (r == 0)
	  return;		/* Reply values set by ipc_reply. */
	break;
      }

    case SYS_ipc_reply:
      r = ipc_reply (e, a1, a2, a3);
      break;

    case SYS_msgring_setring:
      r = msgring_set (e, a1, a2);
      break;

    case SYS_msgring_delring:
      msgring_free (e);
      r = 0;
      break;

    case SYS_ipc_sendmsg:
      r = ipc_sendmsg (e, a1, a2, a3);
      break;

    case SYS_sreg_create:
      r = sreg_create (e, a1, a2);
      break;

    case SYS_sreg_destroy:
      r = sreg_destroy (e, a1, a2);
      break;

    case SYS_sreg_read:
      r = sreg_read (e, a1, a2, a3, a4, a5);
      break;

    case SYS_sreg_write:
      r = sreg_write (e, a1, a2, a3, a4, a5);
      break;

    case SYS_net_xmit:
      r = sys_net_xmit (e, a1, a2, a3, a4);
      break;

    case SYS_dpf_insert:
      r = sys_dpf_insert (e, a1, a2, a3, a4);
      break;

    case SYS_dpf_delete:
      r = sys_dpf_delete (e, a1, a2);
      break;

    case SYS_dpf_ref:
      r = sys_dpf_ref (e, a1, a2, a3, a4);
      break;

    case SYS_dpf_pktring:
      r = sys_dpf_pktring (e, a1, a2, a3);
      break;

    case SYS_pktring_setring:
      r = sys_pktring_setring (e, a1, a2);
      break;

    case SYS_pktring_modring:
      r = sys_pktring_modring (e, a1, a2, a3);
      break;

    case SYS_pktring_delring:
      r = sys_pktring_delring (e, a1);
      break;

    case SYS_disk_request:
      r = sys_disk_request (e, a1, a2, a3, a4 & 0x7fffffff, a5, a4 >> 31, a6);
      break;

    case SYS_xn_format ... SYS_xn_sync:
      r = sys_xn (e, num, a1, a2, a3, a4, a5, a6);
      break;

    case SYS_debug:
      switch (a1)
	{
	case DBG_PRINT_ENVS:
	  debug_envs ();
	  break;
	case DBG_PANIC:
	  kpanic ("panic requested by env %x", e->id);
	  break;
	case DBG_REVOKE:
	  {
	    struct env *v;
	    r = env_control (e, CAP_ROOT, a2, &v);
	    if (r == 0)
	      r = revoke_request (v, 16);
	    break;
	  }
	default:
	  r = -E_INVAL;
	}
      break;

    default:
      r = -E_NOSYS;
      break;
    }

  e->tf.eax = r;
}
