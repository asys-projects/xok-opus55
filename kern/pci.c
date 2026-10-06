/*
 * PCI bus enumeration (configuration mechanism #1).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "dev.h"

#define PCI_CONFIG_ADDR 0xcf8
#define PCI_CONFIG_DATA 0xcfc

static uint32_t
pci_addr (struct pci_dev *d, unsigned off)
{
  return 0x80000000u | ((uint32_t) d->bus << 16) | ((uint32_t) d->dev << 11)
    | ((uint32_t) d->fn << 8) | (off & 0xfc);
}

uint32_t
pci_read32 (struct pci_dev *d, unsigned off)
{
  outl (PCI_CONFIG_ADDR, pci_addr (d, off));
  return inl (PCI_CONFIG_DATA);
}

void
pci_write32 (struct pci_dev *d, unsigned off, uint32_t v)
{
  outl (PCI_CONFIG_ADDR, pci_addr (d, off));
  outl (PCI_CONFIG_DATA, v);
}

uint16_t
pci_read16 (struct pci_dev *d, unsigned off)
{
  return pci_read32 (d, off) >> ((off & 2) * 8);
}

void
pci_write16 (struct pci_dev *d, unsigned off, uint16_t v)
{
  uint32_t x = pci_read32 (d, off);
  unsigned sh = (off & 2) * 8;
  x = (x & ~(0xffffu << sh)) | ((uint32_t) v << sh);
  pci_write32 (d, off, x);
}

uint8_t
pci_read8 (struct pci_dev *d, unsigned off)
{
  return pci_read32 (d, off) >> ((off & 3) * 8);
}

void
pci_write8 (struct pci_dev *d, unsigned off, uint8_t v)
{
  uint32_t x = pci_read32 (d, off);
  unsigned sh = (off & 3) * 8;
  x = (x & ~(0xffu << sh)) | ((uint32_t) v << sh);
  pci_write32 (d, off, x);
}

void
pci_enable (struct pci_dev *d, bool busmaster)
{
  uint16_t cmd = pci_read16 (d, 0x04);
  cmd |= 0x3;			/* I/O and memory space. */
  if (busmaster)
    cmd |= 0x4;
  cmd &= ~0x400;		/* Enable INTx. */
  pci_write16 (d, 0x04, cmd);
}

unsigned
pci_find_cap (struct pci_dev *d, uint8_t id)
{
  unsigned off;

  if (!(pci_read16 (d, 0x06) & 0x10))
    return 0;
  off = pci_read8 (d, 0x34) & 0xfc;
  for (int n = 0; off && n < 48; n++)
    {
      if (pci_read8 (d, off) == id)
	return off;
      off = pci_read8 (d, off + 1) & 0xfc;
    }
  return 0;
}

void *
pci_map_bar (struct pci_dev *d, unsigned bar)
{
  if (bar >= 6 || d->bar_io[bar] || d->bar[bar] == 0)
    return NULL;
  return kva_physmap (d->bar[bar], d->barsize[bar], HAL_PTE_P | HAL_PTE_W);
}

static void
pci_read_bars (struct pci_dev *d)
{
  for (unsigned i = 0; i < 6; i++)
    {
      unsigned off = 0x10 + i * 4;
      uint32_t v = pci_read32 (d, off), sz;

      d->bar[i] = 0;
      d->barsize[i] = 0;
      d->bar_io[i] = false;
      if (v == 0)
	continue;
      pci_write32 (d, off, 0xffffffff);
      sz = pci_read32 (d, off);
      pci_write32 (d, off, v);
      if (v & 1)
	{
	  d->bar_io[i] = true;
	  d->bar[i] = v & ~3u;
	  d->barsize[i] = (~(sz & ~3u) + 1) & 0xffff;
	}
      else
	{
	  d->bar[i] = v & ~0xfu;
	  d->barsize[i] = ~(sz & ~0xfu) + 1;
	  if (((v >> 1) & 3) == 2)
	    i++;		/* 64-bit BAR: skip high half. */
	}
    }
}

static const char *
pci_class_name (uint8_t class, uint8_t sub)
{
  switch (class)
    {
    case 0x01:
      return sub == 0x01 ? "IDE" : sub == 0x06 ? "SATA" : "storage";
    case 0x02:
      return "network";
    case 0x03:
      return "display";
    case 0x04:
      return "multimedia";
    case 0x06:
      return "bridge";
    case 0x0c:
      return "serial bus";
    default:
      return "device";
    }
}

static void
pci_probe_dev (struct pci_dev *d)
{
  bool claimed = false;

  if (ide_probe (d) || ahci_probe (d) || virtio_blk_probe (d) || nvme_probe (d)
      || e1000_probe (d) || rtl8139_probe (d) || virtio_net_probe (d)
      || ne2k_probe (d))
    claimed = true;
  kprintf ("pci %02x:%02x.%x %04x:%04x %s%s\n", d->bus, d->dev, d->fn,
	   d->vendor, d->device, pci_class_name (d->class, d->subclass),
	   claimed ? "" : " (no driver)");
}

static void
pci_scan_bus (unsigned bus)
{
  for (unsigned dev = 0; dev < 32; dev++)
    for (unsigned fn = 0; fn < 8; fn++)
      {
	struct pci_dev d;
	uint32_t id, cls;

	memset (&d, 0, sizeof (d));
	d.bus = bus;
	d.dev = dev;
	d.fn = fn;
	id = pci_read32 (&d, 0);
	if ((id & 0xffff) == 0xffff)
	  {
	    if (fn == 0)
	      break;
	    continue;
	  }
	d.vendor = id & 0xffff;
	d.device = id >> 16;
	cls = pci_read32 (&d, 0x08);
	d.class = cls >> 24;
	d.subclass = (cls >> 16) & 0xff;
	d.progif = (cls >> 8) & 0xff;
	d.rev = cls & 0xff;
	d.irq = pci_read8 (&d, 0x3c);
	d.pin = pci_read8 (&d, 0x3d);
	if ((pci_read8 (&d, 0x0e) & 0x7f) == 0)
	  pci_read_bars (&d);
	if (d.class == 0x06 && d.subclass == 0x04)
	  {
	    /* PCI-PCI bridge. */
	    unsigned sec = pci_read8 (&d, 0x19);
	    kprintf ("pci %02x:%02x.%x bridge to bus %u\n", bus, dev, fn,
		     sec);
	    if (sec > bus)
	      pci_scan_bus (sec);
	    continue;
	  }
	pci_probe_dev (&d);
	if (fn == 0 && !(pci_read8 (&d, 0x0e) & 0x80))
	  break;
      }
}

void
pci_init (void)
{
  pci_scan_bus (0);
}
