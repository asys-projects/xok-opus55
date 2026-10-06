/*
 * NVM Express disk driver: one admin queue pair, one I/O queue pair,
 * namespace 1, one outstanding command (requests are serialised by the
 * block layer).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "dev.h"
#include "blk.h"

#define NVME_CAP   0x00
#define NVME_CC    0x14
#define NVME_CSTS  0x1c
#define NVME_AQA   0x24
#define NVME_ASQ   0x28
#define NVME_ACQ   0x30

#define QSIZE 16

struct nvme_sqe
{
  uint8_t opc, flags;
  uint16_t cid;
  uint32_t nsid;
  uint64_t rsvd;
  uint64_t mptr;
  uint64_t prp1, prp2;
  uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __attribute__ ((packed));

struct nvme_cqe
{
  uint32_t dw0, dw1;
  uint16_t sqhd, sqid;
  uint16_t cid;
  uint16_t status;		/* Bit 0: phase. */
} __attribute__ ((packed));

struct nvme_q
{
  struct dmabuf sq, cq;
  volatile struct nvme_sqe *sqe;
  volatile struct nvme_cqe *cqe;
  unsigned sqt, cqh, phase, qid;
};

struct nvme
{
  struct blkdev blk;
  volatile uint8_t *regs;
  unsigned dstrd;
  struct nvme_q aq, ioq;
  struct dmabuf prplist, idbuf;
  unsigned lbashift;		/* log2 of the LBA size. */
  bool busy;
  uint16_t cid;
  struct bio *bio;		/* Active request. */
  unsigned seg;			/* Next segment (per-segment mode). */
  uint64_t sector;		/* Its sector. */
};

static inline uint32_t
rd32 (struct nvme *n, uint32_t r)
{
  return mmio_read32 (n->regs, r);
}

static inline void
wr32 (struct nvme *n, uint32_t r, uint32_t v)
{
  mmio_write32 (n->regs, r, v);
}

static inline void
wr64 (struct nvme *n, uint32_t r, uint64_t v)
{
  mmio_write32 (n->regs, r, v);
  mmio_write32 (n->regs, r + 4, v >> 32);
}

static void
doorbell (struct nvme *n, unsigned qid, bool cq, uint32_t v)
{
  wr32 (n, 0x1000 + (2 * qid + (cq ? 1 : 0)) * (4 << n->dstrd), v);
}

static int
q_init (struct nvme_q *q, unsigned qid)
{
  if (dma_alloc (&q->sq, QSIZE * sizeof (struct nvme_sqe)) < 0
      || dma_alloc (&q->cq, QSIZE * sizeof (struct nvme_cqe)) < 0)
    return -E_NO_MEM;
  q->sqe = q->sq.va;
  q->cqe = q->cq.va;
  q->sqt = q->cqh = 0;
  q->phase = 1;
  q->qid = qid;
  return 0;
}

static void
q_submit (struct nvme *n, struct nvme_q *q, struct nvme_sqe *cmd)
{
  cmd->cid = ++n->cid;
  memcpy ((void *) &q->sqe[q->sqt], cmd, sizeof (*cmd));
  q->sqt = (q->sqt + 1) % QSIZE;
  __atomic_thread_fence (__ATOMIC_SEQ_CST);
  doorbell (n, q->qid, false, q->sqt);
}

/* Returns 1 and the status if a completion is available. */
static int
q_complete (struct nvme *n, struct nvme_q *q, uint16_t * status)
{
  volatile struct nvme_cqe *c = &q->cqe[q->cqh];

  if ((c->status & 1) != q->phase)
    return 0;
  *status = c->status >> 1;
  q->cqh = (q->cqh + 1) % QSIZE;
  if (q->cqh == 0)
    q->phase ^= 1;
  doorbell (n, q->qid, true, q->cqh);
  return 1;
}

static int
admin_sync (struct nvme *n, struct nvme_sqe *cmd)
{
  uint16_t st;

  q_submit (n, &n->aq, cmd);
  for (uint64_t t0 = ktime (); ktime () - t0 < 2000000000ULL;)
    if (q_complete (n, &n->aq, &st))
      return st ? -E_IO : 0;
  return -E_IO;
}

/* Can the request be described by one PRP list? */
static bool
prp_ok (struct bio *b)
{
  for (unsigned i = 0; i < b->nsegs; i++)
    {
      if (i > 0 && (b->seg[i].pa & PAGE_MASK))
	return false;
      if (i < b->nsegs - 1 && ((b->seg[i].pa + b->seg[i].len) & PAGE_MASK))
	return false;
    }
  return true;
}

static void
issue (struct nvme *n, struct bio *b, uint64_t sector, uint32_t nsect,
       unsigned first, unsigned nsegs)
{
  struct nvme_sqe cmd;
  uint64_t lba = (sector * SECTOR_SIZE) >> n->lbashift;

  memset (&cmd, 0, sizeof (cmd));
  cmd.opc = b->write ? 0x01 : 0x02;
  cmd.nsid = 1;
  cmd.prp1 = b->seg[first].pa;
  if (nsegs == 2)
    cmd.prp2 = b->seg[first + 1].pa;
  else if (nsegs > 2)
    {
      uint64_t *l = n->prplist.va;
      for (unsigned i = 1; i < nsegs; i++)
	l[i - 1] = b->seg[first + i].pa;
      cmd.prp2 = n->prplist.pa;
    }
  cmd.cdw10 = lba;
  cmd.cdw11 = lba >> 32;
  cmd.cdw12 = ((nsect * SECTOR_SIZE) >> n->lbashift) - 1;
  q_submit (n, &n->ioq, &cmd);
}

static int
nvme_start (struct blkdev *d, struct bio *b)
{
  struct nvme *n = d->drv;
  uint32_t lbamask = (1u << n->lbashift) - 1;

  if (((b->sector * SECTOR_SIZE) & lbamask) || ((b->nsect * SECTOR_SIZE)
						 & lbamask))
    return -E_INVAL;
  n->busy = true;
  n->bio = b;
  if (prp_ok (b))
    {
      n->seg = b->nsegs;
      issue (n, b, b->sector, b->nsect, 0, b->nsegs);
    }
  else
    {
      /* One command per segment (each lies within a page). */
      if (b->seg[0].len & lbamask)
	return -E_INVAL;
      n->seg = 1;
      n->sector = b->sector + b->seg[0].len / SECTOR_SIZE;
      issue (n, b, b->sector, b->seg[0].len / SECTOR_SIZE, 0, 1);
    }
  return 0;
}

static bool
nvme_check (struct nvme *n)
{
  uint16_t st;
  struct bio *b = n->bio;

  if (!n->busy || !q_complete (n, &n->ioq, &st))
    return false;
  if (st == 0 && n->seg < b->nsegs)
    {
      uint32_t ns = b->seg[n->seg].len / SECTOR_SIZE;
      issue (n, b, n->sector, ns, n->seg, 1);
      n->sector += ns;
      n->seg++;
      return false;
    }
  n->busy = false;
  blk_complete (&n->blk, st ? -E_IO : 0);
  return true;
}

static bool
nvme_poll (struct blkdev *d)
{
  return nvme_check (d->drv);
}

static void
nvme_irq (unsigned irq, void *arg)
{
  nvme_check (arg);
}

static const struct blkdev_ops nvme_ops = {
  .start = nvme_start,
  .poll = nvme_poll,
};

bool
nvme_probe (struct pci_dev *d)
{
  struct nvme *n;
  struct nvme_sqe cmd;
  uint64_t cap;
  uint8_t *id;

  if (d->class != 0x01 || d->subclass != 0x08 || d->progif != 0x02)
    return false;
  pci_enable (d, true);
  n = (struct nvme *) kmalloc (sizeof (*n));
  KASSERT (n != NULL);
  memset (n, 0, sizeof (*n));
  n->regs = pci_map_bar (d, 0);
  if (n->regs == NULL)
    {
      kprintf ("nvme: BAR0 %x not mapped (64-bit BARs above 4GB are not "
	       "supported)\n", pci_read32 (d, 0x10));
      return false;
    }
  cap = rd32 (n, NVME_CAP) | ((uint64_t) rd32 (n, NVME_CAP + 4) << 32);
  n->dstrd = (cap >> 32) & 0xf;

  /* Reset, then configure the admin queues. */
  wr32 (n, NVME_CC, 0);
  for (uint64_t t0 = ktime (); (rd32 (n, NVME_CSTS) & 1)
       && ktime () - t0 < 2000000000ULL;)
    ;
  if (q_init (&n->aq, 0) < 0 || q_init (&n->ioq, 1) < 0
      || dma_alloc (&n->prplist, PAGE_SIZE) < 0
      || dma_alloc (&n->idbuf, PAGE_SIZE) < 0)
    return false;
  wr32 (n, NVME_AQA, ((QSIZE - 1) << 16) | (QSIZE - 1));
  wr64 (n, NVME_ASQ, n->aq.sq.pa);
  wr64 (n, NVME_ACQ, n->aq.cq.pa);
  wr32 (n, NVME_CC, 1 | (6 << 16) | (4 << 20));
  for (uint64_t t0 = ktime (); !(rd32 (n, NVME_CSTS) & 1);)
    if (ktime () - t0 > 2000000000ULL)
      {
	kprintf ("nvme: controller not ready\n");
	return false;
      }

  /* Identify namespace 1. */
  memset (&cmd, 0, sizeof (cmd));
  cmd.opc = 0x06;
  cmd.nsid = 1;
  cmd.prp1 = n->idbuf.pa;
  cmd.cdw10 = 0;
  if (admin_sync (n, &cmd) < 0)
    {
      kprintf ("nvme: identify failed\n");
      return false;
    }
  id = n->idbuf.va;
  uint64_t nsze = *(uint64_t *) id;
  unsigned flbas = id[26] & 0xf;
  n->lbashift = id[128 + 4 * flbas + 2];
  if (n->lbashift < 9 || n->lbashift > 12)
    {
      kprintf ("nvme: unsupported LBA size 2^%u\n", n->lbashift);
      return false;
    }

  /* I/O completion queue (interrupts on vector 0), then submission. */
  memset (&cmd, 0, sizeof (cmd));
  cmd.opc = 0x05;
  cmd.prp1 = n->ioq.cq.pa;
  cmd.cdw10 = ((QSIZE - 1) << 16) | 1;
  cmd.cdw11 = 0x3;
  if (admin_sync (n, &cmd) < 0)
    return false;
  memset (&cmd, 0, sizeof (cmd));
  cmd.opc = 0x01;
  cmd.prp1 = n->ioq.sq.pa;
  cmd.cdw10 = ((QSIZE - 1) << 16) | 1;
  cmd.cdw11 = (1 << 16) | 1;
  if (admin_sync (n, &cmd) < 0)
    return false;

  n->blk.nsectors = (nsze << n->lbashift) / SECTOR_SIZE;
  strlcpy (n->blk.name, "nvme", sizeof (n->blk.name));
  n->blk.ops = &nvme_ops;
  n->blk.drv = n;
  irq_register (d->irq, nvme_irq, n);
  blk_register (&n->blk);
  return true;
}
