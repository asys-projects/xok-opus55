/*
 * Xok network multiplexing: dynamic packet filters, packet rings,
 * transmission (see <xok/net.h>).
 *
 * Filters are merged in a trie of atoms: filters that share a prefix of
 * atoms share the corresponding nodes, so that each atom common to
 * several filters is evaluated once per packet.  A packet is delivered
 * to the deepest matching filter (the most specific one).  Installing a
 * filter whose atoms extend those of an existing filter, or equal them,
 * would steal that filter's packets: it requires a capability granting
 * write access to the existing filter's guard.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "env.h"
#include "net.h"
#include "dev.h"
#include <xok/net.h>

static struct netdev *netdevs[NET_MAXDEV];
static unsigned nnetdevs;

/*
 * Packet filter trie.
 */
#define DPF_MAXNODES (DPF_MAXFILTERS * 8)

struct dpf_node
{
  struct dpf_atom atom;
  struct dpf_node *parent, *child, *sibling;
  int filter;			/* Filter ending here, or -1. */
  bool used;
};

#define DPF_MAXOWNERS 8

struct dpf_filter
{
  bool used;
  struct cap guard;
  envid_t owners[DPF_MAXOWNERS];
  int ring;
  struct dpf_node *leaf;
  unsigned natoms;
  uint32_t matches;
};

static struct dpf_node nodes[DPF_MAXNODES];
static struct dpf_node root = {.filter = -1,.used = true };
static struct dpf_filter filters[DPF_MAXFILTERS];

/*
 * Packet rings.
 */
struct pr_ent
{
  pfn_t flagpfn;
  uint16_t flagoff;
  pfn_t bufpfn;
  uint16_t bufoff;
  uint32_t size;
};

struct pktring
{
  bool used;
  envid_t owner;
  unsigned n, cur;
  struct pr_ent ent[PKTRING_MAXENTS];
};

static struct pktring rings[PKTRING_MAX];
static uint8_t txbuf[NET_MAXFRAME + 64];

/*
 * Devices.
 */
int
net_register (struct netdev *d)
{
  volatile struct netinfo *ni;

  if (nnetdevs >= NET_MAXDEV)
    return -E_FULL;
  d->idx = nnetdevs;
  netdevs[nnetdevs++] = d;
  ni = &sysinfo->si_net[d->idx];
  ni->n_present = 1;
  memcpy ((void *) ni->n_mac, d->mac, 6);
  ni->n_mtu = d->mtu;
  ni->n_link = d->link;
  strlcpy ((char *) ni->n_name, d->name, sizeof (ni->n_name));
  sysinfo->si_nnet = nnetdevs;
  kprintf ("net%u: %s, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", d->idx, d->name,
	   d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
  return 0;
}

void
net_link (struct netdev *d, bool up)
{
  d->link = up;
  sysinfo->si_net[d->idx].n_link = up;
}

static void
net_poll_all (void *arg)
{
  for (unsigned i = 0; i < nnetdevs; i++)
    if (netdevs[i]->ops->poll)
      netdevs[i]->ops->poll (netdevs[i]);
}

void
net_init (void)
{
  dev_register_poll (net_poll_all, NULL);
}

/*
 * Filter evaluation.
 */
static bool
atom_eval (const struct dpf_atom *a, const uint8_t * pkt, unsigned len,
	   unsigned base, unsigned *nbase)
{
  unsigned off = base + a->a_off;
  uint32_t v;

  if (off + a->a_size > len)
    return false;
  switch (a->a_size)
    {
    case 1:
      v = pkt[off];
      break;
    case 2:
      v = ((uint32_t) pkt[off] << 8) | pkt[off + 1];
      break;
    case 4:
      v = ((uint32_t) pkt[off] << 24) | ((uint32_t) pkt[off + 1] << 16)
	| ((uint32_t) pkt[off + 2] << 8) | pkt[off + 3];
      break;
    default:
      return false;
    }
  *nbase = base;
  if (a->a_op == DPF_EQ)
    return (v & a->a_mask) == a->a_val;
  if (a->a_op == DPF_SHIFT)
    {
      *nbase = base + ((v & a->a_mask) << (a->a_val & 31));
      return *nbase < len;
    }
  return false;
}

static int
dpf_match (struct dpf_node *n, const uint8_t * pkt, unsigned len,
	   unsigned base, unsigned depth, unsigned *bestdepth)
{
  int best = -1;

  if (n->filter >= 0 && filters[n->filter].ring >= 0)
    {
      best = n->filter;
      *bestdepth = depth;
    }
  for (struct dpf_node *c = n->child; c; c = c->sibling)
    {
      unsigned nb, d = 0;
      int f;
      if (!atom_eval (&c->atom, pkt, len, base, &nb))
	continue;
      f = dpf_match (c, pkt, len, nb, depth + 1, &d);
      if (f >= 0 && (best < 0 || d > *bestdepth))
	{
	  best = f;
	  *bestdepth = d;
	}
    }
  return best;
}

static bool
atom_eq (const struct dpf_atom *a, const struct dpf_atom *b)
{
  return a->a_op == b->a_op && a->a_size == b->a_size && a->a_off == b->a_off
    && a->a_mask == b->a_mask && a->a_val == b->a_val;
}

static struct dpf_node *
node_alloc (void)
{
  for (unsigned i = 0; i < DPF_MAXNODES; i++)
    if (!nodes[i].used)
      {
	memset (&nodes[i], 0, sizeof (nodes[i]));
	nodes[i].used = true;
	nodes[i].filter = -1;
	return &nodes[i];
      }
  return NULL;
}

/* Remove N and its now useless ancestors. */
static void
node_prune (struct dpf_node *n)
{
  while (n != &root && n->filter < 0 && n->child == NULL)
    {
      struct dpf_node *p = n->parent, **pp = &p->child;
      while (*pp != n)
	pp = &(*pp)->sibling;
      *pp = n->sibling;
      n->used = false;
      n = p;
    }
}

static void
filter_free (int fid)
{
  struct dpf_filter *f = &filters[fid];
  struct dpf_node *leaf = f->leaf;

  leaf->filter = -1;
  node_prune (leaf);
  memset (f, 0, sizeof (*f));
}

/*
 * Delivery.
 */
static bool
ring_deliver (struct pktring *r, const void *frame, unsigned len)
{
  struct pr_ent *e = &r->ent[r->cur];
  uint8_t *fp, *bp;
  uint32_t *flag;
  bool ok = false;

  fp = pfn_get (e->flagpfn);
  flag = (uint32_t *) (fp + e->flagoff);
  if (__atomic_load_n (flag, __ATOMIC_ACQUIRE) == 0)
    {
      bp = pfn_get (e->bufpfn);
      memcpy (bp + e->bufoff, frame, MIN (len, e->size));
      pfn_put (e->bufpfn, bp);
      __atomic_store_n (flag, len, __ATOMIC_RELEASE);
      r->cur = (r->cur + 1) % r->n;
      ok = true;
    }
  pfn_put (e->flagpfn, fp);
  return ok;
}

void
net_rx (struct netdev *d, const void *frame, unsigned len)
{
  unsigned depth = 0;
  int fid;

  sysinfo->si_net[d->idx].n_rxpkts++;
  fid = dpf_match (&root, frame, len, 0, 0, &depth);
  if (fid < 0 || filters[fid].ring < 0 || !rings[filters[fid].ring].used)
    {
      sysinfo->si_net[d->idx].n_rxdrop++;
      return;
    }
  filters[fid].matches++;
  if (!ring_deliver (&rings[filters[fid].ring], frame, len))
    sysinfo->si_net[d->idx].n_rxdrop++;
  else
    sched_kick_idle ();
}

/*
 * System calls.
 */
static bool
filter_owned (struct dpf_filter *f, envid_t id)
{
  for (unsigned i = 0; i < DPF_MAXOWNERS; i++)
    if (f->owners[i] == id)
      return true;
  return false;
}

static int
ring_owned (struct env *e, unsigned ring)
{
  if (ring >= PKTRING_MAX || !rings[ring].used)
    return -E_NOT_FOUND;
  if (rings[ring].owner != e->id)
    return -E_CAP_INSUFF;
  return 0;
}

int
sys_dpf_insert (struct env *e, unsigned k, uaddr_t uatoms, unsigned n,
		unsigned ring)
{
  struct dpf_atom atoms[DPF_MAXATOMS];
  struct dpf_node *node = &root, *created = NULL;
  struct cap *c = env_cap (e, k);
  int fid = -1, r;

  if (c == NULL)
    return -E_CAP_INVALID;
  if (n == 0 || n > DPF_MAXATOMS)
    return -E_INVAL;
  if ((r = ring_owned (e, ring)) < 0)
    return r;
  if (copyin (atoms, uatoms, n * sizeof (atoms[0])) < 0)
    return -E_FAULT;
  for (unsigned i = 0; i < n; i++)
    if ((atoms[i].a_op != DPF_EQ && atoms[i].a_op != DPF_SHIFT)
	|| (atoms[i].a_size != 1 && atoms[i].a_size != 2
	    && atoms[i].a_size != 4) || atoms[i].a_off > NET_MAXFRAME)
      return -E_INVAL;

  for (unsigned i = 0; i < DPF_MAXFILTERS; i++)
    if (!filters[i].used)
      {
	fid = i;
	break;
      }
  if (fid < 0)
    return -E_FULL;

  /* Walk the trie; less specific filters on the path would lose
     packets to the new one. */
  for (unsigned i = 0; i < n; i++)
    {
      struct dpf_node *ch;

      if (node->filter >= 0
	  && !cap_grants (c, &filters[node->filter].guard, CAP_W))
	goto conflict;
      for (ch = node->child; ch; ch = ch->sibling)
	if (atom_eq (&ch->atom, &atoms[i]))
	  break;
      if (ch == NULL)
	{
	  ch = node_alloc ();
	  if (ch == NULL)
	    {
	      if (created)
		node_prune (created);
	      return -E_NO_MEM;
	    }
	  ch->atom = atoms[i];
	  ch->parent = node;
	  ch->sibling = node->child;
	  node->child = ch;
	  if (created == NULL)
	    created = ch;
	}
      node = ch;
    }
  if (node->filter >= 0)
    {
      /* Identical filter: share it, given the capability. */
      struct dpf_filter *g = &filters[node->filter];
      if (!cap_grants (c, &g->guard, CAP_W))
	goto conflict;
      for (unsigned i = 0; i < DPF_MAXOWNERS; i++)
	if (g->owners[i] == 0 || g->owners[i] == e->id)
	  {
	    g->owners[i] = e->id;
	    g->ring = ring;
	    return node->filter;
	  }
      return -E_FULL;
    }

  memset (&filters[fid], 0, sizeof (filters[fid]));
  filters[fid].used = true;
  memcpy (&filters[fid].guard, c, sizeof (*c));
  filters[fid].owners[0] = e->id;
  filters[fid].ring = ring;
  filters[fid].leaf = node;
  filters[fid].natoms = n;
  node->filter = fid;
  return fid;

conflict:
  if (created)
    {
      /* Remove the nodes we added (the last one is a leaf). */
      node_prune (node);
    }
  return -E_CONFLICT;
}

int
sys_dpf_delete (struct env *e, unsigned k, unsigned fid)
{
  struct dpf_filter *f;
  int r;

  if (fid >= DPF_MAXFILTERS || !filters[fid].used)
    return -E_NOT_FOUND;
  f = &filters[fid];
  if (!filter_owned (f, e->id))
    {
      r = cap_check (e, k, &f->guard, CAP_W);
      if (r < 0)
	return r;
    }
  filter_free (fid);
  return 0;
}

int
sys_dpf_ref (struct env *e, unsigned k, unsigned fid, unsigned ke,
	     envid_t id)
{
  struct dpf_filter *f;
  struct env *t;
  int r;

  if (fid >= DPF_MAXFILTERS || !filters[fid].used)
    return -E_NOT_FOUND;
  f = &filters[fid];
  if ((r = cap_check (e, k, &f->guard, CAP_W)) < 0)
    return r;
  if ((r = env_control (e, ke, id, &t)) < 0)
    return r;
  for (unsigned i = 0; i < DPF_MAXOWNERS; i++)
    if (f->owners[i] == 0 || f->owners[i] == t->id)
      {
	f->owners[i] = t->id;
	return 0;
      }
  return -E_FULL;
}

int
sys_dpf_pktring (struct env *e, unsigned k, unsigned fid, unsigned ring)
{
  struct dpf_filter *f;
  int r;

  if (fid >= DPF_MAXFILTERS || !filters[fid].used)
    return -E_NOT_FOUND;
  f = &filters[fid];
  if ((r = cap_check (e, k, &f->guard, CAP_W)) < 0)
    return r;
  if ((r = ring_owned (e, ring)) < 0)
    return r;
  f->ring = ring;
  return 0;
}

static void
ring_release (struct pktring *r)
{
  for (unsigned i = 0; i < r->n; i++)
    {
      pmem_unpin (r->ent[i].flagpfn);
      pmem_unpin (r->ent[i].bufpfn);
    }
  r->n = 0;
}

static int
ring_entry (struct env *e, const struct pktring_ent *u, struct pr_ent *k)
{
  xpte_t pf, pb;

  if ((u->pr_flag & 3) || u->pr_size == 0
      || (u->pr_buf & PAGE_MASK) + u->pr_size > PAGE_SIZE
      || u->pr_flag >= UXOK_BASE || u->pr_buf >= UXOK_BASE)
    return -E_INVAL;
  pf = env_getpte (e, u->pr_flag & ~(uaddr_t) PAGE_MASK);
  pb = env_getpte (e, u->pr_buf & ~(uaddr_t) PAGE_MASK);
  if (!(pf & PTE_P) || !(pf & PTE_W) || !(pb & PTE_P) || !(pb & PTE_W))
    return -E_FAULT;
  if (ppinfo[PTE_PPN (pf)].pp_state != PP_USER
      || ppinfo[PTE_PPN (pb)].pp_state != PP_USER)
    return -E_INVAL;
  k->flagpfn = PTE_PPN (pf);
  k->flagoff = u->pr_flag & PAGE_MASK;
  k->bufpfn = PTE_PPN (pb);
  k->bufoff = u->pr_buf & PAGE_MASK;
  k->size = u->pr_size;
  pmem_pin (k->flagpfn);
  pmem_pin (k->bufpfn);
  return 0;
}

int
sys_pktring_setring (struct env *e, uaddr_t uents, unsigned n)
{
  struct pktring_ent ue;
  struct pktring *r = NULL;
  int id = -1, err;

  if (n == 0 || n > PKTRING_MAXENTS)
    return -E_INVAL;
  for (unsigned i = 0; i < PKTRING_MAX; i++)
    if (!rings[i].used)
      {
	r = &rings[i];
	id = i;
	break;
      }
  if (r == NULL)
    return -E_FULL;
  memset (r, 0, sizeof (*r));
  for (unsigned i = 0; i < n; i++)
    {
      if (copyin (&ue, uents + i * sizeof (ue), sizeof (ue)) < 0)
	err = -E_FAULT;
      else
	err = ring_entry (e, &ue, &r->ent[i]);
      if (err < 0)
	{
	  r->n = i;
	  ring_release (r);
	  return err;
	}
    }
  r->n = n;
  r->cur = 0;
  r->owner = e->id;
  r->used = true;
  return id;
}

int
sys_pktring_modring (struct env *e, unsigned ring, unsigned idx, uaddr_t uent)
{
  struct pktring_ent ue;
  struct pr_ent ne;
  int r;

  if ((r = ring_owned (e, ring)) < 0)
    return r;
  if (idx >= rings[ring].n)
    return -E_INVAL;
  if (copyin (&ue, uent, sizeof (ue)) < 0)
    return -E_FAULT;
  if ((r = ring_entry (e, &ue, &ne)) < 0)
    return r;
  pmem_unpin (rings[ring].ent[idx].flagpfn);
  pmem_unpin (rings[ring].ent[idx].bufpfn);
  rings[ring].ent[idx] = ne;
  return 0;
}

int
sys_pktring_delring (struct env *e, unsigned ring)
{
  int r;

  if ((r = ring_owned (e, ring)) < 0)
    return r;
  for (unsigned i = 0; i < DPF_MAXFILTERS; i++)
    if (filters[i].used && filters[i].ring == (int) ring)
      filters[i].ring = -1;
  ring_release (&rings[ring]);
  rings[ring].used = false;
  return 0;
}

int
sys_net_xmit (struct env *e, unsigned card, uaddr_t urecs, unsigned n,
	      uaddr_t notify)
{
  struct sendrec recs[NET_MAXSENDRECS];
  struct netdev *d;
  unsigned total = 0;
  int r;

  if (card >= nnetdevs)
    return -E_NO_DEV;
  d = netdevs[card];
  if (n == 0 || n > NET_MAXSENDRECS)
    return -E_INVAL;
  if (copyin (recs, urecs, n * sizeof (recs[0])) < 0)
    return -E_FAULT;
  for (unsigned i = 0; i < n; i++)
    {
      if (recs[i].sr_len > NET_MAXFRAME - total)
	return -E_INVAL;
      if (copyin (txbuf + total, recs[i].sr_va, recs[i].sr_len) < 0)
	return -E_FAULT;
      total += recs[i].sr_len;
    }
  if (total < 14)
    return -E_INVAL;
  if (total < 60)
    {
      memset (txbuf + total, 0, 60 - total);
      total = 60;
    }
  if (memcmp (txbuf, d->mac, 6) == 0)
    {
      /* Addressed to ourselves: loop back through the filters. */
      net_rx (d, txbuf, total);
      r = 0;
    }
  else
    r = d->ops->xmit (d, txbuf, total);
  if (r < 0)
    return r;
  sysinfo->si_net[card].n_txpkts++;
  /* The data was copied: the buffers can be reused right away. */
  if (notify)
    {
      uint32_t v;
      if (copyin (&v, notify, 4) == 0)
	{
	  v--;
	  copyout (notify, &v, 4);
	}
    }
  return 0;
}

void
net_env_freed (struct env *e)
{
  for (unsigned i = 0; i < DPF_MAXFILTERS; i++)
    {
      struct dpf_filter *f = &filters[i];
      bool any = false;

      if (!f->used)
	continue;
      for (unsigned k = 0; k < DPF_MAXOWNERS; k++)
	{
	  if (f->owners[k] == e->id)
	    f->owners[k] = 0;
	  if (f->owners[k])
	    any = true;
	}
      if (!any)
	filter_free (i);
    }
  for (unsigned i = 0; i < PKTRING_MAX; i++)
    if (rings[i].used && rings[i].owner == e->id)
      {
	for (unsigned k = 0; k < DPF_MAXFILTERS; k++)
	  if (filters[k].used && filters[k].ring == (int) i)
	    filters[k].ring = -1;
	ring_release (&rings[i]);
	rings[i].used = false;
      }
}
