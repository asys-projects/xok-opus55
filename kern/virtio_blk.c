/*
 * Virtio block device driver.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "virtio.h"
#include "blk.h"

#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1

struct vblk_hdr
{
  uint32_t type;
  uint32_t reserved;
  uint64_t sector;
} __attribute__ ((packed));

struct vblk
{
  struct blkdev blk;
  uint16_t io;
  struct virtq q;
  struct dmabuf hdrbuf;		/* Header at 0, status at 64. */
  int head;
};

static int
vblk_start (struct blkdev *d, struct bio *b)
{
  struct vblk *v = d->drv;
  struct vblk_hdr *hdr = v->hdrbuf.va;
  volatile uint8_t *status = (uint8_t *) v->hdrbuf.va + 64;
  int head;
  uint16_t di;

  head = virtq_alloc_chain (&v->q, b->nsegs + 2);
  if (head < 0)
    return -E_BUSY;
  hdr->type = b->write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
  hdr->reserved = 0;
  hdr->sector = b->sector;
  *status = 0xff;

  di = head;
  v->q.desc[di].addr = v->hdrbuf.pa;
  v->q.desc[di].len = sizeof (*hdr);
  v->q.desc[di].flags = VRING_DESC_F_NEXT;
  for (unsigned i = 0; i < b->nsegs; i++)
    {
      di = v->q.desc[di].next;
      v->q.desc[di].addr = b->seg[i].pa;
      v->q.desc[di].len = b->seg[i].len;
      v->q.desc[di].flags = VRING_DESC_F_NEXT
	| (b->write ? 0 : VRING_DESC_F_WRITE);
    }
  di = v->q.desc[di].next;
  v->q.desc[di].addr = v->hdrbuf.pa + 64;
  v->q.desc[di].len = 1;
  v->q.desc[di].flags = VRING_DESC_F_WRITE;
  v->head = head;
  virtq_submit (v->io, &v->q, head);
  return 0;
}

static bool
vblk_check (struct vblk *v)
{
  uint32_t id, len;
  bool done = false;

  while (virtq_used (&v->q, &id, &len))
    {
      volatile uint8_t *status = (uint8_t *) v->hdrbuf.va + 64;
      virtq_free_chain (&v->q, id);
      if ((int) id == v->head)
	{
	  v->head = -1;
	  blk_complete (&v->blk, *status == 0 ? 0 : -E_IO);
	  done = true;
	}
    }
  return done;
}

static bool
vblk_poll (struct blkdev *d)
{
  return vblk_check (d->drv);
}

static void
vblk_irq (unsigned irq, void *arg)
{
  struct vblk *v = arg;

  (void) inb (v->io + VIRTIO_PCI_ISR);
  vblk_check (v);
}

static const struct blkdev_ops vblk_ops = {
  .start = vblk_start,
  .poll = vblk_poll,
};

bool
virtio_blk_probe (struct pci_dev *d)
{
  struct vblk *v;

  if (d->vendor != VIRTIO_VENDOR || (d->device != 0x1001
				     && d->device != 0x1042))
    return false;
  v = (struct vblk *) kmem_alloc (0, sizeof (*v));
  KASSERT (v != NULL);
  memset (v, 0, sizeof (*v));
  v->io = virtio_begin (d);
  if (v->io == 0)
    {
      kprintf ("virtio-blk: no legacy I/O interface\n");
      return false;
    }
  outl (v->io + VIRTIO_PCI_GUEST_FEATURES, 0);
  if (virtq_init (v->io, &v->q, 0) < 0 || dma_alloc (&v->hdrbuf, 4096) < 0)
    {
      outb (v->io + VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
      return false;
    }
  v->head = -1;
  v->blk.nsectors = inl (v->io + VIRTIO_PCI_CONFIG)
    | ((uint64_t) inl (v->io + VIRTIO_PCI_CONFIG + 4) << 32);
  strlcpy (v->blk.name, "virtio-blk", sizeof (v->blk.name));
  v->blk.ops = &vblk_ops;
  v->blk.drv = v;
  virtio_ready (v->io);
  irq_register (d->irq, vblk_irq, v);
  blk_register (&v->blk);
  return true;
}
