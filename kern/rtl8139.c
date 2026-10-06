/*
 * Realtek RTL8139 Ethernet driver.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "dev.h"
#include "net.h"

#define R_IDR0   0x00
#define R_TSD0   0x10
#define R_TSAD0  0x20
#define R_RBSTART 0x30
#define R_CMD    0x37
#define R_CAPR   0x38
#define R_CBR    0x3a
#define R_IMR    0x3c
#define R_ISR    0x3e
#define R_TCR    0x40
#define R_RCR    0x44
#define R_CONFIG1 0x52
#define R_MSR    0x58

#define CMD_BUFE 0x01
#define CMD_TE   0x04
#define CMD_RE   0x08
#define CMD_RST  0x10

#define ISR_ROK  0x0001
#define ISR_RER  0x0002
#define ISR_TOK  0x0004
#define ISR_TER  0x0008
#define ISR_RXOVW 0x0010

#define RXBUF_LEN 8192
#define TXBUF_LEN 2048

struct rtl
{
  struct netdev nd;
  uint16_t io;
  struct dmabuf rxbuf, txbuf;
  unsigned rxoff;
  unsigned txnext;
};

static int
rtl_xmit (struct netdev *nd, const void *frame, unsigned len)
{
  struct rtl *r = nd->drv;
  unsigned i = r->txnext;
  uint32_t tsd = inl (r->io + R_TSD0 + 4 * i);

  if (len > 1792)
    return -E_INVAL;
  /* OWN set: the descriptor is free (DMA to the FIFO completed). */
  if (!(tsd & (1 << 13)) && (tsd & 0x1fff))
    return -E_FULL;
  memcpy ((uint8_t *) r->txbuf.va + i * TXBUF_LEN, frame, len);
  outl (r->io + R_TSAD0 + 4 * i, r->txbuf.pa + i * TXBUF_LEN);
  outl (r->io + R_TSD0 + 4 * i, len);
  r->txnext = (i + 1) % 4;
  return 0;
}

static void
rtl_rx (struct rtl *r)
{
  for (int n = 0; n < 64 && !(inb (r->io + R_CMD) & CMD_BUFE); n++)
    {
      uint8_t *p = (uint8_t *) r->rxbuf.va + r->rxoff;
      uint16_t status = p[0] | (p[1] << 8);
      uint16_t len = p[2] | (p[3] << 8);

      if (!(status & 1) || len < 64 || len > 1522)
	{
	  /* Corrupted ring: reset the receiver. */
	  outb (r->io + R_CMD, CMD_TE);
	  outb (r->io + R_CMD, CMD_TE | CMD_RE);
	  r->rxoff = 0;
	  outw (r->io + R_CAPR, 0xfff0);
	  break;
	}
      net_rx (&r->nd, p + 4, len - 4);
      r->rxoff = (r->rxoff + len + 4 + 3) & ~3u;
      if (r->rxoff >= RXBUF_LEN)
	r->rxoff -= RXBUF_LEN;
      outw (r->io + R_CAPR, r->rxoff - 16);
    }
}

static void
rtl_poll (struct netdev *nd)
{
  struct rtl *r = nd->drv;
  rtl_rx (r);
  net_link (nd, !(inb (r->io + R_MSR) & 0x04));
}

static void
rtl_irq (unsigned irq, void *arg)
{
  struct rtl *r = arg;
  uint16_t isr = inw (r->io + R_ISR);

  outw (r->io + R_ISR, isr);
  if (isr & (ISR_ROK | ISR_RER | ISR_RXOVW))
    rtl_rx (r);
}

static const struct netdev_ops rtl_ops = {
  .xmit = rtl_xmit,
  .poll = rtl_poll,
};

bool
rtl8139_probe (struct pci_dev *d)
{
  struct rtl *r;

  if (d->vendor != 0x10ec || d->device != 0x8139)
    return false;
  if (!d->bar_io[0])
    return false;
  pci_enable (d, true);
  r = (struct rtl *) kmalloc (sizeof (*r));
  KASSERT (r != NULL);
  memset (r, 0, sizeof (*r));
  r->io = d->bar[0];

  outb (r->io + R_CONFIG1, 0);
  outb (r->io + R_CMD, CMD_RST);
  for (int i = 0; i < 100000 && (inb (r->io + R_CMD) & CMD_RST); i++)
    ;
  for (int i = 0; i < 6; i++)
    r->nd.mac[i] = inb (r->io + R_IDR0 + i);
  /* Receive ring: 8K + 16 bytes + room for a wrapped packet. */
  if (dma_alloc (&r->rxbuf, RXBUF_LEN + 16 + 1536) < 0
      || dma_alloc (&r->txbuf, 4 * TXBUF_LEN) < 0)
    return false;
  outl (r->io + R_RBSTART, r->rxbuf.pa);
  outw (r->io + R_IMR, ISR_ROK | ISR_RER | ISR_TOK | ISR_TER | ISR_RXOVW);
  /* Accept broadcast, multicast, physical match; WRAP; 8K ring. */
  outl (r->io + R_RCR, 0x0e | (1 << 7));
  outl (r->io + R_TCR, 0x03000600);
  outb (r->io + R_CMD, CMD_RE | CMD_TE);
  outw (r->io + R_CAPR, 0xfff0);

  strlcpy (r->nd.name, "rtl8139", sizeof (r->nd.name));
  r->nd.mtu = 1500;
  r->nd.ops = &rtl_ops;
  r->nd.drv = r;
  r->nd.link = !(inb (r->io + R_MSR) & 0x04);
  net_register (&r->nd);
  irq_register (d->irq, rtl_irq, r);
  return true;
}
