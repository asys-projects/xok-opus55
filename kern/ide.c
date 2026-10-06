/*
 * ATA disk driver for PCI IDE controllers (PIIX and compatibles), with
 * bus-master DMA, LBA28/LBA48 and a PIO fallback.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "dev.h"
#include "blk.h"

#define ATA_DATA    0
#define ATA_ERROR   1
#define ATA_COUNT   2
#define ATA_LBA0    3
#define ATA_LBA1    4
#define ATA_LBA2    5
#define ATA_DEVICE  6
#define ATA_STATUS  7
#define ATA_CMD     7

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DF   0x20
#define ST_DRDY 0x40
#define ST_BSY  0x80

#define CMD_IDENTIFY      0xec
#define CMD_READ_DMA      0xc8
#define CMD_WRITE_DMA     0xca
#define CMD_READ_DMA_EXT  0x25
#define CMD_WRITE_DMA_EXT 0x35
#define CMD_READ_PIO      0x20
#define CMD_WRITE_PIO     0x30
#define CMD_READ_PIO_EXT  0x24
#define CMD_WRITE_PIO_EXT 0x34
#define CMD_FLUSH         0xe7
#define CMD_FLUSH_EXT     0xea

#define BM_CMD    0
#define BM_STATUS 2
#define BM_PRD    4

struct prd
{
  uint32_t addr;
  uint16_t count;
  uint16_t flags;
} __attribute__ ((packed));

struct ide_channel;

struct ide_drive
{
  struct blkdev blk;
  struct ide_channel *ch;
  unsigned slave;
  bool lba48;
  bool dma;
};

struct ide_channel
{
  uint16_t cmd, ctl, bm;
  unsigned irq;
  struct dmabuf prdbuf;
  struct ide_drive *active;
  bool active_dma;
};

static int
ide_wait (struct ide_channel *ch, bool drq)
{
  for (int i = 0; i < 1000000; i++)
    {
      uint8_t st = inb (ch->cmd + ATA_STATUS);
      if (st & ST_BSY)
	continue;
      if (st & (ST_ERR | ST_DF))
	return -1;
      if (!drq || (st & ST_DRQ))
	return 0;
    }
  return -1;
}

static void
ide_select (struct ide_channel *ch, unsigned slave, uint8_t bits)
{
  outb (ch->cmd + ATA_DEVICE, bits | (slave << 4));
  for (int i = 0; i < 4; i++)
    (void) inb (ch->ctl);	/* 400ns delay. */
}

static void
ide_setup_lba (struct ide_drive *dr, uint64_t lba, uint32_t n)
{
  struct ide_channel *ch = dr->ch;

  if (dr->lba48)
    {
      ide_select (ch, dr->slave, 0x40);
      outb (ch->cmd + ATA_COUNT, (n >> 8) & 0xff);
      outb (ch->cmd + ATA_LBA0, (lba >> 24) & 0xff);
      outb (ch->cmd + ATA_LBA1, (lba >> 32) & 0xff);
      outb (ch->cmd + ATA_LBA2, (lba >> 40) & 0xff);
      outb (ch->cmd + ATA_COUNT, n & 0xff);
      outb (ch->cmd + ATA_LBA0, lba & 0xff);
      outb (ch->cmd + ATA_LBA1, (lba >> 8) & 0xff);
      outb (ch->cmd + ATA_LBA2, (lba >> 16) & 0xff);
    }
  else
    {
      ide_select (ch, dr->slave, 0xe0 | ((lba >> 24) & 0x0f));
      outb (ch->cmd + ATA_COUNT, n & 0xff);
      outb (ch->cmd + ATA_LBA0, lba & 0xff);
      outb (ch->cmd + ATA_LBA1, (lba >> 8) & 0xff);
      outb (ch->cmd + ATA_LBA2, (lba >> 16) & 0xff);
    }
}

/* PIO transfer, performed synchronously. */
static int
ide_pio (struct ide_drive *dr, struct bio *b)
{
  struct ide_channel *ch = dr->ch;
  unsigned s = 0;

  ide_setup_lba (dr, b->sector, b->nsect);
  outb (ch->cmd + ATA_CMD, b->write
	? (dr->lba48 ? CMD_WRITE_PIO_EXT : CMD_WRITE_PIO)
	: (dr->lba48 ? CMD_READ_PIO_EXT : CMD_READ_PIO));
  for (unsigned i = 0; i < b->nsegs; i++)
    for (uint32_t off = 0; off < b->seg[i].len; off += SECTOR_SIZE, s++)
      {
	paddr_t pa = b->seg[i].pa + off;
	pfn_t pfn = pa >> PAGE_SHIFT;
	uint8_t *va;

	if (ide_wait (ch, true) < 0)
	  return -E_IO;
	va = pfn_get (pfn);
	if (b->write)
	  outsw (ch->cmd + ATA_DATA, va + (pa & PAGE_MASK), 256);
	else
	  insw (ch->cmd + ATA_DATA, va + (pa & PAGE_MASK), 256);
	pfn_put (pfn, va);
      }
  if (b->write)
    {
      if (ide_wait (ch, false) < 0)
	return -E_IO;
      outb (ch->cmd + ATA_CMD, dr->lba48 ? CMD_FLUSH_EXT : CMD_FLUSH);
    }
  return ide_wait (ch, false) < 0 ? -E_IO : 0;
}

static int
ide_start (struct blkdev *d, struct bio *b)
{
  struct ide_drive *dr = d->drv;
  struct ide_channel *ch = dr->ch;
  struct prd *prd = ch->prdbuf.va;

  if (!dr->dma)
    {
      int r = ide_pio (dr, b);
      ch->active = NULL;
      if (r == 0)
	{
	  /* Complete synchronously: blk_complete expects the device to be
	     active. */
	  ch->active = dr;
	  ch->active_dma = false;
	  blk_complete (d, 0);
	  ch->active = NULL;
	  return 0;
	}
      return r;
    }

  for (unsigned i = 0; i < b->nsegs; i++)
    {
      prd[i].addr = b->seg[i].pa;
      prd[i].count = b->seg[i].len;
      prd[i].flags = (i == b->nsegs - 1) ? 0x8000 : 0;
    }
  outb (ch->bm + BM_CMD, 0);
  outl (ch->bm + BM_PRD, ch->prdbuf.pa);
  outb (ch->bm + BM_STATUS, inb (ch->bm + BM_STATUS) | 0x06);
  outb (ch->bm + BM_CMD, b->write ? 0x00 : 0x08);
  if (ide_wait (ch, false) < 0)
    return -E_IO;
  ide_setup_lba (dr, b->sector, b->nsect);
  ch->active = dr;
  ch->active_dma = true;
  outb (ch->cmd + ATA_CMD, b->write
	? (dr->lba48 ? CMD_WRITE_DMA_EXT : CMD_WRITE_DMA)
	: (dr->lba48 ? CMD_READ_DMA_EXT : CMD_READ_DMA));
  outb (ch->bm + BM_CMD, inb (ch->bm + BM_CMD) | 0x01);
  return 0;
}

static bool
ide_check (struct ide_channel *ch)
{
  struct ide_drive *dr = ch->active;
  uint8_t bms, st;
  int err = 0;

  if (dr == NULL || !ch->active_dma)
    return false;
  bms = inb (ch->bm + BM_STATUS);
  if (!(bms & 0x04) && (bms & 0x01))
    return false;		/* Still running. */
  st = inb (ch->cmd + ATA_STATUS);
  if (st & ST_BSY)
    return false;
  outb (ch->bm + BM_CMD, 0);
  outb (ch->bm + BM_STATUS, bms | 0x06);
  if ((bms & 0x02) || (st & (ST_ERR | ST_DF)))
    err = -E_IO;
  ch->active = NULL;
  blk_complete (&dr->blk, err);
  return true;
}

static bool
ide_poll (struct blkdev *d)
{
  struct ide_drive *dr = d->drv;
  return ide_check (dr->ch);
}

static void
ide_irq (unsigned irq, void *arg)
{
  struct ide_channel *ch = arg;

  if (!ide_check (ch))
    (void) inb (ch->cmd + ATA_STATUS);	/* Acknowledge stray interrupt. */
}

static const struct blkdev_ops ide_ops = {
  .start = ide_start,
  .poll = ide_poll,
};

static void
ide_probe_drive (struct ide_channel *ch, unsigned slave)
{
  uint16_t id[256];
  struct ide_drive *dr;
  uint8_t st;
  uint64_t nsect;

  ide_select (ch, slave, 0xa0);
  outb (ch->cmd + ATA_COUNT, 0);
  outb (ch->cmd + ATA_LBA0, 0);
  outb (ch->cmd + ATA_LBA1, 0);
  outb (ch->cmd + ATA_LBA2, 0);
  outb (ch->cmd + ATA_CMD, CMD_IDENTIFY);
  st = inb (ch->cmd + ATA_STATUS);
  if (st == 0 || st == 0xff)
    return;
  for (int i = 0; i < 1000000 && (inb (ch->cmd + ATA_STATUS) & ST_BSY); i++)
    ;
  if (inb (ch->cmd + ATA_LBA1) != 0 || inb (ch->cmd + ATA_LBA2) != 0)
    return;			/* ATAPI or SATA bridge: not an ATA disk. */
  if (ide_wait (ch, true) < 0)
    return;
  insw (ch->cmd + ATA_DATA, id, 256);

  dr = (struct ide_drive *) kmem_alloc (0, sizeof (*dr));
  KASSERT (dr != NULL);
  memset (dr, 0, sizeof (*dr));
  dr->ch = ch;
  dr->slave = slave;
  dr->lba48 = !!(id[83] & (1 << 10));
  dr->dma = ch->bm != 0 && (id[49] & (1 << 8));
  if (dr->lba48)
    nsect = (uint64_t) id[100] | ((uint64_t) id[101] << 16)
      | ((uint64_t) id[102] << 32) | ((uint64_t) id[103] << 48);
  else
    nsect = (uint32_t) id[60] | ((uint32_t) id[61] << 16);
  snprintf (dr->blk.name, sizeof (dr->blk.name), "ide%u%s%s",
	    ch->irq == 15, slave ? "s" : "m", dr->dma ? "-dma" : "-pio");
  dr->blk.nsectors = nsect;
  dr->blk.ops = &ide_ops;
  dr->blk.drv = dr;
  blk_register (&dr->blk);
}

static void
ide_probe_channel (uint16_t cmd, uint16_t ctl, uint16_t bm, unsigned irq)
{
  struct ide_channel *ch;

  /* Floating bus? */
  if (inb (cmd + ATA_STATUS) == 0xff)
    return;
  ch = (struct ide_channel *) kmem_alloc (0, sizeof (*ch));
  KASSERT (ch != NULL);
  memset (ch, 0, sizeof (*ch));
  ch->cmd = cmd;
  ch->ctl = ctl;
  ch->bm = bm;
  ch->irq = irq;
  if (dma_alloc (&ch->prdbuf, PAGE_SIZE) < 0)
    ch->bm = 0;
  outb (ctl, 0x00);		/* Enable interrupts (nIEN = 0). */
  irq_register (irq, ide_irq, ch);
  ide_probe_drive (ch, 0);
  ide_probe_drive (ch, 1);
}

bool
ide_probe (struct pci_dev *d)
{
  uint16_t bm = 0;

  if (d->class != 0x01 || d->subclass != 0x01)
    return false;
  pci_enable (d, true);
  if (d->bar_io[4] && d->bar[4])
    bm = d->bar[4];

  if (d->progif & 0x01)
    ide_probe_channel (d->bar[0], d->bar[1] + 2, bm, d->irq);
  else
    ide_probe_channel (0x1f0, 0x3f6, bm, 14);
  if (d->progif & 0x04)
    ide_probe_channel (d->bar[2], d->bar[3] + 2, bm ? bm + 8 : 0, d->irq);
  else
    ide_probe_channel (0x170, 0x376, bm ? bm + 8 : 0, 15);
  return true;
}
