/*
 * Virtio over PCI (legacy interface) and split virtqueues.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef XOK_KERN_VIRTIO_H
#define XOK_KERN_VIRTIO_H

#include "dev.h"

#define VIRTIO_VENDOR 0x1af4

/* Legacy PCI register offsets (I/O BAR 0). */
#define VIRTIO_PCI_HOST_FEATURES  0x00
#define VIRTIO_PCI_GUEST_FEATURES 0x04
#define VIRTIO_PCI_QUEUE_PFN      0x08
#define VIRTIO_PCI_QUEUE_NUM      0x0c
#define VIRTIO_PCI_QUEUE_SEL      0x0e
#define VIRTIO_PCI_QUEUE_NOTIFY   0x10
#define VIRTIO_PCI_STATUS         0x12
#define VIRTIO_PCI_ISR            0x13
#define VIRTIO_PCI_CONFIG         0x14

#define VIRTIO_STATUS_ACK       1
#define VIRTIO_STATUS_DRIVER    2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FAILED    128

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2

struct vring_desc
{
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
} __attribute__ ((packed));

struct vring_used_elem
{
  uint32_t id;
  uint32_t len;
} __attribute__ ((packed));

struct virtq
{
  uint16_t num;
  uint16_t qidx;
  struct dmabuf mem;
  volatile struct vring_desc *desc;
  volatile uint16_t *avail;	/* flags, idx, ring[num], used_event */
  volatile uint8_t *used;	/* flags, idx, ring[num] (8 bytes each) */
  uint16_t last_used;
  uint16_t free_head;
  uint16_t nfree;
};

int virtq_init (uint16_t iobase, struct virtq *q, unsigned idx);
int virtq_alloc_chain (struct virtq *q, unsigned n);
void virtq_free_chain (struct virtq *q, uint16_t head);
void virtq_submit (uint16_t iobase, struct virtq *q, uint16_t head);
bool virtq_used (struct virtq *q, uint32_t * id, uint32_t * len);
uint16_t virtio_begin (struct pci_dev *d);
void virtio_ready (uint16_t iobase);

#endif
