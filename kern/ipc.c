/*
 * Xok IPC: protected control transfers and message rings.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "env.h"

/*
 * Protected control transfer: CUR calls TO.  The control transfer is
 * atomic: TO is entered at its IPC upcall on this CPU, in the rest of
 * CUR's time slice.  CUR sleeps until TO replies.
 */
int
ipc_call (struct env *cur, envid_t to, const uint32_t * args)
{
  struct env *t = env_lookup (to);

  if (t == NULL || t->status == ENV_DYING)
    return -E_BAD_ENV;
  if (t == cur)
    return -E_INVAL;
  if (!t->u->u_ipc_accept || !t->u->u_entipc)
    return -E_IPC_BLOCKED;
  if (t->cpu != -1 || t->ipc_pending)
    return -E_BUSY;
  if (t->status != ENV_RUNNABLE && t->status != ENV_WAITING)
    return -E_IPC_BLOCKED;

  if (t->status == ENV_WAITING)
    {
      /* The callee's sys_wkpred returns; the library re-checks. */
      wk_free (t);
      t->status = ENV_RUNNABLE;
      t->info->e_status = ENV_RUNNABLE;
    }

  memcpy (t->ipc_args, args, sizeof (t->ipc_args));
  t->ipc_pending = true;

  cur->status = ENV_IPC_WAIT;
  cur->info->e_status = ENV_IPC_WAIT;
  cur->ipc_peer = t->id;
  cur->tf.eax = -E_UNSPEC;
  sched_yield_to (t);
  return 0;
}

/*
 * TO's IPC reply: resume the caller, donating the rest of the slice.
 */
int
ipc_reply (struct env *cur, envid_t to, uint32_t r0, uint32_t r1)
{
  struct env *c = env_lookup (to);

  if (c == NULL || c->status != ENV_IPC_WAIT || c->ipc_peer != cur->id)
    return -E_INVAL;
  c->tf.eax = r0;
  c->tf.edx = r1;
  c->ipc_peer = 0;
  c->status = ENV_RUNNABLE;
  c->info->e_status = ENV_RUNNABLE;
  c->ipc_return = true;
  cur->tf.eax = 0;
  sched_yield_to (c);
  return 0;
}

/*
 * E is going away: release callers blocked on it.
 */
void
ipc_env_freed (struct env *e)
{
  for (unsigned i = 0; i < NENV; i++)
    {
      struct env *c = envs + i;
      if (c->status == ENV_IPC_WAIT && c->ipc_peer == e->id)
	{
	  c->tf.eax = -E_BAD_ENV;
	  c->ipc_peer = 0;
	  c->status = ENV_RUNNABLE;
	  c->info->e_status = ENV_RUNNABLE;
	}
    }
  e->ipc_pending = false;
  e->ipc_peer = 0;
}

/*
 * Message rings.
 */
struct msgring
{
  unsigned n;
  unsigned head;
  pfn_t pfn;
  unsigned off;			/* Offset of entry 0 in the page. */
};

void
msgring_free (struct env *e)
{
  if (e->mr == NULL)
    return;
  pmem_unpin (e->mr->pfn);
  kmem_free (0, (vaddr_t) e->mr, sizeof (struct msgring));
  e->mr = NULL;
}

int
msgring_set (struct env *e, uaddr_t ring, unsigned n)
{
  struct msgring *mr;
  xpte_t pte;
  size_t size = n * sizeof (struct msgring_ent);

  if (n == 0 || n > MSGRING_MAXENTS || (ring & 3))
    return -E_INVAL;
  if ((ring & PAGE_MASK) + size > PAGE_SIZE || ring >= UXOK_BASE)
    return -E_INVAL;
  pte = env_getpte (e, ring & ~(uaddr_t) PAGE_MASK);
  if (!(pte & PTE_P) || !(pte & PTE_W))
    return -E_FAULT;
  if (ppinfo[PTE_PPN (pte)].pp_state != PP_USER)
    return -E_INVAL;

  msgring_free (e);
  mr = (struct msgring *) kmem_alloc (0, sizeof (*mr));
  if (mr == NULL)
    return -E_NO_MEM;
  mr->n = n;
  mr->head = 0;
  mr->pfn = PTE_PPN (pte);
  mr->off = ring & PAGE_MASK;
  pmem_pin (mr->pfn);
  e->mr = mr;
  e->u->u_msgring_head = 0;
  return 0;
}

int
ipc_sendmsg (struct env *cur, envid_t to, uaddr_t msg, unsigned size)
{
  struct env *t = env_lookup (to);
  struct msgring *mr;
  struct msgring_ent *ent;
  uint8_t buf[MSGRING_MSGSIZE];
  uint8_t *page;
  int r = 0;

  if (t == NULL || t->status == ENV_DYING)
    return -E_BAD_ENV;
  if (size > MSGRING_MSGSIZE)
    return -E_INVAL;
  mr = t->mr;
  if (mr == NULL)
    return -E_IPC_BLOCKED;
  if (copyin (buf, msg, size) < 0)
    return -E_FAULT;

  page = pfn_get (mr->pfn);
  ent = (struct msgring_ent *) (page + mr->off) + mr->head;
  if (ent->m_flag != 0)
    r = -E_FULL;
  else
    {
      ent->m_from = cur->id;
      ent->m_size = size;
      memcpy (ent->m_buf, buf, size);
      __atomic_store_n (&ent->m_flag, 1, __ATOMIC_RELEASE);
      mr->head = (mr->head + 1) % mr->n;
      t->u->u_msgring_head = mr->head;
    }
  pfn_put (mr->pfn, page);
  if (r == 0)
    sched_kick_idle ();
  return r;
}
