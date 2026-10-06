/*
 * Virtio legacy PCI transport and split virtqueues.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "virtio.h"

#define VRING_ALIGN 4096

static inline void
mb (void)
{
  __atomic_thread_fence (__ATOMIC_SEQ_CST);
}

uint16_t
virtio_begin (struct pci_dev *d)
{
  uint16_t io;

  if (!d->bar_io[0] || d->bar[0] == 0)
    return 0;
  io = d->bar[0];
  pci_enable (d, true);
  outb (io + VIRTIO_PCI_STATUS, 0);
  outb (io + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK);
  outb (io + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
  return io;
}

void
virtio_ready (uint16_t io)
{
  outb (io + VIRTIO_PCI_STATUS, inb (io + VIRTIO_PCI_STATUS)
	| VIRTIO_STATUS_DRIVER_OK);
}

int
virtq_init (uint16_t io, struct virtq *q, unsigned idx)
{
  uint16_t num;
  size_t availsz, usedoff, total;

  outw (io + VIRTIO_PCI_QUEUE_SEL, idx);
  num = inw (io + VIRTIO_PCI_QUEUE_NUM);
  if (num == 0)
    return -E_NO_DEV;
  availsz = 6 + 2 * num;
  usedoff = ROUNDUP (16 * num + availsz, VRING_ALIGN);
  total = usedoff + ROUNDUP (6 + 8 * num, VRING_ALIGN);
  if (dma_alloc (&q->mem, total) < 0)
    return -E_NO_MEM;
  q->num = num;
  q->qidx = idx;
  q->desc = q->mem.va;
  q->avail = (volatile uint16_t *) ((uint8_t *) q->mem.va + 16 * num);
  q->used = (volatile uint8_t *) q->mem.va + usedoff;
  q->last_used = 0;
  for (unsigned i = 0; i < num; i++)
    q->desc[i].next = (i + 1) % num;
  q->free_head = 0;
  q->nfree = num;
  outl (io + VIRTIO_PCI_QUEUE_PFN, q->mem.pa >> 12);
  return 0;
}

/* Allocate a chain of N descriptors; returns the head index or -1. */
int
virtq_alloc_chain (struct virtq *q, unsigned n)
{
  uint16_t head, d;

  if (n == 0 || q->nfree < n)
    return -1;
  head = d = q->free_head;
  for (unsigned i = 0; i < n - 1; i++)
    {
      q->desc[d].flags = VRING_DESC_F_NEXT;
      d = q->desc[d].next;
    }
  q->desc[d].flags = 0;
  q->free_head = q->desc[d].next;
  q->nfree -= n;
  return head;
}

void
virtq_free_chain (struct virtq *q, uint16_t head)
{
  uint16_t d = head;
  unsigned n = 1;

  while (q->desc[d].flags & VRING_DESC_F_NEXT)
    {
      d = q->desc[d].next;
      n++;
    }
  q->desc[d].next = q->free_head;
  q->free_head = head;
  q->nfree += n;
}

void
virtq_submit (uint16_t io, struct virtq *q, uint16_t head)
{
  uint16_t aidx = q->avail[1];

  q->avail[2 + (aidx % q->num)] = head;
  mb ();
  q->avail[1] = aidx + 1;
  mb ();
  outw (io + VIRTIO_PCI_QUEUE_NOTIFY, q->qidx);
}

bool
virtq_used (struct virtq *q, uint32_t * id, uint32_t * len)
{
  volatile uint16_t *uidx = (volatile uint16_t *) (q->used + 2);
  volatile struct vring_used_elem *ring =
    (volatile struct vring_used_elem *) (q->used + 4);

  mb ();
  if (*uidx == q->last_used)
    return false;
  *id = ring[q->last_used % q->num].id;
  *len = ring[q->last_used % q->num].len;
  q->last_used++;
  return true;
}
