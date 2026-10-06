/*
 * Intel 8254x (e1000) and 82574 (e1000e) Ethernet driver, using legacy
 * descriptors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "dev.h"
#include "net.h"

#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR    0x00c0
#define REG_IMS    0x00d0
#define REG_IMC    0x00d8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define CTRL_SLU   (1 << 6)
#define CTRL_ASDE  (1 << 5)
#define CTRL_RST   (1 << 26)
#define STATUS_LU  (1 << 1)

#define RCTL_EN    (1 << 1)
#define RCTL_BAM   (1 << 15)
#define RCTL_SECRC (1 << 26)

#define TCTL_EN    (1 << 1)
#define TCTL_PSP   (1 << 3)

#define ICR_TXDW   (1 << 0)
#define ICR_LSC    (1 << 2)
#define ICR_RXDMT0 (1 << 4)
#define ICR_RXO    (1 << 6)
#define ICR_RXT0   (1 << 7)

#define NRX 64
#define NTX 64
#define BUFSZ 2048

struct rxdesc
{
  uint64_t addr;
  uint16_t len;
  uint16_t csum;
  uint8_t status;
  uint8_t errors;
  uint16_t special;
} __attribute__ ((packed));

struct txdesc
{
  uint64_t addr;
  uint16_t len;
  uint8_t cso;
  uint8_t cmd;
  uint8_t status;
  uint8_t css;
  uint16_t special;
} __attribute__ ((packed));

struct e1000
{
  struct netdev nd;
  volatile uint8_t *regs;
  struct dmabuf rxring, txring, rxbufs, txbufs;
  volatile struct rxdesc *rx;
  volatile struct txdesc *tx;
  unsigned rxnext, txnext;
};

static inline uint32_t
rd (struct e1000 *e, uint32_t r)
{
  return mmio_read32 (e->regs, r);
}

static inline void
wr (struct e1000 *e, uint32_t r, uint32_t v)
{
  mmio_write32 (e->regs, r, v);
}

static int
e1000_xmit (struct netdev *nd, const void *frame, unsigned len)
{
  struct e1000 *e = nd->drv;
  unsigned i = e->txnext;
  volatile struct txdesc *d = &e->tx[i];

  if (len > BUFSZ)
    return -E_INVAL;
  if (!(d->status & 1) && d->cmd != 0)
    return -E_FULL;		/* Descriptor still owned by the card. */
  memcpy ((uint8_t *) e->txbufs.va + i * BUFSZ, frame, len);
  d->addr = e->txbufs.pa + i * BUFSZ;
  d->len = len;
  d->cso = 0;
  d->status = 0;
  d->cmd = 0x0b;		/* EOP | IFCS | RS */
  e->txnext = (i + 1) % NTX;
  __atomic_thread_fence (__ATOMIC_SEQ_CST);
  wr (e, REG_TDT, e->txnext);
  return 0;
}

static void
e1000_rx (struct e1000 *e)
{
  for (int n = 0; n < NRX; n++)
    {
      unsigned i = e->rxnext;
      volatile struct rxdesc *d = &e->rx[i];

      if (!(d->status & 1))
	break;
      if ((d->status & 2) && d->errors == 0 && d->len <= BUFSZ)
	net_rx (&e->nd, (uint8_t *) e->rxbufs.va + i * BUFSZ, d->len);
      d->status = 0;
      e->rxnext = (i + 1) % NRX;
      __atomic_thread_fence (__ATOMIC_SEQ_CST);
      wr (e, REG_RDT, i);
    }
}

static void
e1000_poll (struct netdev *nd)
{
  struct e1000 *e = nd->drv;

  e1000_rx (e);
  net_link (nd, (rd (e, REG_STATUS) & STATUS_LU) != 0);
}

static void
e1000_irq (unsigned irq, void *arg)
{
  struct e1000 *e = arg;
  uint32_t icr = rd (e, REG_ICR);

  if (icr & ICR_LSC)
    net_link (&e->nd, (rd (e, REG_STATUS) & STATUS_LU) != 0);
  if (icr & (ICR_RXT0 | ICR_RXO | ICR_RXDMT0))
    e1000_rx (e);
}

static const struct netdev_ops e1000_ops = {
  .xmit = e1000_xmit,
  .poll = e1000_poll,
};

static uint16_t
eeprom_read (struct e1000 *e, unsigned addr, bool e1000e)
{
  uint32_t v;

  if (e1000e)
    wr (e, REG_EERD, 1 | (addr << 2));
  else
    wr (e, REG_EERD, 1 | (addr << 8));
  for (int i = 0; i < 100000; i++)
    {
      v = rd (e, REG_EERD);
      if (v & (e1000e ? (1 << 1) : (1 << 4)))
	return v >> 16;
    }
  return 0;
}

bool
e1000_probe (struct pci_dev *d)
{
  struct e1000 *e;
  bool e1000e;
  uint32_t ral, rah;

  if (d->vendor != 0x8086)
    return false;
  switch (d->device)
    {
    case 0x100e:		/* 82540EM (QEMU e1000) */
    case 0x100f:		/* 82545EM */
    case 0x1004:
    case 0x1026:		/* 82545GM (QEMU e1000-82545em) */
    case 0x1076:
    case 0x107c:
      e1000e = false;
      break;
    case 0x10d3:		/* 82574L (QEMU e1000e) */
      e1000e = true;
      break;
    default:
      return false;
    }
  pci_enable (d, true);
  e = (struct e1000 *) kmalloc (sizeof (*e));
  KASSERT (e != NULL);
  memset (e, 0, sizeof (*e));
  e->regs = pci_map_bar (d, 0);
  if (e->regs == NULL)
    return false;

  wr (e, REG_IMC, 0xffffffff);
  wr (e, REG_CTRL, rd (e, REG_CTRL) | CTRL_RST);
  delay_us (10000);
  for (int i = 0; i < 100000 && (rd (e, REG_CTRL) & CTRL_RST); i++)
    ;
  wr (e, REG_IMC, 0xffffffff);
  (void) rd (e, REG_ICR);
  wr (e, REG_CTRL, rd (e, REG_CTRL) | CTRL_SLU | CTRL_ASDE);

  /* MAC address: receive address registers, or the EEPROM. */
  ral = rd (e, REG_RAL);
  rah = rd (e, REG_RAH);
  if ((rah & (1u << 31)) && (ral || (rah & 0xffff)))
    {
      for (int i = 0; i < 4; i++)
	e->nd.mac[i] = ral >> (8 * i);
      e->nd.mac[4] = rah;
      e->nd.mac[5] = rah >> 8;
    }
  else
    for (int i = 0; i < 3; i++)
      {
	uint16_t w = eeprom_read (e, i, e1000e);
	e->nd.mac[2 * i] = w;
	e->nd.mac[2 * i + 1] = w >> 8;
      }
  wr (e, REG_RAL, e->nd.mac[0] | (e->nd.mac[1] << 8) | (e->nd.mac[2] << 16)
      | ((uint32_t) e->nd.mac[3] << 24));
  wr (e, REG_RAH, e->nd.mac[4] | (e->nd.mac[5] << 8) | (1u << 31));
  for (int i = 0; i < 128; i++)
    wr (e, REG_MTA + 4 * i, 0);

  if (dma_alloc (&e->rxring, NRX * sizeof (struct rxdesc)) < 0
      || dma_alloc (&e->txring, NTX * sizeof (struct txdesc)) < 0
      || dma_alloc (&e->rxbufs, NRX * BUFSZ) < 0
      || dma_alloc (&e->txbufs, NTX * BUFSZ) < 0)
    return false;
  e->rx = e->rxring.va;
  e->tx = e->txring.va;
  for (int i = 0; i < NRX; i++)
    e->rx[i].addr = e->rxbufs.pa + i * BUFSZ;
  for (int i = 0; i < NTX; i++)
    {
      e->tx[i].status = 1;	/* Done. */
      e->tx[i].cmd = 0;
    }

  wr (e, REG_RDBAL, e->rxring.pa);
  wr (e, REG_RDBAH, 0);
  wr (e, REG_RDLEN, NRX * sizeof (struct rxdesc));
  wr (e, REG_RDH, 0);
  wr (e, REG_RDT, NRX - 1);
  wr (e, REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

  wr (e, REG_TDBAL, e->txring.pa);
  wr (e, REG_TDBAH, 0);
  wr (e, REG_TDLEN, NTX * sizeof (struct txdesc));
  wr (e, REG_TDH, 0);
  wr (e, REG_TDT, 0);
  wr (e, REG_TIPG, 10 | (8 << 10) | (6 << 20));
  wr (e, REG_TCTL, TCTL_EN | TCTL_PSP | (0x0f << 4) | (0x40 << 12));

  strlcpy (e->nd.name, e1000e ? "e1000e" : "e1000", sizeof (e->nd.name));
  e->nd.mtu = 1500;
  e->nd.ops = &e1000_ops;
  e->nd.drv = e;
  e->nd.link = (rd (e, REG_STATUS) & STATUS_LU) != 0;
  net_register (&e->nd);
  irq_register (d->irq, e1000_irq, e);
  wr (e, REG_IMS, ICR_RXT0 | ICR_RXO | ICR_RXDMT0 | ICR_LSC);
  return true;
}
