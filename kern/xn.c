/*
 * XN: protected, extensible stable storage (see <xok/xn.h>).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "xn.h"
#include "blk.h"
#include "dev.h"

#define SECT_PER_BLK (XN_BLKSIZE / SECTOR_SIZE)
#define XN_NHASH 4096
#define XN_NPENDFREE 4096
#define CAT_TYPES_OFF 4096
#define CAT_ROOTS_OFF (4096 * (1 + XN_MAXTYPES))
#define CAT_SIZE (4096 * (2 + XN_MAXTYPES))

struct xndev
{
  bool mounted;
  struct blkdev *bd;
  unsigned dev;
  struct xn_super super;
  uint8_t *cat;			/* Exported: super copy, types, roots. */
  struct xn_template *types;
  struct xn_root *roots;
  uint32_t *freemap;		/* Exported. */
  size_t freemap_size;
  bool clean_on_disk;
};

static struct xndev xndevs[XOK_MAXDISK];

/* Registry. */
static volatile struct bc_entry *bc;
static uint16_t bc_rootidx[XN_NBC];
static uint32_t bc_next[XN_NBC];
static uint32_t bc_hash[XN_NHASH];
static uint32_t bc_freelist;

/* Blocks freed, waiting for their parent to reach the disk. */
static struct
{
  uint16_t used, dev;
  uint32_t blk, parent;
} pendfree[XN_NPENDFREE];

static pfn_t zero_page;
static uint8_t xn_tmp[XN_BLKSIZE] __attribute__ ((aligned (16)));
static struct xn_item set_old[UDF_MAXEMIT], set_new[UDF_MAXEMIT];

#define BCNIL 0xffffffffu

/*
 * Registry management.
 */
static unsigned
bc_hashfn (unsigned dev, uint32_t blk)
{
  return ((blk * 2654435761u) ^ (dev * 40503u)) % XN_NHASH;
}

static int
bc_lookup (unsigned dev, uint32_t blk)
{
  uint32_t i = bc_hash[bc_hashfn (dev, blk)];

  while (i != BCNIL)
    {
      if (bc[i].bc_dev == dev && bc[i].bc_blk == blk)
	return i;
      i = bc_next[i];
    }
  return -1;
}

static int
bc_new (unsigned dev, uint32_t blk)
{
  uint32_t i = bc_freelist, h;

  if (i == BCNIL)
    return -E_NO_MEM;
  bc_freelist = bc_next[i];
  memset ((void *) &bc[i], 0, sizeof (struct bc_entry));
  bc[i].bc_dev = dev;
  bc[i].bc_blk = blk;
  bc[i].bc_type = XN_TYPE_UNKNOWN;
  bc[i].bc_parent = XN_NOPARENT;
  bc[i].bc_state = BC_USED;
  bc[i].bc_lastuse = sysinfo->si_ticks;
  bc_rootidx[i] = 0xffff;
  h = bc_hashfn (dev, blk);
  bc_next[i] = bc_hash[h];
  bc_hash[h] = i;
  return i;
}

static void
bc_drop_page (int i)
{
  pfn_t pfn = bc[i].bc_ppn;

  if (pfn == 0)
    return;
  KASSERT (ppinfo[pfn].pp_state == PP_BC);
  KASSERT (ppinfo[pfn].pp_refcnt == 0);
  ppinfo[pfn].pp_bc = 0;
  ppinfo[pfn].pp_state = PP_KERNEL;
  pmem_free (pfn);
  bc[i].bc_ppn = 0;
  bc[i].bc_state &= ~BC_VALID;
}

static void
bc_release (int i)
{
  uint32_t *pp = &bc_hash[bc_hashfn (bc[i].bc_dev, bc[i].bc_blk)];

  bc_drop_page (i);
  while (*pp != (uint32_t) i)
    {
      KASSERT (*pp != BCNIL);
      pp = &bc_next[*pp];
    }
  *pp = bc_next[i];
  memset ((void *) &bc[i], 0, sizeof (struct bc_entry));
  bc_next[i] = bc_freelist;
  bc_freelist = i;
}

static bool
bc_releasable (int i)
{
  uint32_t st = bc[i].bc_state;

  if (st & (BC_IN_TRANSIT | BC_DIRTY | BC_UNINIT | BC_LOCKED))
    return false;
  if (bc[i].bc_taint)
    return false;
  if (bc[i].bc_ppn && ppinfo[bc[i].bc_ppn].pp_refcnt)
    return false;
  if (bc[i].bc_ppn && ppinfo[bc[i].bc_ppn].pp_pinned)
    return false;
  return true;
}

/*
 * Free up to N clean, unused buffers, least recently used first.
 */
unsigned
xn_reclaim (unsigned n)
{
  unsigned freed = 0;

  while (freed < n)
    {
      int best = -1;
      for (int i = 0; i < XN_NBC; i++)
	if ((bc[i].bc_state & BC_USED) && bc[i].bc_ppn && bc_releasable (i)
	    && (best < 0 || bc[i].bc_lastuse < bc[best].bc_lastuse))
	  best = i;
      if (best < 0)
	break;
      bc_release (best);
      freed++;
    }
  return freed;
}

static pfn_t
bc_page_alloc (int i)
{
  pfn_t pfn = pmem_alloc (0);

  if (pfn == PFN_INVALID && xn_reclaim (8))
    pfn = pmem_alloc (0);
  if (pfn == PFN_INVALID)
    return PFN_INVALID;
  ppinfo[pfn].pp_state = PP_BC;
  ppinfo[pfn].pp_bc = i + 1;
  ppinfo[pfn].pp_owner = 0;
  bc[i].bc_ppn = pfn;
  return pfn;
}

void
xn_page_unref (pfn_t pfn)
{
  uint32_t i = ppinfo[pfn].pp_bc;

  if (i && bc[i - 1].bc_refcnt)
    bc[i - 1].bc_refcnt--;
}

/*
 * Free map.
 */
static inline bool
fm_isfree (struct xndev *x, uint32_t b)
{
  return (x->freemap[b / 32] >> (b % 32)) & 1;
}

static inline void
fm_set (struct xndev *x, uint32_t b, bool free)
{
  if (free)
    x->freemap[b / 32] |= 1u << (b % 32);
  else
    x->freemap[b / 32] &= ~(1u << (b % 32));
}

static bool
blk_in_data (struct xndev *x, uint32_t b)
{
  return b >= x->super.s_data_start && b < x->super.s_nblocks;
}

/*
 * Disk I/O helpers.
 */
static int
xn_sync_rw (struct xndev *x, uint32_t blk, void *buf, bool write)
{
  return blk_rw_sync (x->bd, (uint64_t) blk * SECT_PER_BLK, SECT_PER_BLK,
		      buf, write);
}

static uint8_t superbuf[XN_BLKSIZE];

static int
xn_write_super (struct xndev *x)
{
  memset (superbuf, 0, sizeof (superbuf));
  memcpy (superbuf, &x->super, sizeof (x->super));
  memcpy (x->cat, &x->super, sizeof (x->super));
  return xn_sync_rw (x, 0, superbuf, true);
}

static void
mark_unclean (struct xndev *x)
{
  if (!x->clean_on_disk)
    return;
  x->super.s_clean = 0;
  xn_write_super (x);
  x->clean_on_disk = false;
}

/*
 * Owns sets.
 */
static void
items_sort (struct xn_item *a, unsigned n)
{
  for (unsigned gap = n / 2; gap > 0; gap /= 2)
    for (unsigned i = gap; i < n; i++)
      {
	struct xn_item t = a[i];
	unsigned j = i;
	while (j >= gap && a[j - gap].blk > t.blk)
	  {
	    a[j] = a[j - gap];
	    j -= gap;
	  }
	a[j] = t;
      }
}

static int
owns_set (struct xndev *x, unsigned type, const uint8_t * meta,
	  struct xn_item *out, unsigned *n)
{
  struct xn_template *t;
  struct udf_ctx c;
  uint32_t ret;

  *n = 0;
  if (type >= XN_MAXTYPES || x->types[type].t_name[0] == 0)
    return -E_INVAL;
  t = &x->types[type];
  if (t->t_nowns == 0)
    return 0;
  memset (&c, 0, sizeof (c));
  c.meta = meta;
  c.out = out;
  c.maxout = UDF_MAXEMIT;
  if (udf_run (t->t_owns, t->t_nowns, &c, &ret) < 0)
    return -E_UDF;
  for (unsigned i = 0; i < c.nout; i++)
    if (!blk_in_data (x, out[i].blk)
	|| x->types[out[i].type].t_name[0] == 0)
      return -E_UDF;
  items_sort (out, c.nout);
  for (unsigned i = 1; i < c.nout; i++)
    if (out[i].blk == out[i - 1].blk)
      return -E_UDF;
  *n = c.nout;
  return 0;
}

static bool
is_meta (struct xndev *x, unsigned type)
{
  return type < XN_MAXTYPES && !(x->types[type].t_flags & XN_TF_DATA);
}

/*
 * Access control: run acl-uf at block I, deferring to parents.
 */
static int
acl_check (struct xndev *x, int i, const struct cap *cred, uint32_t op,
	   uint32_t off, uint32_t len, uint32_t child, const uint8_t * newmeta)
{
  for (int depth = 0; depth < 32; depth++)
    {
      struct xn_template *t;
      uint32_t res = XN_ACL_DEFER;

      if (!(bc[i].bc_state & BC_VALID) || bc[i].bc_type == XN_TYPE_UNKNOWN)
	return -E_NOT_INCORE;
      t = &x->types[bc[i].bc_type];
      if (t->t_nacl)
	{
	  struct udf_ctx c;
	  uint32_t args[XN_NARGS] = { op, off, len, child };
	  uint8_t *page = pfn_get (bc[i].bc_ppn);
	  int r;

	  memset (&c, 0, sizeof (c));
	  c.meta = page;
	  c.newmeta = newmeta ? newmeta : page;
	  c.args = args;
	  c.cred = cred;
	  r = udf_run (t->t_acl, t->t_nacl, &c, &res);
	  pfn_put (bc[i].bc_ppn, page);
	  if (r < 0)
	    return -E_ACCESS;
	}
      if (res == XN_ACL_ALLOW)
	return 0;
      if (res != XN_ACL_DEFER)
	return -E_ACCESS;

      /* Defer to the parent. */
      if (bc[i].bc_state & BC_ROOT)
	{
	  struct xn_root *root = &x->roots[bc_rootidx[i]];
	  unsigned perm = op == XN_OP_READ ? CAP_R : CAP_W;
	  return cap_grants (cred, &root->r_guard, perm) ? 0 : -E_ACCESS;
	}
      child = bc[i].bc_blk;
      i = bc_lookup (x->dev, bc[i].bc_parent);
      if (i < 0)
	return -E_NOT_INCORE;
      op = op == XN_OP_READ ? XN_OP_READ : XN_OP_WRITE;
      off = len = 0;
      newmeta = NULL;
    }
  return -E_ACCESS;
}

static struct xndev *
xn_get (unsigned dev)
{
  if (dev >= XOK_MAXDISK || !xndevs[dev].mounted)
    return NULL;
  return &xndevs[dev];
}

/*
 * I/O completions.
 */
static void
pendfree_parent_written (struct xndev *x, uint32_t parent)
{
  for (unsigned i = 0; i < XN_NPENDFREE; i++)
    if (pendfree[i].used && pendfree[i].dev == x->dev
	&& pendfree[i].parent == parent)
      {
	fm_set (x, pendfree[i].blk, true);
	pendfree[i].used = 0;
      }
}

static void
read_done (struct bio *b)
{
  int i = (int) (uintptr_t) b->arg;

  bc[i].bc_state &= ~BC_IN_TRANSIT;
  if (b->error)
    bc[i].bc_state |= BC_ERROR;
  else
    bc[i].bc_state = (bc[i].bc_state | BC_VALID) & ~BC_ERROR;
  bio_free (b);
}

static void
write_done (struct bio *b)
{
  int i = (int) (uintptr_t) b->arg;
  struct xndev *x = &xndevs[bc[i].bc_dev];

  bc[i].bc_state &= ~BC_IN_TRANSIT;
  if (b->error)
    {
      bc[i].bc_state |= BC_ERROR;
      bio_free (b);
      return;
    }
  bc[i].bc_state &= ~(BC_DIRTY | BC_ERROR);
  if (bc[i].bc_state & BC_UNINIT)
    {
      int p = bc_lookup (bc[i].bc_dev, bc[i].bc_parent);
      bc[i].bc_state &= ~BC_UNINIT;
      if (p >= 0 && bc[p].bc_taint)
	bc[p].bc_taint--;
    }
  pendfree_parent_written (x, bc[i].bc_blk);
  bio_free (b);
}

static int
start_io (struct xndev *x, int i, bool write)
{
  struct bio *b = bio_alloc ();

  if (b == NULL)
    return -E_NO_MEM;
  b->sector = (uint64_t) bc[i].bc_blk * SECT_PER_BLK;
  b->nsect = SECT_PER_BLK;
  b->write = write;
  bio_add_page (b, bc[i].bc_ppn, 0, XN_BLKSIZE);
  b->done = write ? write_done : read_done;
  b->arg = (void *) (uintptr_t) i;
  bc[i].bc_state |= BC_IN_TRANSIT;
  blk_submit (x->bd, b);
  return 0;
}

/*
 * Mount, format, recovery.
 */
static int
find_root (struct xndev *x, uint32_t blk)
{
  for (int r = 0; r < XN_MAXROOTS; r++)
    if ((x->roots[r].r_flags & XN_RF_USED) && blk >= x->roots[r].r_blk
	&& blk < x->roots[r].r_blk + x->roots[r].r_nblocks)
      return r;
  return -1;
}

static void
xn_rebuild_freemap (struct xndev *x)
{
  static uint32_t stack[4096];
  static uint16_t stype[4096];
  unsigned sp = 0, nmeta = 0;
  static uint8_t buf[XN_BLKSIZE];

  kprintf ("xn%u: rebuilding free map\n", x->dev);
  for (uint32_t b = 0; b < x->super.s_nblocks; b++)
    fm_set (x, b, b >= x->super.s_data_start);
  for (int r = 0; r < XN_MAXROOTS; r++)
    {
      struct xn_root *root = &x->roots[r];
      if (!(root->r_flags & XN_RF_USED))
	continue;
      for (uint32_t b = root->r_blk; b < root->r_blk + root->r_nblocks; b++)
	{
	  fm_set (x, b, false);
	  if (is_meta (x, root->r_type) && sp < 4096)
	    {
	      stack[sp] = b;
	      stype[sp++] = root->r_type;
	    }
	}
    }
  while (sp > 0)
    {
      uint32_t b = stack[--sp];
      unsigned type = stype[sp], n;

      if (xn_sync_rw (x, b, buf, false) < 0)
	continue;
      nmeta++;
      if (owns_set (x, type, buf, set_old, &n) < 0)
	{
	  kprintf ("xn%u: owns-udf failed on block %u\n", x->dev, b);
	  continue;
	}
      for (unsigned k = 0; k < n; k++)
	{
	  if (!fm_isfree (x, set_old[k].blk))
	    continue;
	  fm_set (x, set_old[k].blk, false);
	  if (is_meta (x, set_old[k].type))
	    {
	      if (sp >= 4096)
		kpanic ("xn: metadata tree too deep");
	      stack[sp] = set_old[k].blk;
	      stype[sp++] = set_old[k].type;
	    }
	}
    }
  kprintf ("xn%u: %u metadata blocks scanned\n", x->dev, nmeta);
}

static int
xn_write_freemap (struct xndev *x)
{
  for (uint32_t i = 0; i < x->super.s_freemap_nblocks; i++)
    {
      int r = xn_sync_rw (x, x->super.s_freemap_start + i,
			  (uint8_t *) x->freemap + i * XN_BLKSIZE, true);
      if (r < 0)
	return r;
    }
  return 0;
}

static int
xn_alloc_areas (struct xndev *x)
{
  vaddr_t kva;

  if (x->cat == NULL)
    {
      x->cat = pmem_export_area (CAT_SIZE, &kva);
      x->types = (struct xn_template *) (x->cat + CAT_TYPES_OFF);
      x->roots = (struct xn_root *) (x->cat + CAT_ROOTS_OFF);
    }
  if (x->freemap == NULL)
    {
      x->freemap_size = x->super.s_freemap_nblocks * XN_BLKSIZE;
      if (x->freemap_size > 0x400000)
	return -E_RANGE;
      x->freemap = pmem_export_area (x->freemap_size, &kva);
    }
  return 0;
}

static int
xn_mount (unsigned dev)
{
  struct xndev *x = &xndevs[dev];
  struct blkdev *bd = blk_get (dev);
  static uint8_t buf[XN_BLKSIZE];

  x->bd = bd;
  x->dev = dev;
  if (blk_rw_sync (bd, 0, SECT_PER_BLK, buf, false) < 0)
    return -E_IO;
  memcpy (&x->super, buf, sizeof (x->super));
  if (x->super.s_magic != XN_MAGIC || x->super.s_version != XN_VERSION)
    return -E_NOT_FOUND;
  if ((uint64_t) x->super.s_nblocks * SECT_PER_BLK > bd->nsectors)
    {
      kprintf ("xn%u: file system larger than disk\n", dev);
      return -E_RANGE;
    }
  if (xn_alloc_areas (x) < 0)
    return -E_NO_MEM;
  for (unsigned t = 0; t < XN_MAXTYPES; t++)
    if (xn_sync_rw (x, x->super.s_typecat_start + t, &x->types[t], false) < 0)
      return -E_IO;
  if (xn_sync_rw (x, x->super.s_rootcat_start, x->roots, false) < 0)
    return -E_IO;
  /* Re-verify the templates: they come from the disk. */
  for (unsigned t = 0; t < XN_MAXTYPES; t++)
    {
      struct xn_template *tp = &x->types[t];
      if (tp->t_name[0] == 0)
	continue;
      tp->t_name[XN_NAMELEN - 1] = 0;
      if (udf_verify (tp->t_owns, tp->t_nowns, UDF_KIND_OWNS) < 0
	  || udf_verify (tp->t_acl, tp->t_nacl, UDF_KIND_ACL) < 0
	  || udf_verify (tp->t_size, tp->t_nsize, UDF_KIND_SIZE) < 0
	  || tp->t_nowns > XN_OWNS_MAX || tp->t_nacl > XN_ACL_MAX
	  || tp->t_nsize > XN_SIZE_MAX)
	{
	  kprintf ("xn%u: invalid template %u, disabled\n", dev, t);
	  memset (tp, 0, sizeof (*tp));
	}
    }

  if (x->super.s_clean)
    {
      for (uint32_t i = 0; i < x->super.s_freemap_nblocks; i++)
	if (xn_sync_rw (x, x->super.s_freemap_start + i,
			(uint8_t *) x->freemap + i * XN_BLKSIZE, false) < 0)
	  return -E_IO;
    }
  else
    {
      xn_rebuild_freemap (x);
      xn_write_freemap (x);
    }
  x->super.s_clean = 0;
  x->super.s_gen++;
  xn_write_super (x);
  x->clean_on_disk = false;
  x->mounted = true;
  sysinfo->si_disk[dev].d_xn = 1;
  kprintf ("xn%u: mounted, %u blocks, generation %u\n", dev,
	   x->super.s_nblocks, x->super.s_gen);
  return 0;
}

static int
xn_format (unsigned dev)
{
  struct xndev *x = &xndevs[dev];
  struct blkdev *bd = blk_get (dev);
  uint32_t nblocks, fmblocks;

  if (bd == NULL)
    return -E_NO_DEV;
  if (x->mounted)
    {
      for (int i = 0; i < XN_NBC; i++)
	if ((bc[i].bc_state & BC_USED) && bc[i].bc_dev == dev)
	  return -E_BUSY;
    }
  nblocks = bd->nsectors / SECT_PER_BLK;
  fmblocks = ROUNDUP (nblocks, XN_BLKSIZE * 8) / (XN_BLKSIZE * 8);
  memset (&x->super, 0, sizeof (x->super));
  x->super.s_magic = XN_MAGIC;
  x->super.s_version = XN_VERSION;
  x->super.s_nblocks = nblocks;
  x->super.s_typecat_start = 1;
  x->super.s_rootcat_start = 1 + XN_MAXTYPES;
  x->super.s_freemap_start = 2 + XN_MAXTYPES;
  x->super.s_freemap_nblocks = fmblocks;
  x->super.s_data_start = 2 + XN_MAXTYPES + fmblocks;
  x->bd = bd;
  x->dev = dev;
  if (x->freemap && x->freemap_size < fmblocks * XN_BLKSIZE)
    return -E_RANGE;
  if (xn_alloc_areas (x) < 0)
    return -E_NO_MEM;
  memset (x->cat + CAT_TYPES_OFF, 0, CAT_SIZE - CAT_TYPES_OFF);
  for (uint32_t b = 0; b < fmblocks * XN_BLKSIZE * 8; b++)
    fm_set (x, b, b >= x->super.s_data_start && b < nblocks);
  memset (xn_tmp, 0, sizeof (xn_tmp));
  for (uint32_t b = 1; b < x->super.s_freemap_start; b++)
    if (xn_sync_rw (x, b, xn_tmp, true) < 0)
      return -E_IO;
  if (xn_write_freemap (x) < 0)
    return -E_IO;
  x->super.s_clean = 0;
  if (xn_write_super (x) < 0)
    return -E_IO;
  x->mounted = true;
  x->clean_on_disk = false;
  sysinfo->si_disk[dev].d_xn = 1;
  kprintf ("xn%u: formatted, %u blocks\n", dev, nblocks);
  return 0;
}

/*
 * Exported regions.
 */
pfn_t
xn_export_pfn (vaddr_t va)
{
  if (va >= UBC && va < UBC + UBC_SIZE)
    {
      size_t off = va - UBC;
      if (off < ROUNDUP (XN_NBC * sizeof (struct bc_entry), PAGE_SIZE))
	return pmem_kva_pfn ((vaddr_t) bc + off);
      return zero_page;
    }
  if (va >= UXNFREE && va < UXNFREE + UXNFREE_SIZE)
    {
      unsigned d = (va - UXNFREE) / 0x400000;
      size_t off = (va - UXNFREE) % 0x400000;
      if (d < XOK_MAXDISK && xndevs[d].freemap && off < xndevs[d].freemap_size)
	return pmem_kva_pfn ((vaddr_t) xndevs[d].freemap + off);
      return zero_page;
    }
  if (va >= UXNCAT && va < UXNCAT + UXNCAT_SIZE)
    {
      unsigned d = (va - UXNCAT) / XN_CAT_SIZE;
      size_t off = (va - UXNCAT) % XN_CAT_SIZE;
      if (d < XOK_MAXDISK && xndevs[d].cat && off < CAT_SIZE)
	return pmem_kva_pfn ((vaddr_t) xndevs[d].cat + off);
      return zero_page;
    }
  return PFN_INVALID;
}

void
xn_init (void)
{
  vaddr_t kva;

  _Static_assert (sizeof (struct xn_template) <= XN_BLKSIZE, "template");
  _Static_assert (sizeof (struct xn_root) * XN_MAXROOTS <= XN_BLKSIZE,
		  "roots");
  _Static_assert (sizeof (struct bc_entry) == 32, "bc_entry");

  bc = pmem_export_area (XN_NBC * sizeof (struct bc_entry), &kva);
  for (unsigned i = 0; i < XN_NHASH; i++)
    bc_hash[i] = BCNIL;
  bc_freelist = BCNIL;
  for (int i = XN_NBC - 1; i >= 0; i--)
    {
      bc_next[i] = bc_freelist;
      bc_freelist = i;
    }
  zero_page = pfn_alloc (0);
  KASSERT (zero_page != PFN_INVALID);

  for (unsigned d = 0; d < blk_count (); d++)
    {
      int r = xn_mount (d);
      if (r < 0 && r != -E_NOT_FOUND)
	kprintf ("xn%u: mount failed (%d)\n", d, r);
    }
}

void
xn_env_freed (struct env *e)
{
  for (int i = 0; i < XN_NBC; i++)
    if ((bc[i].bc_state & BC_LOCKED) && bc[i].bc_locker == e->id)
      {
	bc[i].bc_state &= ~BC_LOCKED;
	bc[i].bc_locker = 0;
      }
}

/*
 * System calls.
 */
static int
sys_type_install (struct xndev *x, uaddr_t ut)
{
  static struct xn_template t;
  unsigned n;
  int free_slot = -1;

  if (copyin (&t, ut, sizeof (t)) < 0)
    return -E_FAULT;
  t.t_name[XN_NAMELEN - 1] = 0;
  if (t.t_name[0] == 0)
    return -E_INVAL;
  if (t.t_nowns > XN_OWNS_MAX || t.t_nacl > XN_ACL_MAX
      || t.t_nsize > XN_SIZE_MAX)
    return -E_INVAL;
  if (udf_verify (t.t_owns, t.t_nowns, UDF_KIND_OWNS) < 0
      || udf_verify (t.t_acl, t.t_nacl, UDF_KIND_ACL) < 0
      || udf_verify (t.t_size, t.t_nsize, UDF_KIND_SIZE) < 0)
    return -E_UDF;
  if ((t.t_flags & XN_TF_DATA) && t.t_nowns)
    return -E_INVAL;

  for (unsigned i = 0; i < XN_MAXTYPES; i++)
    {
      if (x->types[i].t_name[0] == 0)
	{
	  if (free_slot < 0)
	    free_slot = i;
	  continue;
	}
      if (strcmp (x->types[i].t_name, t.t_name) == 0)
	return memcmp (&x->types[i], &t, sizeof (t)) == 0 ? (int) i
	  : -E_EXISTS;
    }
  if (free_slot < 0)
    return -E_FULL;

  /* Templates are immutable once installed; the initial (zeroed)
     state of a metadata type must own nothing. */
  memcpy (&x->types[free_slot], &t, sizeof (t));
  if (!(t.t_flags & XN_TF_DATA))
    {
      memset (xn_tmp, 0, sizeof (xn_tmp));
      if (owns_set (x, free_slot, xn_tmp, set_old, &n) < 0 || n != 0)
	{
	  memset (&x->types[free_slot], 0, sizeof (t));
	  return -E_BOGUS_UPDATE;
	}
    }
  memset (xn_tmp, 0, sizeof (xn_tmp));
  memcpy (xn_tmp, &t, sizeof (t));
  if (xn_sync_rw (x, x->super.s_typecat_start + free_slot, xn_tmp, true) < 0)
    {
      memset (&x->types[free_slot], 0, sizeof (t));
      return -E_IO;
    }
  return free_slot;
}

static int
sys_type_lookup (struct xndev *x, uaddr_t uname)
{
  char name[XN_NAMELEN];

  if (copyin_str (name, uname, XN_NAMELEN) < 0)
    return -E_FAULT;
  for (unsigned i = 0; i < XN_MAXTYPES; i++)
    if (x->types[i].t_name[0] && strcmp (x->types[i].t_name, name) == 0)
      return i;
  return -E_NOT_FOUND;
}

static int
sys_root_install (struct xndev *x, struct env *e, unsigned k, uaddr_t ur)
{
  struct xn_root r;
  int slot = -1;

  if (env_cap (e, k) == NULL)
    return -E_CAP_INVALID;
  if (copyin (&r, ur, sizeof (r)) < 0)
    return -E_FAULT;
  r.r_name[XN_NAMELEN - 1] = 0;
  if (r.r_name[0] == 0 || r.r_nblocks == 0 || r.r_nblocks > 64)
    return -E_INVAL;
  if (r.r_type >= XN_MAXTYPES || x->types[r.r_type].t_name[0] == 0)
    return -E_INVAL;
  for (int i = 0; i < XN_MAXROOTS; i++)
    {
      if (!(x->roots[i].r_flags & XN_RF_USED))
	{
	  if (slot < 0)
	    slot = i;
	}
      else if (strcmp (x->roots[i].r_name, r.r_name) == 0)
	return -E_EXISTS;
    }
  if (slot < 0)
    return -E_FULL;
  for (uint32_t b = r.r_blk; b < r.r_blk + r.r_nblocks; b++)
    if (!blk_in_data (x, b) || !fm_isfree (x, b) || bc_lookup (x->dev, b) >= 0)
      return -E_NOT_FREE;

  mark_unclean (x);
  memset (xn_tmp, 0, sizeof (xn_tmp));
  for (uint32_t b = r.r_blk; b < r.r_blk + r.r_nblocks; b++)
    {
      fm_set (x, b, false);
      if (xn_sync_rw (x, b, xn_tmp, true) < 0)
	return -E_IO;
    }
  r.r_flags = (r.r_flags & XN_RF_TEMPORARY) | XN_RF_USED;
  r.r_guard.c_valid = 1;
  memcpy (&x->roots[slot], &r, sizeof (r));
  if (xn_sync_rw (x, x->super.s_rootcat_start, x->roots, true) < 0)
    return -E_IO;
  return slot;
}

static int
sys_root_lookup (struct xndev *x, uaddr_t uname, uaddr_t uout)
{
  char name[XN_NAMELEN];

  if (copyin_str (name, uname, XN_NAMELEN) < 0)
    return -E_FAULT;
  for (int i = 0; i < XN_MAXROOTS; i++)
    if ((x->roots[i].r_flags & XN_RF_USED)
	&& strcmp (x->roots[i].r_name, name) == 0)
      return copyout (uout, &x->roots[i], sizeof (struct xn_root)) < 0
	? -E_FAULT : i;
  return -E_NOT_FOUND;
}

/*
 * Bind BLK in the registry as a child of PARENT (or as a root block):
 * owns-udf of the parent determines whether the parent really points
 * to it, and its type.  No access control here: it is performed when
 * the data is used.
 */
static int
sys_bind (struct xndev *x, uint32_t blk, uint32_t parent)
{
  int i, p, ri = -1;
  unsigned type = XN_TYPE_UNKNOWN, n;
  uint8_t *page;
  int r;

  if (!blk_in_data (x, blk))
    return -E_INVAL;
  if (parent == XN_NOPARENT)
    {
      ri = find_root (x, blk);
      if (ri < 0)
	return -E_NOT_FOUND;
      type = x->roots[ri].r_type;
    }
  else
    {
      p = bc_lookup (x->dev, parent);
      if (p < 0 || !(bc[p].bc_state & BC_VALID)
	  || bc[p].bc_type == XN_TYPE_UNKNOWN)
	return -E_NOT_INCORE;
      if (!(bc[p].bc_state & BC_META))
	return -E_INVAL;
      page = pfn_get (bc[p].bc_ppn);
      r = owns_set (x, bc[p].bc_type, page, set_old, &n);
      pfn_put (bc[p].bc_ppn, page);
      if (r < 0)
	return r;
      for (unsigned k = 0; k < n; k++)
	if (set_old[k].blk == blk)
	  type = set_old[k].type;
      if (type == XN_TYPE_UNKNOWN)
	return -E_NOT_FOUND;
      ri = bc_rootidx[p];
    }

  i = bc_lookup (x->dev, blk);
  if (i >= 0)
    {
      if (bc[i].bc_type != XN_TYPE_UNKNOWN)
	{
	  if (bc[i].bc_type != type)
	    return -E_CONFLICT;
	  if (parent != XN_NOPARENT && bc[i].bc_parent != parent)
	    return -E_CONFLICT;
	  return i;
	}
    }
  else
    {
      if (fm_isfree (x, blk))
	return -E_NOT_FOUND;
      i = bc_new (x->dev, blk);
      if (i < 0)
	return i;
    }
  bc[i].bc_type = type;
  bc[i].bc_parent = parent;
  bc_rootidx[i] = ri;
  if (parent == XN_NOPARENT)
    bc[i].bc_state |= BC_ROOT;
  if (is_meta (x, type))
    bc[i].bc_state |= BC_META;
  return i;
}

static int
sys_readin (struct xndev *x, uint32_t blk, uint32_t nblk)
{
  if (nblk == 0 || nblk > 64)
    return -E_INVAL;
  for (uint32_t b = blk; b < blk + nblk; b++)
    {
      int i = bc_lookup (x->dev, b);
      if (i < 0)
	{
	  /* Speculative read of an unbound block: its type is unknown
	     until bound, and it cannot be used until then. */
	  if (!blk_in_data (x, b) || fm_isfree (x, b))
	    return -E_INVAL;
	  i = bc_new (x->dev, b);
	  if (i < 0)
	    return i;
	}
      bc[i].bc_lastuse = sysinfo->si_ticks;
      if (bc[i].bc_state & (BC_VALID | BC_IN_TRANSIT))
	continue;
      if (bc[i].bc_ppn == 0 && bc_page_alloc (i) == PFN_INVALID)
	return -E_NO_MEM;
      int r = start_io (x, i, false);
      if (r < 0)
	return r;
    }
  return 0;
}

static int
sys_insert_pte (struct xndev *x, struct env *cur, unsigned k, uint32_t blk,
		uint32_t vaw, unsigned ke, envid_t id)
{
  struct cap *c = env_cap (cur, k);
  bool writable = vaw & 1;
  vaddr_t va = vaw & ~(vaddr_t) PAGE_MASK;
  struct env *t;
  int i, r;

  if (c == NULL)
    return -E_CAP_INVALID;
  if (va >= UXOK_BASE || va == UAREA)
    return -E_INVAL;
  r = env_control (cur, ke, id, &t);
  if (r < 0)
    return r;
  i = bc_lookup (x->dev, blk);
  if (i < 0 || !(bc[i].bc_state & BC_VALID))
    return -E_NOT_INCORE;
  if (bc[i].bc_type == XN_TYPE_UNKNOWN)
    return -E_INVAL;
  if (writable)
    {
      if (bc[i].bc_state & BC_META)
	return -E_ACCESS;	/* Metadata is never mapped writable. */
      if ((bc[i].bc_state & BC_LOCKED) && bc[i].bc_locker != cur->id)
	return -E_BUSY;
    }
  r = acl_check (x, i, c, writable ? XN_OP_WRITE : XN_OP_READ, 0, 0, 0,
		 NULL);
  if (r < 0)
    return r;
  r = env_map (t, va, bc[i].bc_ppn, PTE_P | PTE_U | (writable ? PTE_W : 0));
  if (r < 0)
    return r;
  bc[i].bc_refcnt++;
  bc[i].bc_lastuse = sysinfo->si_ticks;
  if (writable)
    bc[i].bc_state |= BC_DIRTY;
  return 0;
}

/*
 * Metadata update: apply OP's modifications to a copy of the block,
 * verify the owns-udf transition, then commit.
 */
#define MOD_MODIFY 0
#define MOD_ALLOC  1
#define MOD_FREE   2

static int
sys_update (struct xndev *x, struct env *cur, unsigned k, uint32_t blk,
	    uaddr_t uop, int kind)
{
  struct cap *c = env_cap (cur, k);
  struct xn_op op;
  unsigned nold, nnew;
  uint8_t *page;
  int i, r;
  pfn_t newpages[16];
  bool temporary;

  if (c == NULL)
    return -E_CAP_INVALID;
  if (copyin (&op, uop, sizeof (op)) < 0)
    return -E_FAULT;
  if (op.o_nmods > XN_MAXMODS)
    return -E_INVAL;
  if (kind != MOD_MODIFY && (op.o_nchild == 0 || op.o_nchild > 16))
    return -E_INVAL;
  i = bc_lookup (x->dev, blk);
  if (i < 0 || !(bc[i].bc_state & BC_VALID))
    return -E_NOT_INCORE;
  if (!(bc[i].bc_state & BC_META) || bc[i].bc_type == XN_TYPE_UNKNOWN)
    return -E_INVAL;
  if (bc[i].bc_state & BC_IN_TRANSIT)
    return -E_BUSY;
  if ((bc[i].bc_state & BC_LOCKED) && bc[i].bc_locker != cur->id)
    return -E_BUSY;
  temporary = bc_rootidx[i] != 0xffff
    && (x->roots[bc_rootidx[i]].r_flags & XN_RF_TEMPORARY);

  /* Build the proposed new contents. */
  for (unsigned m = 0; m < op.o_nmods; m++)
    if (op.o_mods[m].m_len > XN_MODMAX
	|| op.o_mods[m].m_off + op.o_mods[m].m_len > XN_BLKSIZE)
      return -E_INVAL;
  page = pfn_get (bc[i].bc_ppn);
  memcpy (xn_tmp, page, XN_BLKSIZE);
  pfn_put (bc[i].bc_ppn, page);
  for (unsigned m = 0; m < op.o_nmods; m++)
    if (copyin (xn_tmp + op.o_mods[m].m_off, op.o_mods[m].m_data,
		op.o_mods[m].m_len) < 0)
      return -E_FAULT;

  /* Access control, for each modification, seeing the proposal. */
  for (unsigned m = 0; m < op.o_nmods; m++)
    {
      uint32_t aop = kind == MOD_ALLOC ? XN_OP_ALLOC
	: kind == MOD_FREE ? XN_OP_FREE : XN_OP_MODIFY;
      r = acl_check (x, i, c, aop, op.o_mods[m].m_off, op.o_mods[m].m_len,
		     kind == MOD_MODIFY ? 0 : op.o_child, xn_tmp);
      if (r < 0)
	return r;
    }

  /* Verify. */
  page = pfn_get (bc[i].bc_ppn);
  r = owns_set (x, bc[i].bc_type, page, set_old, &nold);
  pfn_put (bc[i].bc_ppn, page);
  if (r < 0)
    return r;
  r = owns_set (x, bc[i].bc_type, xn_tmp, set_new, &nnew);
  if (r < 0)
    return r;

  if (kind == MOD_MODIFY)
    {
      if (nold != nnew)
	return -E_BOGUS_UPDATE;
      for (unsigned q = 0; q < nold; q++)
	if (set_old[q].blk != set_new[q].blk
	    || set_old[q].type != set_new[q].type)
	  return -E_BOGUS_UPDATE;
    }
  else
    {
      /* ALLOC: new = old + children; FREE: old = new + children. */
      struct xn_item *big = kind == MOD_ALLOC ? set_new : set_old;
      struct xn_item *small = kind == MOD_ALLOC ? set_old : set_new;
      unsigned nbig = kind == MOD_ALLOC ? nnew : nold;
      unsigned nsmall = kind == MOD_ALLOC ? nold : nnew;
      unsigned si = 0, ci = 0;

      if (nbig != nsmall + op.o_nchild)
	return -E_BOGUS_UPDATE;
      for (unsigned bi = 0; bi < nbig; bi++)
	{
	  if (si < nsmall && big[bi].blk == small[si].blk)
	    {
	      if (big[bi].type != small[si].type)
		return -E_BOGUS_UPDATE;
	      si++;
	    }
	  else if (ci < op.o_nchild && big[bi].blk == op.o_child + ci)
	    {
	      if (kind == MOD_ALLOC && big[bi].type != op.o_ctype)
		return -E_BOGUS_UPDATE;
	      if (kind == MOD_FREE)
		op.o_ctype = big[bi].type;
	      ci++;
	    }
	  else
	    return -E_BOGUS_UPDATE;
	}
      if (si != nsmall || ci != op.o_nchild)
	return -E_BOGUS_UPDATE;
    }

  if (kind == MOD_ALLOC)
    {
      for (uint32_t b = op.o_child; b < op.o_child + op.o_nchild; b++)
	if (!blk_in_data (x, b) || !fm_isfree (x, b)
	    || bc_lookup (x->dev, b) >= 0)
	  return -E_NOT_FREE;
      for (unsigned n = 0; n < op.o_nchild; n++)
	{
	  newpages[n] = pmem_alloc (0);
	  if (newpages[n] == PFN_INVALID && xn_reclaim (8))
	    newpages[n] = pmem_alloc (0);
	  if (newpages[n] == PFN_INVALID)
	    {
	      while (n-- > 0)
		pmem_free (newpages[n]);
	      return -E_NO_MEM;
	    }
	}
      mark_unclean (x);
      for (unsigned n = 0; n < op.o_nchild; n++)
	{
	  uint32_t b = op.o_child + n;
	  int ci = bc_new (x->dev, b);
	  if (ci < 0)
	    kpanic ("xn: registry full");
	  fm_set (x, b, false);
	  bc[ci].bc_ppn = newpages[n];
	  ppinfo[newpages[n]].pp_state = PP_BC;
	  ppinfo[newpages[n]].pp_bc = ci + 1;
	  bc[ci].bc_type = op.o_ctype;
	  bc[ci].bc_parent = blk;
	  bc_rootidx[ci] = bc_rootidx[i];
	  bc[ci].bc_state |= BC_VALID | BC_DIRTY
	    | (temporary ? 0 : BC_UNINIT)
	    | (is_meta (x, op.o_ctype) ? BC_META : 0);
	  if (!temporary)
	    bc[i].bc_taint++;
	}
    }
  else if (kind == MOD_FREE)
    {
      /* The children must be unused, and own nothing. */
      for (uint32_t b = op.o_child; b < op.o_child + op.o_nchild; b++)
	{
	  int ci = bc_lookup (x->dev, b);
	  if (ci < 0)
	    continue;
	  if (bc[ci].bc_state & BC_IN_TRANSIT)
	    return -E_BUSY;
	  if (bc[ci].bc_ppn && (ppinfo[bc[ci].bc_ppn].pp_refcnt
				|| ppinfo[bc[ci].bc_ppn].pp_pinned))
	    return -E_BUSY;
	  if (bc[ci].bc_taint)
	    return -E_TAINTED;
	  if (bc[ci].bc_state & BC_META)
	    {
	      unsigned nc;
	      if (!(bc[ci].bc_state & BC_VALID))
		return -E_NOT_INCORE;
	      page = pfn_get (bc[ci].bc_ppn);
	      r = owns_set (x, bc[ci].bc_type, page, set_new, &nc);
	      pfn_put (bc[ci].bc_ppn, page);
	      if (r < 0 || nc != 0)
		return -E_BOGUS_UPDATE;
	    }
	}
      for (uint32_t b = op.o_child; b < op.o_child + op.o_nchild; b++)
	if (bc_lookup (x->dev, b) < 0 && is_meta (x, op.o_ctype))
	  return -E_NOT_INCORE;
      mark_unclean (x);
      for (uint32_t b = op.o_child; b < op.o_child + op.o_nchild; b++)
	{
	  int ci = bc_lookup (x->dev, b);
	  bool uninit = false;

	  if (ci >= 0)
	    {
	      uninit = bc[ci].bc_state & BC_UNINIT;
	      bc_release (ci);
	    }
	  /* Re-parent blocks waiting on b being written. */
	  for (unsigned q = 0; q < XN_NPENDFREE; q++)
	    if (pendfree[q].used && pendfree[q].dev == x->dev
		&& pendfree[q].parent == b)
	      pendfree[q].parent = blk;
	  if (uninit)
	    {
	      if (bc[i].bc_taint)
		bc[i].bc_taint--;
	      fm_set (x, b, true);
	    }
	  else if (temporary)
	    fm_set (x, b, true);
	  else
	    {
	      unsigned q;
	      for (q = 0; q < XN_NPENDFREE; q++)
		if (!pendfree[q].used)
		  break;
	      if (q == XN_NPENDFREE)
		kpanic ("xn: pending free table full");
	      pendfree[q].used = 1;
	      pendfree[q].dev = x->dev;
	      pendfree[q].blk = b;
	      pendfree[q].parent = blk;
	    }
	}
    }

  /* Commit. */
  page = pfn_get (bc[i].bc_ppn);
  memcpy (page, xn_tmp, XN_BLKSIZE);
  pfn_put (bc[i].bc_ppn, page);
  bc[i].bc_state |= BC_DIRTY;
  bc[i].bc_lastuse = sysinfo->si_ticks;
  return 0;
}

static int
sys_writeback (struct xndev *x, uint32_t blk, uint32_t nblk)
{
  int started = 0;

  if (nblk == 0 || nblk > 64)
    return -E_INVAL;
  for (uint32_t b = blk; b < blk + nblk; b++)
    {
      int i = bc_lookup (x->dev, b), r;
      bool temporary;

      if (i < 0 || !(bc[i].bc_state & BC_VALID))
	continue;
      if (bc[i].bc_state & BC_IN_TRANSIT)
	continue;
      temporary = bc_rootidx[i] != 0xffff
	&& (x->roots[bc_rootidx[i]].r_flags & XN_RF_TEMPORARY);
      if (bc[i].bc_taint && !temporary)
	return started ? started : -E_TAINTED;
      if (bc[i].bc_type == XN_TYPE_UNKNOWN)
	continue;
      r = start_io (x, i, true);
      if (r < 0)
	return r;
      started++;
    }
  return started;
}

static int
sys_unbind (struct xndev *x, uint32_t blk)
{
  int i = bc_lookup (x->dev, blk);

  if (i < 0)
    return -E_NOT_INCORE;
  if (!bc_releasable (i))
    return -E_BUSY;
  bc_release (i);
  return 0;
}

static int
sys_lock (struct xndev *x, struct env *cur, unsigned k, uint32_t blk,
	  int lock)
{
  struct cap *c = env_cap (cur, k);
  int i = bc_lookup (x->dev, blk), r;

  if (c == NULL)
    return -E_CAP_INVALID;
  if (i < 0)
    return -E_NOT_INCORE;
  if (lock)
    {
      if (bc[i].bc_state & BC_LOCKED)
	return bc[i].bc_locker == cur->id ? 0 : -E_BUSY;
      r = acl_check (x, i, c, XN_OP_LOCK, 0, 0, 0, NULL);
      if (r < 0)
	return r;
      bc[i].bc_state |= BC_LOCKED;
      bc[i].bc_locker = cur->id;
      return 0;
    }
  if (!(bc[i].bc_state & BC_LOCKED) || bc[i].bc_locker != cur->id)
    return -E_INVAL;
  bc[i].bc_state &= ~BC_LOCKED;
  bc[i].bc_locker = 0;
  sched_kick_idle ();
  return 0;
}

static int
sys_sync (struct xndev *x)
{
  bool clean = true;
  int r;

  for (int i = 0; i < XN_NBC; i++)
    if ((bc[i].bc_state & BC_USED) && bc[i].bc_dev == x->dev
	&& (bc[i].bc_state & (BC_DIRTY | BC_UNINIT | BC_IN_TRANSIT)))
      clean = false;
  for (unsigned q = 0; q < XN_NPENDFREE; q++)
    if (pendfree[q].used && pendfree[q].dev == x->dev)
      clean = false;
  r = xn_write_freemap (x);
  if (r < 0)
    return r;
  x->super.s_clean = clean;
  r = xn_write_super (x);
  if (r < 0)
    return r;
  x->clean_on_disk = clean;
  return clean;
}

int
sys_xn (struct env *e, unsigned num, unsigned long a1, unsigned long a2,
	unsigned long a3, unsigned long a4, unsigned long a5,
	unsigned long a6)
{
  struct xndev *x;

  if (num == SYS_xn_format)
    {
      struct cap *c = env_cap (e, a1);
      if (c == NULL)
	return -E_CAP_INVALID;
      if (c->c_len != 0)
	return -E_CAP_INSUFF;
      if (a2 >= XOK_MAXDISK)
	return -E_NO_DEV;
      return xn_format (a2);
    }

  switch (num)
    {
    case SYS_xn_root_install:
    case SYS_xn_insert_pte:
    case SYS_xn_alloc:
    case SYS_xn_free:
    case SYS_xn_modify:
    case SYS_xn_lock:
      x = xn_get (a2);
      break;
    default:
      x = xn_get (a1);
    }
  if (x == NULL)
    return -E_NO_DEV;

  switch (num)
    {
    case SYS_xn_type_install:
      return sys_type_install (x, a2);
    case SYS_xn_type_lookup:
      return sys_type_lookup (x, a2);
    case SYS_xn_root_install:
      return sys_root_install (x, e, a1, a3);
    case SYS_xn_root_lookup:
      return sys_root_lookup (x, a2, a3);
    case SYS_xn_bind:
      return sys_bind (x, a2, a3);
    case SYS_xn_readin:
      return sys_readin (x, a2, a3);
    case SYS_xn_insert_pte:
      return sys_insert_pte (x, e, a1, a3, a4, a5, a6);
    case SYS_xn_alloc:
      return sys_update (x, e, a1, a3, a4, MOD_ALLOC);
    case SYS_xn_free:
      return sys_update (x, e, a1, a3, a4, MOD_FREE);
    case SYS_xn_modify:
      return sys_update (x, e, a1, a3, a4, MOD_MODIFY);
    case SYS_xn_writeback:
      return sys_writeback (x, a2, a3);
    case SYS_xn_unbind:
      return sys_unbind (x, a2);
    case SYS_xn_lock:
      return sys_lock (x, e, a1, a3, a4);
    case SYS_xn_lookup:
      {
	int i = bc_lookup (x->dev, a2);
	return i < 0 ? -E_NOT_INCORE : i;
      }
    case SYS_xn_sync:
      return sys_sync (x);
    }
  return -E_NOSYS;
}
