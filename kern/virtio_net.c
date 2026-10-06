/*
 * Virtio network device driver.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "virtio.h"
#include "net.h"

#define VIRTIO_NET_F_MAC    (1 << 5)
#define VIRTIO_NET_F_STATUS (1 << 16)

#define NETHDR 10		/* struct virtio_net_hdr (legacy) */
#define BUFSZ 2048
#define NRXBUF 64
#define NTXBUF 64

struct vnet
{
  struct netdev nd;
  uint16_t io;
  struct virtq rxq, txq;
  struct dmabuf rxbufs, txbufs;
  unsigned txnext;
  bool txbusy[NTXBUF];
  int txdesc[NTXBUF];
};

static void
rx_post (struct vnet *v, unsigned i)
{
  int d = virtq_alloc_chain (&v->rxq, 1);
  if (d < 0)
    return;
  v->rxq.desc[d].addr = v->rxbufs.pa + i * BUFSZ;
  v->rxq.desc[d].len = BUFSZ;
  v->rxq.desc[d].flags = VRING_DESC_F_WRITE;
  /* Remember which buffer: descriptor index == buffer index. */
  if ((unsigned) d != i)
    {
      v->rxq.desc[d].addr = v->rxbufs.pa + d * BUFSZ;
    }
  virtq_submit (v->io, &v->rxq, d);
}

static void
tx_reap (struct vnet *v)
{
  uint32_t id, len;

  while (virtq_used (&v->txq, &id, &len))
    {
      for (unsigned i = 0; i < NTXBUF; i++)
	if (v->txbusy[i] && v->txdesc[i] == (int) id)
	  v->txbusy[i] = false;
      virtq_free_chain (&v->txq, id);
    }
}

static int
vnet_xmit (struct netdev *nd, const void *frame, unsigned len)
{
  struct vnet *v = nd->drv;
  unsigned i = v->txnext;
  uint8_t *buf;
  int d;

  tx_reap (v);
  if (len + NETHDR > BUFSZ)
    return -E_INVAL;
  if (v->txbusy[i])
    return -E_FULL;
  d = virtq_alloc_chain (&v->txq, 1);
  if (d < 0)
    return -E_FULL;
  buf = (uint8_t *) v->txbufs.va + i * BUFSZ;
  memset (buf, 0, NETHDR);
  memcpy (buf + NETHDR, frame, len);
  v->txq.desc[d].addr = v->txbufs.pa + i * BUFSZ;
  v->txq.desc[d].len = NETHDR + len;
  v->txq.desc[d].flags = 0;
  v->txbusy[i] = true;
  v->txdesc[i] = d;
  v->txnext = (i + 1) % NTXBUF;
  virtq_submit (v->io, &v->txq, d);
  return 0;
}

static void
vnet_rx (struct vnet *v)
{
  uint32_t id, len;

  while (virtq_used (&v->rxq, &id, &len))
    {
      uint8_t *buf = (uint8_t *) v->rxbufs.va + id * BUFSZ;
      if (len > NETHDR)
	net_rx (&v->nd, buf + NETHDR, len - NETHDR);
      virtq_free_chain (&v->rxq, id);
      rx_post (v, id);
    }
}

static void
vnet_poll (struct netdev *nd)
{
  struct vnet *v = nd->drv;
  vnet_rx (v);
  tx_reap (v);
}

static void
vnet_irq (unsigned irq, void *arg)
{
  struct vnet *v = arg;

  (void) inb (v->io + VIRTIO_PCI_ISR);
  vnet_rx (v);
  tx_reap (v);
}

static const struct netdev_ops vnet_ops = {
  .xmit = vnet_xmit,
  .poll = vnet_poll,
};

bool
virtio_net_probe (struct pci_dev *d)
{
  struct vnet *v;
  uint32_t feat;

  if (d->vendor != VIRTIO_VENDOR || (d->device != 0x1000
				     && d->device != 0x1041))
    return false;
  v = (struct vnet *) kmalloc (sizeof (*v));
  KASSERT (v != NULL);
  memset (v, 0, sizeof (*v));
  v->io = virtio_begin (d);
  if (v->io == 0)
    {
      kprintf ("virtio-net: no legacy I/O interface\n");
      return false;
    }
  feat = inl (v->io + VIRTIO_PCI_HOST_FEATURES);
  outl (v->io + VIRTIO_PCI_GUEST_FEATURES, feat & VIRTIO_NET_F_MAC);
  if (feat & VIRTIO_NET_F_MAC)
    for (int i = 0; i < 6; i++)
      v->nd.mac[i] = inb (v->io + VIRTIO_PCI_CONFIG + i);
  else
    {
      static const uint8_t def[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
      memcpy (v->nd.mac, def, 6);
    }
  if (virtq_init (v->io, &v->rxq, 0) < 0 || virtq_init (v->io, &v->txq, 1) < 0)
    {
      outb (v->io + VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
      return false;
    }
  if (dma_alloc (&v->rxbufs, v->rxq.num * BUFSZ) < 0
      || dma_alloc (&v->txbufs, NTXBUF * BUFSZ) < 0)
    return false;
  virtio_ready (v->io);
  for (unsigned i = 0; i < v->rxq.num && i < 256; i++)
    rx_post (v, i);

  strlcpy (v->nd.name, "virtio-net", sizeof (v->nd.name));
  v->nd.mtu = 1500;
  v->nd.ops = &vnet_ops;
  v->nd.drv = v;
  v->nd.link = true;
  net_register (&v->nd);
  irq_register (d->irq, vnet_irq, v);
  return true;
}
