/*
 * ExOS XN helpers: bringing blocks in core, mapping them, writing them
 * back in an order that satisfies XN's rules.
 *
 * Cached blocks are mapped in the buffer cache window at a fixed
 * address per registry entry (BCWIN + index * 4K).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <exos/exos.h>
#include <exos/fs.h>
#include <string.h>
#include <errno.h>

/* Window state: block mapped at each registry index (+1), and mode. */
static uint32_t winblk[XN_NBC];
static uint8_t winrw[XN_NBC];
static unsigned nmapped;

/* Parent of each block we know about (to re-bind evicted blocks). */
#define PHASH 4096
static struct pent
{
  uint32_t blk, parent;
} pmap[PHASH];

static unsigned
ph (uint32_t blk)
{
  return (blk * 2654435761u) % PHASH;
}

void
xnl_set_parent (uint32_t blk, uint32_t parent)
{
  unsigned h = ph (blk);
  for (unsigned i = 0; i < PHASH; i++, h = (h + 1) % PHASH)
    if (pmap[h].blk == 0 || pmap[h].blk == blk)
      {
	pmap[h].blk = blk;
	pmap[h].parent = parent;
	return;
      }
}

uint32_t
xnl_parent (uint32_t blk)
{
  unsigned h = ph (blk);
  for (unsigned i = 0; i < PHASH; i++, h = (h + 1) % PHASH)
    {
      if (pmap[h].blk == blk)
	return pmap[h].parent;
      if (pmap[h].blk == 0)
	break;
    }
  return 0;
}

static int
xerr (int r)
{
  switch (-r)
    {
    case E_NO_MEM:
      return -ENOMEM;
    case E_ACCESS:
    case E_CAP_INSUFF:
    case E_CAP_INVALID:
      return -EACCES;
    case E_NOT_FREE:
    case E_FULL:
      return -ENOSPC;
    case E_BUSY:
      return -EBUSY;
    case E_NOT_FOUND:
      return -ENOENT;
    default:
      return -EIO;
    }
}

/*
 * Wait until registry entry IDX has no I/O in progress.
 */
int
xnl_wait_io (int idx)
{
  volatile struct bc_entry *e = &xn_registry[idx];

  exos_sleep_until_mem (&e->bc_state, WK_ANDZ, BC_IN_TRANSIT, 0);
  return (e->bc_state & BC_ERROR) ? -EIO : 0;
}

/*
 * Make BLK (child of PARENT, or XN_NOPARENT for a root block) bound and
 * valid in the registry.  Returns the registry index.
 */
int
xnl_get (unsigned dev, uint32_t blk, uint32_t parent)
{
  int idx, r;

  idx = sys_xn_lookup (dev, blk);
  if (idx >= 0 && (xn_registry[idx].bc_state & BC_VALID)
      && xn_registry[idx].bc_type != XN_TYPE_UNKNOWN)
    return idx;
  if (parent == 0)
    parent = xnl_parent (blk);
  if (parent == 0)
    return -EIO;
  xnl_set_parent (blk, parent);
  if (parent != XN_NOPARENT)
    {
      r = xnl_get (dev, parent, 0);
      if (r < 0)
	return r;
    }
  for (int tries = 0;; tries++)
    {
      idx = sys_xn_bind (dev, blk, parent);
      if (idx >= 0)
	break;
      if (idx == -E_NO_MEM && tries < 3)
	{
	  xnl_unmap_all ();
	  continue;
	}
      return xerr (idx);
    }
  if (!(xn_registry[idx].bc_state & BC_VALID))
    {
      if (!(xn_registry[idx].bc_state & BC_IN_TRANSIT))
	{
	  r = sys_xn_readin (dev, blk, 1);
	  if (r < 0)
	    return xerr (r);
	}
      r = xnl_wait_io (idx);
      if (r < 0)
	return r;
      if (!(xn_registry[idx].bc_state & BC_VALID))
	return -EIO;
    }
  return idx;
}

void
xnl_unmap_all (void)
{
  for (int i = 0; i < XN_NBC; i++)
    if (winblk[i])
      {
	sys_self_insert_pte (EXOS_CAP, 0, BCWIN + i * PGSIZE);
	winblk[i] = 0;
	winrw[i] = 0;
      }
  nmapped = 0;
}

/*
 * Map BLK (which must be valid in core) in the window.
 */
void *
xnl_map (unsigned dev, uint32_t blk, int writable)
{
  int idx = sys_xn_lookup (dev, blk), r;
  uintptr_t va;

  if (idx < 0)
    {
      idx = xnl_get (dev, blk, 0);
      if (idx < 0)
	{
	  errno = -idx;
	  return NULL;
	}
    }
  va = BCWIN + idx * PGSIZE;
  if (winblk[idx] == blk + 1 && (!writable || winrw[idx])
      && exos_mapped (va) && PTE_PPN (exos_pte (va)) == xn_registry[idx].bc_ppn)
    {
      if (writable)
	{
	  /* Re-assert the mapping so XN marks the block dirty. */
	  sys_xn_insert_pte (EXOS_CAP, dev, blk, va, 1, 0, 0);
	}
      return (void *) va;
    }
  if (nmapped > 2048)
    xnl_unmap_all ();
  r = sys_xn_insert_pte (EXOS_CAP, dev, blk, va, writable, 0, 0);
  if (r < 0)
    {
      errno = -xerr (r);
      return NULL;
    }
  if (!winblk[idx])
    nmapped++;
  winblk[idx] = blk + 1;
  winrw[idx] = writable;
  return (void *) va;
}

/* Remove our window mapping of BLK, if any (XN refuses to free mapped
   blocks). */
void
xnl_unmap (unsigned dev, uint32_t blk)
{
  int idx = sys_xn_lookup (dev, blk);

  if (idx >= 0 && winblk[idx] == blk + 1)
    {
      sys_self_insert_pte (EXOS_CAP, 0, BCWIN + idx * PGSIZE);
      winblk[idx] = 0;
      winrw[idx] = 0;
      nmapped--;
    }
}

void
xnl_fork_child (void)
{
  memset (winblk, 0, sizeof (winblk));
  memset (winrw, 0, sizeof (winrw));
  nmapped = 0;
}

int
xnl_writeback (unsigned dev, uint32_t blk, int wait)
{
  int idx = sys_xn_lookup (dev, blk), r;

  if (idx < 0)
    return 0;
  if (xn_registry[idx].bc_state & BC_IN_TRANSIT)
    xnl_wait_io (idx);
  r = sys_xn_writeback (dev, blk, 1);
  if (r < 0)
    return xerr (r);
  if (wait)
    return xnl_wait_io (idx);
  return 0;
}

/*
 * Write back every dirty block of DEV, children before parents (XN
 * refuses to write a block that points to an uninitialised one).
 */
int
xnl_sync (unsigned dev)
{
  for (int round = 0; round < 64; round++)
    {
      int pending = 0, started = 0, last = -1;

      for (int i = 0; i < XN_NBC; i++)
	{
	  volatile struct bc_entry *e = &xn_registry[i];
	  uint32_t st = e->bc_state;

	  if (!(st & BC_USED) || e->bc_dev != dev)
	    continue;
	  if (!(st & (BC_DIRTY | BC_UNINIT)))
	    continue;
	  pending++;
	  if (st & BC_IN_TRANSIT)
	    {
	      last = i;
	      continue;
	    }
	  if (e->bc_taint)
	    continue;
	  if (sys_xn_writeback (dev, e->bc_blk, 1) > 0)
	    {
	      started++;
	      last = i;
	    }
	}
      if (pending == 0)
	return 0;
      if (last >= 0)
	xnl_wait_io (last);
      else if (!started)
	break;
      /* Wait for all the I/O of this round. */
      for (int i = 0; i < XN_NBC; i++)
	{
	  volatile struct bc_entry *e = &xn_registry[i];
	  if ((e->bc_state & BC_USED) && e->bc_dev == dev
	      && (e->bc_state & BC_IN_TRANSIT))
	    xnl_wait_io (i);
	}
    }
  return -EIO;
}

/*
 * Choose a free block near HINT.
 */
int
xnl_alloc_block (unsigned dev, uint32_t hint)
{
  volatile struct xn_super *s = xn_cat_super (dev);
  volatile uint32_t *fm = xn_freemap (dev);
  uint32_t n = s->s_nblocks, start = s->s_data_start;

  if (hint < start || hint >= n)
    hint = start;
  for (uint32_t k = 0; k < n; k++)
    {
      uint32_t b = hint + k;
      if (b >= n)
	b = start + (b - n);
      if (b < start)
	continue;
      if ((fm[b / 32] >> (b % 32)) & 1)
	{
	  if (sys_xn_lookup (dev, b) >= 0)
	    continue;
	  return b;
	}
    }
  return -ENOSPC;
}
