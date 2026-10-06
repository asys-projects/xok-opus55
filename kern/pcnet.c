/*
 * AMD PCnet-PCI II (Am79C970A, QEMU "pcnet") Ethernet driver.
 * Word I/O mode, 32-bit software style (SWSTYLE 2) descriptor rings.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "dev.h"
#include "net.h"

#define P_APROM  0x00
#define P_RDP    0x10
#define P_RAP    0x12
#define P_RESET  0x14
#define P_BDP    0x16

#define CSR0_INIT 0x0001
#define CSR0_STRT 0x0002
#define CSR0_TDMD 0x0008
#define CSR0_IENA 0x0040
#define CSR0_IDON 0x0100
#define CSR0_TINT 0x0200
#define CSR0_RINT 0x0400
#define CSR0_ERRS 0xf800

#define D_OWN 0x8000
#define D_ERR 0x4000
#define D_STP 0x0200
#define D_ENP 0x0100

#define NRX 32
#define NTX 16
#define BUFSZ 1536

struct pcnet_desc
{
  uint32_t addr;
  int16_t bcnt;
  volatile uint16_t status;
  volatile uint32_t mcnt;
  uint32_t user;
};

struct pcnet_init
{
  uint16_t mode;
  uint8_t rlen, tlen;
  uint8_t padr[6];
  uint16_t rsvd;
  uint32_t ladrf[2];
  uint32_t rdra, tdra;
};

struct pcnet
{
  struct netdev nd;
  uint16_t io;
  struct dmabuf ring, bufs, initb;
  volatile struct pcnet_desc *rx, *tx;
  unsigned rxnext, txnext;
};

static uint16_t
csr_read (struct pcnet *p, unsigned r)
{
  outw (p->io + P_RAP, r);
  return inw (p->io + P_RDP);
}

static void
csr_write (struct pcnet *p, unsigned r, uint16_t v)
{
  outw (p->io + P_RAP, r);
  outw (p->io + P_RDP, v);
}

static void
bcr_write (struct pcnet *p, unsigned r, uint16_t v)
{
  outw (p->io + P_RAP, r);
  outw (p->io + P_BDP, v);
}

static uint8_t *
rxbuf (struct pcnet *p, unsigned i)
{
  return (uint8_t *) p->bufs.va + i * BUFSZ;
}

static uint8_t *
txbuf (struct pcnet *p, unsigned i)
{
  return (uint8_t *) p->bufs.va + (NRX + i) * BUFSZ;
}

static int
pcnet_xmit (struct netdev *nd, const void *frame, unsigned len)
{
  struct pcnet *p = nd->drv;
  volatile struct pcnet_desc *d = &p->tx[p->txnext];

  if (len > BUFSZ)
    return -E_INVAL;
  if (d->status & D_OWN)
    return -E_FULL;
  memcpy (txbuf (p, p->txnext), frame, len);
  d->bcnt = -(int16_t) len;
  d->mcnt = 0;
  __atomic_thread_fence (__ATOMIC_SEQ_CST);
  d->status = D_OWN | D_STP | D_ENP;
  p->txnext = (p->txnext + 1) % NTX;
  csr_write (p, 0, CSR0_TDMD | CSR0_IENA);
  return 0;
}

static void
pcnet_rx (struct pcnet *p)
{
  for (int n = 0; n < NRX; n++)
    {
      volatile struct pcnet_desc *d = &p->rx[p->rxnext];
      uint16_t st = d->status;

      if (st & D_OWN)
	break;
      if ((st & (D_ERR | D_STP | D_ENP)) == (D_STP | D_ENP))
	{
	  unsigned len = d->mcnt & 0xfff;
	  if (len > 4)
	    net_rx (&p->nd, rxbuf (p, p->rxnext), len - 4);	/* FCS. */
	}
      d->bcnt = -BUFSZ;
      d->mcnt = 0;
      __atomic_thread_fence (__ATOMIC_SEQ_CST);
      d->status = D_OWN;
      p->rxnext = (p->rxnext + 1) % NRX;
    }
}

static void
pcnet_poll (struct netdev *nd)
{
  pcnet_rx (nd->drv);
}

static void
pcnet_irq (unsigned irq, void *arg)
{
  struct pcnet *p = arg;
  uint16_t csr0 = csr_read (p, 0);

  /* Acknowledge (write-one-to-clear) and keep interrupts enabled. */
  csr_write (p, 0, (csr0 & (CSR0_ERRS | CSR0_RINT | CSR0_TINT | CSR0_IDON))
	     | CSR0_IENA);
  if (csr0 & CSR0_RINT)
    pcnet_rx (p);
}

static const struct netdev_ops pcnet_ops = {
  .xmit = pcnet_xmit,
  .poll = pcnet_poll,
};

bool
pcnet_probe (struct pci_dev *d)
{
  struct pcnet *p;
  struct pcnet_init *ib;

  if (d->vendor != 0x1022 || d->device != 0x2000 || !d->bar_io[0])
    return false;
  pci_enable (d, true);
  p = (struct pcnet *) kmalloc (sizeof (*p));
  KASSERT (p != NULL);
  memset (p, 0, sizeof (*p));
  p->io = d->bar[0];
  if (dma_alloc (&p->ring, (NRX + NTX) * sizeof (struct pcnet_desc)) < 0
      || dma_alloc (&p->bufs, (NRX + NTX) * BUFSZ) < 0
      || dma_alloc (&p->initb, sizeof (struct pcnet_init)) < 0)
    return false;

  inw (p->io + P_RESET);	/* Reset; word I/O mode. */
  outw (p->io + P_RAP, 0);
  for (int i = 0; i < 6; i++)
    p->nd.mac[i] = inb (p->io + P_APROM + i);
  bcr_write (p, 20, 2);		/* SWSTYLE 2: 32-bit structures. */

  p->rx = p->ring.va;
  p->tx = p->rx + NRX;
  for (unsigned i = 0; i < NRX; i++)
    {
      p->rx[i].addr = p->bufs.pa + i * BUFSZ;
      p->rx[i].bcnt = -BUFSZ;
      p->rx[i].status = D_OWN;
    }
  for (unsigned i = 0; i < NTX; i++)
    {
      p->tx[i].addr = p->bufs.pa + (NRX + i) * BUFSZ;
      p->tx[i].status = 0;
    }

  ib = p->initb.va;
  memset (ib, 0, sizeof (*ib));
  ib->mode = 0;
  ib->rlen = 5 << 4;		/* log2 (NRX) */
  ib->tlen = 4 << 4;		/* log2 (NTX) */
  memcpy (ib->padr, p->nd.mac, 6);
  ib->ladrf[0] = ib->ladrf[1] = 0xffffffff;
  ib->rdra = p->ring.pa;
  ib->tdra = p->ring.pa + NRX * sizeof (struct pcnet_desc);

  csr_write (p, 1, p->initb.pa & 0xffff);
  csr_write (p, 2, p->initb.pa >> 16);
  csr_write (p, 3, 0x0100);	/* Mask initialisation done. */
  csr_write (p, 4, csr_read (p, 4) | 0x0800);	/* Pad short frames. */
  csr_write (p, 0, CSR0_INIT);
  for (uint64_t t0 = ktime (); !(csr_read (p, 0) & CSR0_IDON);)
    if (ktime () - t0 > 1000000000ULL)
      {
	kprintf ("pcnet: initialisation timed out\n");
	return false;
      }
  csr_write (p, 0, CSR0_IDON | CSR0_STRT | CSR0_IENA);

  strlcpy (p->nd.name, "pcnet", sizeof (p->nd.name));
  p->nd.mtu = 1500;
  p->nd.ops = &pcnet_ops;
  p->nd.drv = p;
  p->nd.link = true;
  net_register (&p->nd);
  irq_register (d->irq, pcnet_irq, p);
  return true;
}
