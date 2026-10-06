/*
 * AHCI (Serial ATA) disk driver.
 *
 * Every implemented port with an ATA disk attached becomes a block
 * device.  One command slot is used per port; requests are serialised
 * by the block layer.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "dev.h"
#include "blk.h"

#define HBA_CAP  0x00
#define HBA_GHC  0x04
#define HBA_IS   0x08
#define HBA_PI   0x0c

#define GHC_AE   (1u << 31)
#define GHC_IE   (1u << 1)

#define PORT(_p, _r) (0x100 + (_p) * 0x80 + (_r))
#define PX_CLB   0x00
#define PX_CLBU  0x04
#define PX_FB    0x08
#define PX_FBU   0x0c
#define PX_IS    0x10
#define PX_IE    0x14
#define PX_CMD   0x18
#define PX_TFD   0x20
#define PX_SIG   0x24
#define PX_SSTS  0x28
#define PX_SERR  0x30
#define PX_CI    0x38

#define CMD_ST   (1u << 0)
#define CMD_FRE  (1u << 4)
#define CMD_FR   (1u << 14)
#define CMD_CR   (1u << 15)

#define IS_TFES  (1u << 30)

#define SIG_ATA  0x00000101

struct ahci_hba;

struct ahci_port
{
  struct blkdev blk;
  struct ahci_hba *hba;
  unsigned port;
  struct dmabuf mem;		/* Command list, FIS, command table. */
  bool busy;
};

struct ahci_hba
{
  volatile uint8_t *abar;
  struct ahci_port *ports[32];
};

#define CL_OFF  0
#define FIS_OFF 1024
#define CT_OFF  2048
#define PRDT_OFF (CT_OFF + 0x80)

static inline uint32_t
rd (struct ahci_hba *h, uint32_t r)
{
  return mmio_read32 (h->abar, r);
}

static inline void
wr (struct ahci_hba *h, uint32_t r, uint32_t v)
{
  mmio_write32 (h->abar, r, v);
}

static void
port_stop (struct ahci_hba *h, unsigned p)
{
  uint32_t cmd = rd (h, PORT (p, PX_CMD));

  cmd &= ~(CMD_ST | CMD_FRE);
  wr (h, PORT (p, PX_CMD), cmd);
  for (int i = 0; i < 1000000; i++)
    if (!(rd (h, PORT (p, PX_CMD)) & (CMD_CR | CMD_FR)))
      break;
}

static void
port_start (struct ahci_hba *h, unsigned p)
{
  for (int i = 0; i < 1000000; i++)
    if (!(rd (h, PORT (p, PX_CMD)) & CMD_CR))
      break;
  wr (h, PORT (p, PX_CMD), rd (h, PORT (p, PX_CMD)) | CMD_FRE);
  wr (h, PORT (p, PX_CMD), rd (h, PORT (p, PX_CMD)) | CMD_ST);
}

/*
 * Build the command in slot 0: an H2D register FIS and the PRDT.
 */
static void
build_cmd (struct ahci_port *ap, uint8_t command, uint64_t lba,
	   uint16_t count, bool write, const struct bio_seg *segs,
	   unsigned nsegs)
{
  uint8_t *m = ap->mem.va;
  uint32_t *hdr = (uint32_t *) (m + CL_OFF);
  uint8_t *fis = m + CT_OFF;
  uint32_t *prdt = (uint32_t *) (m + PRDT_OFF);
  uint32_t ctba = ap->mem.pa + CT_OFF;

  memset (m + CT_OFF, 0, PAGE_SIZE - CT_OFF);
  hdr[0] = 5 | (write ? (1 << 6) : 0) | ((uint32_t) nsegs << 16);
  hdr[1] = 0;
  hdr[2] = ctba;
  hdr[3] = 0;

  fis[0] = 0x27;		/* Register H2D. */
  fis[1] = 0x80;		/* Command. */
  fis[2] = command;
  fis[4] = lba & 0xff;
  fis[5] = (lba >> 8) & 0xff;
  fis[6] = (lba >> 16) & 0xff;
  fis[7] = 0x40;		/* LBA mode. */
  fis[8] = (lba >> 24) & 0xff;
  fis[9] = (lba >> 32) & 0xff;
  fis[10] = (lba >> 40) & 0xff;
  fis[12] = count & 0xff;
  fis[13] = (count >> 8) & 0xff;

  for (unsigned i = 0; i < nsegs; i++)
    {
      prdt[i * 4 + 0] = segs[i].pa;
      prdt[i * 4 + 1] = segs[i].pa >> 32;
      prdt[i * 4 + 2] = 0;
      prdt[i * 4 + 3] = (segs[i].len - 1) | (i == nsegs - 1 ? (1u << 31) : 0);
    }
}

static void
issue (struct ahci_port *ap)
{
  struct ahci_hba *h = ap->hba;
  unsigned p = ap->port;

  wr (h, PORT (p, PX_IS), 0xffffffff);
  wr (h, PORT (p, PX_SERR), 0xffffffff);
  ap->busy = true;
  wr (h, PORT (p, PX_CI), 1);
}

static bool
ahci_check (struct ahci_port *ap)
{
  struct ahci_hba *h = ap->hba;
  unsigned p = ap->port;
  uint32_t is;
  int err = 0;

  if (!ap->busy)
    return false;
  is = rd (h, PORT (p, PX_IS));
  if ((rd (h, PORT (p, PX_CI)) & 1) && !(is & IS_TFES))
    return false;
  if ((is & IS_TFES) || (rd (h, PORT (p, PX_TFD)) & 0x01))
    {
      err = -E_IO;
      /* Recover the port. */
      port_stop (h, p);
      wr (h, PORT (p, PX_SERR), 0xffffffff);
      port_start (h, p);
    }
  wr (h, PORT (p, PX_IS), is);
  ap->busy = false;
  blk_complete (&ap->blk, err);
  return true;
}

static int
ahci_start (struct blkdev *d, struct bio *b)
{
  struct ahci_port *ap = d->drv;

  build_cmd (ap, b->write ? 0x35 : 0x25, b->sector, b->nsect, b->write,
	     b->seg, b->nsegs);
  issue (ap);
  return 0;
}

static bool
ahci_poll (struct blkdev *d)
{
  return ahci_check (d->drv);
}

static void
ahci_irq (unsigned irq, void *arg)
{
  struct ahci_hba *h = arg;
  uint32_t is = rd (h, HBA_IS);

  for (unsigned p = 0; p < 32; p++)
    if (h->ports[p])
      ahci_check (h->ports[p]);
  wr (h, HBA_IS, is);
}

static const struct blkdev_ops ahci_ops = {
  .start = ahci_start,
  .poll = ahci_poll,
};

static int
ahci_identify (struct ahci_port *ap, uint16_t * id)
{
  struct dmabuf buf;
  struct bio_seg seg;

  if (dma_alloc (&buf, PAGE_SIZE) < 0)
    return -E_NO_MEM;
  seg.pa = buf.pa;
  seg.len = 512;
  build_cmd (ap, 0xec, 0, 0, false, &seg, 1);
  issue (ap);
  for (uint64_t t0 = ktime ();;)
    {
      uint32_t is = rd (ap->hba, PORT (ap->port, PX_IS));
      if (!(rd (ap->hba, PORT (ap->port, PX_CI)) & 1))
	break;
      if ((is & IS_TFES) || ktime () - t0 > 2000000000ULL)
	{
	  ap->busy = false;
	  return -E_IO;
	}
    }
  ap->busy = false;
  wr (ap->hba, PORT (ap->port, PX_IS), 0xffffffff);
  memcpy (id, buf.va, 512);
  return 0;
}

bool
ahci_probe (struct pci_dev *d)
{
  struct ahci_hba *h;
  uint32_t pi;

  if (d->class != 0x01 || d->subclass != 0x06 || d->progif != 0x01)
    return false;
  pci_enable (d, true);
  h = (struct ahci_hba *) kmem_alloc (0, sizeof (*h));
  KASSERT (h != NULL);
  memset (h, 0, sizeof (*h));
  h->abar = pci_map_bar (d, 5);
  if (h->abar == NULL)
    return false;

  wr (h, HBA_GHC, rd (h, HBA_GHC) | GHC_AE);
  pi = rd (h, HBA_PI);
  for (unsigned p = 0; p < 32; p++)
    {
      struct ahci_port *ap;
      uint16_t id[256];
      uint32_t ssts;

      if (!(pi & (1u << p)))
	continue;
      ssts = rd (h, PORT (p, PX_SSTS));
      if ((ssts & 0xf) != 3)
	continue;
      if (rd (h, PORT (p, PX_SIG)) != SIG_ATA)
	continue;

      ap = (struct ahci_port *) kmem_alloc (0, sizeof (*ap));
      KASSERT (ap != NULL);
      memset (ap, 0, sizeof (*ap));
      ap->hba = h;
      ap->port = p;
      if (dma_alloc (&ap->mem, PAGE_SIZE) < 0)
	continue;
      port_stop (h, p);
      wr (h, PORT (p, PX_CLB), ap->mem.pa + CL_OFF);
      wr (h, PORT (p, PX_CLBU), 0);
      wr (h, PORT (p, PX_FB), ap->mem.pa + FIS_OFF);
      wr (h, PORT (p, PX_FBU), 0);
      wr (h, PORT (p, PX_SERR), 0xffffffff);
      wr (h, PORT (p, PX_IS), 0xffffffff);
      port_start (h, p);

      if (ahci_identify (ap, id) < 0)
	{
	  kprintf ("ahci: port %u identify failed\n", p);
	  continue;
	}
      ap->blk.nsectors = (uint64_t) id[100] | ((uint64_t) id[101] << 16)
	| ((uint64_t) id[102] << 32) | ((uint64_t) id[103] << 48);
      if (ap->blk.nsectors == 0)
	ap->blk.nsectors = (uint32_t) id[60] | ((uint32_t) id[61] << 16);
      snprintf (ap->blk.name, sizeof (ap->blk.name), "ahci-p%u", p);
      ap->blk.ops = &ahci_ops;
      ap->blk.drv = ap;
      h->ports[p] = ap;
      wr (h, PORT (p, PX_IE), 0xffffffff);
      blk_register (&ap->blk);
    }
  wr (h, HBA_IS, 0xffffffff);
  wr (h, HBA_GHC, rd (h, HBA_GHC) | GHC_IE);
  irq_register (d->irq, ahci_irq, h);
  return true;
}
