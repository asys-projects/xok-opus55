/*
 * NE2000-compatible Ethernet (RTL8029 PCI, QEMU ne2k_pci) driver.
 * Programmed I/O through the remote DMA port, 16-bit mode.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "dev.h"
#include "net.h"

#define NE_CR    0x00
#define NE_PSTART 0x01
#define NE_PSTOP 0x02
#define NE_BNRY  0x03
#define NE_TPSR  0x04
#define NE_TBCR0 0x05
#define NE_TBCR1 0x06
#define NE_ISR   0x07
#define NE_RSAR0 0x08
#define NE_RSAR1 0x09
#define NE_RBCR0 0x0a
#define NE_RBCR1 0x0b
#define NE_RCR   0x0c
#define NE_TCR   0x0d
#define NE_DCR   0x0e
#define NE_IMR   0x0f
#define NE_DATA  0x10
#define NE_RESET 0x1f
/* Page 1. */
#define NE_PAR0  0x01
#define NE_CURR  0x07
#define NE_MAR0  0x08

#define CR_STP   0x01
#define CR_STA   0x02
#define CR_TXP   0x04
#define CR_RD_READ 0x08
#define CR_RD_WRITE 0x10
#define CR_RD_ABORT 0x20
#define CR_PAGE1 0x40

#define ISR_PRX  0x01
#define ISR_PTX  0x02
#define ISR_RXE  0x04
#define ISR_TXE  0x08
#define ISR_OVW  0x10
#define ISR_RDC  0x40
#define ISR_RST  0x80

#define TX_PAGE  0x40
#define RX_START 0x46
#define RX_STOP  0x80

struct ne2k
{
  struct netdev nd;
  uint16_t io;
  uint8_t buf[1600];
};

static void
dma_read (struct ne2k *n, uint16_t addr, void *dst, unsigned len)
{
  uint16_t *d = dst;

  len = (len + 1) & ~1u;
  outb (n->io + NE_RBCR0, len & 0xff);
  outb (n->io + NE_RBCR1, len >> 8);
  outb (n->io + NE_RSAR0, addr & 0xff);
  outb (n->io + NE_RSAR1, addr >> 8);
  outb (n->io + NE_CR, CR_RD_READ | CR_STA);
  for (unsigned i = 0; i < len / 2; i++)
    d[i] = inw (n->io + NE_DATA);
  outb (n->io + NE_ISR, ISR_RDC);
}

static int
ne2k_xmit (struct netdev *nd, const void *frame, unsigned len)
{
  struct ne2k *n = nd->drv;
  const uint16_t *s = frame;
  unsigned wlen = (len + 1) & ~1u;

  if (len > 1514)
    return -E_INVAL;
  if (inb (n->io + NE_CR) & CR_TXP)
    return -E_FULL;
  if (len & 1)
    {
      memcpy (n->buf, frame, len);
      n->buf[len] = 0;
      s = (const uint16_t *) n->buf;
    }
  outb (n->io + NE_ISR, ISR_RDC);
  outb (n->io + NE_RBCR0, wlen & 0xff);
  outb (n->io + NE_RBCR1, wlen >> 8);
  outb (n->io + NE_RSAR0, 0);
  outb (n->io + NE_RSAR1, TX_PAGE);
  outb (n->io + NE_CR, CR_RD_WRITE | CR_STA);
  for (unsigned i = 0; i < wlen / 2; i++)
    outw (n->io + NE_DATA, s[i]);
  for (int i = 0; i < 100000 && !(inb (n->io + NE_ISR) & ISR_RDC); i++)
    ;
  outb (n->io + NE_ISR, ISR_RDC);
  outb (n->io + NE_TPSR, TX_PAGE);
  outb (n->io + NE_TBCR0, len & 0xff);
  outb (n->io + NE_TBCR1, len >> 8);
  outb (n->io + NE_CR, CR_RD_ABORT | CR_TXP | CR_STA);
  return 0;
}

static void
ne2k_rx (struct ne2k *n)
{
  for (int k = 0; k < 64; k++)
    {
      uint8_t curr, bnry, next;
      uint8_t hdr[4];
      unsigned len;

      outb (n->io + NE_CR, CR_PAGE1 | CR_RD_ABORT | CR_STA);
      curr = inb (n->io + NE_CURR);
      outb (n->io + NE_CR, CR_RD_ABORT | CR_STA);
      bnry = inb (n->io + NE_BNRY);
      next = bnry + 1;
      if (next >= RX_STOP)
	next = RX_START;
      if (next == curr)
	break;
      dma_read (n, next << 8, hdr, 4);
      len = hdr[2] | (hdr[3] << 8);
      if (len >= 64 && len <= 1518 + 4 && (hdr[0] & 0x01))
	{
	  dma_read (n, (next << 8) + 4, n->buf, len - 4);
	  net_rx (&n->nd, n->buf, len - 4);
	}
      if (hdr[1] < RX_START || hdr[1] >= RX_STOP)
	{
	  /* Corrupted ring: restart from the current page. */
	  outb (n->io + NE_BNRY, curr == RX_START ? RX_STOP - 1 : curr - 1);
	  break;
	}
      outb (n->io + NE_BNRY, hdr[1] == RX_START ? RX_STOP - 1 : hdr[1] - 1);
    }
}

static void
ne2k_poll (struct netdev *nd)
{
  ne2k_rx (nd->drv);
}

static void
ne2k_irq (unsigned irq, void *arg)
{
  struct ne2k *n = arg;
  uint8_t isr = inb (n->io + NE_ISR);

  outb (n->io + NE_ISR, isr & ~ISR_RDC);
  if (isr & (ISR_PRX | ISR_RXE | ISR_OVW))
    ne2k_rx (n);
}

static const struct netdev_ops ne2k_ops = {
  .xmit = ne2k_xmit,
  .poll = ne2k_poll,
};

bool
ne2k_probe (struct pci_dev *d)
{
  struct ne2k *n;
  uint8_t prom[32];

  if (d->vendor != 0x10ec || d->device != 0x8029 || !d->bar_io[0])
    return false;
  pci_enable (d, true);
  n = (struct ne2k *) kmalloc (sizeof (*n));
  KASSERT (n != NULL);
  memset (n, 0, sizeof (*n));
  n->io = d->bar[0];

  outb (n->io + NE_RESET, inb (n->io + NE_RESET));
  for (int i = 0; i < 100000 && !(inb (n->io + NE_ISR) & ISR_RST); i++)
    ;
  outb (n->io + NE_ISR, 0xff);
  outb (n->io + NE_CR, CR_RD_ABORT | CR_STP);
  outb (n->io + NE_DCR, 0x49);	/* Word transfers, FIFO threshold 8. */
  outb (n->io + NE_RBCR0, 0);
  outb (n->io + NE_RBCR1, 0);
  outb (n->io + NE_RCR, 0x04);	/* Accept broadcast. */
  outb (n->io + NE_TCR, 0x02);	/* Loopback while configuring. */
  outb (n->io + NE_TPSR, TX_PAGE);
  outb (n->io + NE_PSTART, RX_START);
  outb (n->io + NE_PSTOP, RX_STOP);
  outb (n->io + NE_BNRY, RX_START);
  outb (n->io + NE_IMR, 0);
  outb (n->io + NE_ISR, 0xff);

  /* Station address PROM (bytes doubled in word mode). */
  dma_read (n, 0, prom, sizeof (prom));
  for (int i = 0; i < 6; i++)
    n->nd.mac[i] = prom[2 * i];

  outb (n->io + NE_CR, CR_PAGE1 | CR_RD_ABORT | CR_STP);
  for (int i = 0; i < 6; i++)
    outb (n->io + NE_PAR0 + i, n->nd.mac[i]);
  for (int i = 0; i < 8; i++)
    outb (n->io + NE_MAR0 + i, 0xff);
  outb (n->io + NE_CURR, RX_START + 1);
  outb (n->io + NE_CR, CR_RD_ABORT | CR_STA);
  outb (n->io + NE_TCR, 0x00);
  outb (n->io + NE_ISR, 0xff);
  outb (n->io + NE_IMR, ISR_PRX | ISR_RXE | ISR_OVW);

  strlcpy (n->nd.name, "ne2k-pci", sizeof (n->nd.name));
  n->nd.mtu = 1500;
  n->nd.ops = &ne2k_ops;
  n->nd.drv = n;
  n->nd.link = true;
  net_register (&n->nd);
  irq_register (d->irq, ne2k_irq, n);
  return true;
}
