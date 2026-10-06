/*
 * Xok block device layer, and the raw disk interface.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "env.h"
#include "dev.h"
#include "blk.h"

static struct blkdev *blkdevs[MAXBLKDEV];
static unsigned nblkdevs;

static struct bio *bio_freelist;

struct bio *
bio_alloc (void)
{
  struct bio *b = bio_freelist;

  if (b)
    bio_freelist = b->next;
  else
    {
      b = (struct bio *) kmalloc (sizeof (*b));
      if (b == NULL)
	return NULL;
    }
  memset (b, 0, sizeof (*b));
  return b;
}

void
bio_free (struct bio *b)
{
  b->next = bio_freelist;
  bio_freelist = b;
}

int
bio_add_page (struct bio *b, pfn_t pfn, uint32_t off, uint32_t len)
{
  if (b->nsegs >= BIO_MAXSEGS || off + len > PAGE_SIZE || (len & 511))
    return -E_INVAL;
  b->seg[b->nsegs].pa = ((paddr_t) pfn << PAGE_SHIFT) + off;
  b->seg[b->nsegs].len = len;
  b->nsegs++;
  return 0;
}

static void
blk_poll_dev (void *arg)
{
  struct blkdev *d = arg;

  if (d->active && d->ops->poll)
    d->ops->poll (d);
}

int
blk_register (struct blkdev *d)
{
  if (nblkdevs >= MAXBLKDEV)
    return -E_FULL;
  d->idx = nblkdevs;
  blkdevs[nblkdevs++] = d;
  sysinfo->si_ndisk = nblkdevs;
  sysinfo->si_disk[d->idx].d_present = 1;
  sysinfo->si_disk[d->idx].d_nsectors = d->nsectors;
  sysinfo->si_disk[d->idx].d_nblocks = d->nsectors / 8;
  strlcpy ((char *) sysinfo->si_disk[d->idx].d_name, d->name, 16);
  dev_register_poll (blk_poll_dev, d);
  kprintf ("blk%u: %s, %llu sectors (%llu MB)\n", d->idx, d->name,
	   d->nsectors, d->nsectors / 2048);
  return 0;
}

struct blkdev *
blk_get (unsigned idx)
{
  return idx < nblkdevs ? blkdevs[idx] : NULL;
}

unsigned
blk_count (void)
{
  return nblkdevs;
}

static void
blk_start_next (struct blkdev *d)
{
  while (d->active == NULL && d->qhead != NULL)
    {
      struct bio *b = d->qhead;
      int r;

      d->qhead = b->next;
      if (d->qhead == NULL)
	d->qtail = NULL;
      b->next = NULL;
      if (b->sector + b->nsect > d->nsectors)
	r = -E_RANGE;
      else
	{
	  d->active = b;
	  r = d->ops->start (d, b);
	}
      if (r < 0)
	{
	  d->active = NULL;
	  d->nerrors++;
	  b->error = r;
	  b->done (b);
	}
    }
}

void
blk_submit (struct blkdev *d, struct bio *b)
{
  uint32_t total = 0;

  for (unsigned i = 0; i < b->nsegs; i++)
    total += b->seg[i].len;
  KASSERT (total == b->nsect * SECTOR_SIZE);
  b->next = NULL;
  b->error = 0;
  if (d->qtail)
    d->qtail->next = b;
  else
    d->qhead = b;
  d->qtail = b;
  blk_start_next (d);
}

/*
 * Called by drivers when the active request completes.
 */
void
blk_complete (struct blkdev *d, int error)
{
  struct bio *b = d->active;

  if (b == NULL)
    return;
  d->active = NULL;
  b->error = error;
  if (error)
    d->nerrors++;
  else if (b->write)
    d->nwrites++;
  else
    d->nreads++;
  b->done (b);
  blk_start_next (d);
  sched_kick_idle ();
}

/*
 * Synchronous I/O by polling (used at boot and for small kernel
 * metadata transfers).  BUF must be a kernel buffer; it is bounced
 * through a page.
 */
static void
sync_done (struct bio *b)
{
  *(volatile int *) b->arg = 1;
}

int
blk_rw_sync (struct blkdev *d, uint64_t sector, uint32_t nsect, void *buf,
	     bool write)
{
  while (nsect > 0)
    {
      uint32_t n = MIN (nsect, PAGE_SIZE / SECTOR_SIZE);
      pfn_t pfn = pfn_alloc (0);
      volatile int done = 0;
      struct bio *b;
      void *va;
      int err;

      if (pfn == PFN_INVALID)
	return -E_NO_MEM;
      if (write)
	{
	  va = pfn_get (pfn);
	  memcpy (va, buf, n * SECTOR_SIZE);
	  pfn_put (pfn, va);
	}
      b = bio_alloc ();
      if (b == NULL)
	{
	  pfn_free (pfn);
	  return -E_NO_MEM;
	}
      b->sector = sector;
      b->nsect = n;
      b->write = write;
      bio_add_page (b, pfn, 0, n * SECTOR_SIZE);
      b->done = sync_done;
      b->arg = (void *) &done;
      blk_submit (d, b);
      for (uint64_t t0 = ktime (); !done;)
	{
	  if (d->ops->poll)
	    d->ops->poll (d);
	  if (ktime () - t0 > 10000000000ULL)
	    {
	      kprintf ("blk%u: synchronous I/O timeout\n", d->idx);
	      return -E_IO;
	    }
	}
      err = b->error;
      bio_free (b);
      if (!write && err == 0)
	{
	  va = pfn_get (pfn);
	  memcpy (buf, va, n * SECTOR_SIZE);
	  pfn_put (pfn, va);
	}
      pfn_free (pfn);
      if (err)
	return -E_IO;
      sector += n;
      nsect -= n;
      buf = (uint8_t *) buf + n * SECTOR_SIZE;
    }
  return 0;
}

void
blk_init (void)
{
}

/*
 * Raw disk access (root capability only): read or write sectors
 * to/from a page the caller has access to.  Completion is signalled by
 * incrementing *DONE (the caller sleeps on it with a wakeup predicate).
 */
struct rawreq
{
  pfn_t page;
  pfn_t donepfn;
  uint32_t doneoff;
  int32_t status;
};

static void
raw_done (struct bio *b)
{
  struct rawreq *rq = b->arg;
  uint32_t *p;
  uint8_t *page;

  if (rq->donepfn != PFN_INVALID)
    {
      page = pfn_get (rq->donepfn);
      p = (uint32_t *) (page + rq->doneoff);
      __atomic_store_n (p, b->error ? 0xffffffffu : 1, __ATOMIC_RELEASE);
      pfn_put (rq->donepfn, page);
      pmem_unpin (rq->donepfn);
    }
  pmem_unpin (rq->page);
  kfree (rq, sizeof (*rq));
  bio_free (b);
}

int
sys_disk_request (struct env *e, unsigned k, unsigned dev, uint32_t sector,
		  unsigned nsect, uint32_t ppn, unsigned write, uaddr_t done)
{
  struct cap *c = env_cap (e, k);
  struct blkdev *d = blk_get (dev);
  struct rawreq *rq;
  struct bio *b;
  xpte_t pte;

  if (c == NULL)
    return -E_CAP_INVALID;
  if (c->c_len != 0)
    return -E_CAP_INSUFF;
  if (d == NULL)
    return -E_NO_DEV;
  if (nsect == 0 || nsect > 8 || ppn >= npages
      || ppinfo[ppn].pp_state != PP_USER)
    return -E_INVAL;
  if (done & 3)
    return -E_INVAL;

  rq = (struct rawreq *) kmalloc (sizeof (*rq));
  b = bio_alloc ();
  if (rq == NULL || b == NULL)
    return -E_NO_MEM;
  rq->page = ppn;
  rq->donepfn = PFN_INVALID;
  if (done)
    {
      pte = env_getpte (e, done & ~(uaddr_t) PAGE_MASK);
      if (!(pte & PTE_P) || !(pte & PTE_W))
	{
	  kfree (rq, sizeof (*rq));
	  bio_free (b);
	  return -E_FAULT;
	}
      rq->donepfn = PTE_PPN (pte);
      rq->doneoff = done & PAGE_MASK;
      pmem_pin (rq->donepfn);
    }
  pmem_pin (ppn);
  b->sector = sector;
  b->nsect = nsect;
  b->write = write;
  bio_add_page (b, ppn, 0, nsect * SECTOR_SIZE);
  b->done = raw_done;
  b->arg = rq;
  blk_submit (d, b);
  return 0;
}
