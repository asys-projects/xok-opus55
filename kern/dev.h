/*
 * Xok device support: port I/O, interrupts, PCI.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef XOK_KERN_DEV_H
#define XOK_KERN_DEV_H

#include "kern.h"

static inline uint8_t
inb (uint16_t port)
{
  uint8_t v;
  asm volatile ("inb %w1, %0":"=a" (v):"Nd" (port));
  return v;
}

static inline uint16_t
inw (uint16_t port)
{
  uint16_t v;
  asm volatile ("inw %w1, %0":"=a" (v):"Nd" (port));
  return v;
}

static inline uint32_t
inl (uint16_t port)
{
  uint32_t v;
  asm volatile ("inl %w1, %0":"=a" (v):"Nd" (port));
  return v;
}

static inline void
outb (uint16_t port, uint8_t v)
{
  asm volatile ("outb %0, %w1"::"a" (v), "Nd" (port));
}

static inline void
outw (uint16_t port, uint16_t v)
{
  asm volatile ("outw %0, %w1"::"a" (v), "Nd" (port));
}

static inline void
outl (uint16_t port, uint32_t v)
{
  asm volatile ("outl %0, %w1"::"a" (v), "Nd" (port));
}

static inline void
insw (uint16_t port, void *addr, uint32_t cnt)
{
  asm volatile ("cld; rep insw":"+D" (addr), "+c" (cnt):"d" (port):"memory");
}

static inline void
outsw (uint16_t port, const void *addr, uint32_t cnt)
{
  asm volatile ("cld; rep outsw":"+S" (addr), "+c" (cnt):"d" (port));
}

static inline uint32_t
mmio_read32 (volatile void *base, uint32_t off)
{
  return *(volatile uint32_t *) ((uint8_t *) base + off);
}

static inline void
mmio_write32 (volatile void *base, uint32_t off, uint32_t v)
{
  *(volatile uint32_t *) ((uint8_t *) base + off) = v;
}

static inline uint16_t
mmio_read16 (volatile void *base, uint32_t off)
{
  return *(volatile uint16_t *) ((uint8_t *) base + off);
}

static inline void
mmio_write16 (volatile void *base, uint32_t off, uint16_t v)
{
  *(volatile uint16_t *) ((uint8_t *) base + off) = v;
}

static inline uint8_t
mmio_read8 (volatile void *base, uint32_t off)
{
  return *(volatile uint8_t *) ((uint8_t *) base + off);
}

static inline void
mmio_write8 (volatile void *base, uint32_t off, uint8_t v)
{
  *(volatile uint8_t *) ((uint8_t *) base + off) = v;
}

static inline void
delay_us (unsigned us)
{
  uint64_t end = ktime () + (uint64_t) us * 1000;
  while (ktime () < end)
    hal_cpu_relax ();
}

/*
 * Interrupts.
 */
typedef void (*irq_handler_t) (unsigned irq, void *arg);
void irq_register (unsigned irq, irq_handler_t h, void *arg);
void irq_dispatch (unsigned irq);
void dev_poll (void);
void dev_register_poll (void (*poll) (void *), void *arg);

/*
 * PCI.
 */
struct pci_dev
{
  uint8_t bus, dev, fn;
  uint16_t vendor, device;
  uint8_t class, subclass, progif, rev;
  uint32_t bar[6];		/* Base address (I/O port or memory). */
  uint32_t barsize[6];
  bool bar_io[6];
  uint8_t irq;			/* Legacy interrupt line (GSI). */
  uint8_t pin;
};

uint32_t pci_read32 (struct pci_dev *d, unsigned off);
void pci_write32 (struct pci_dev *d, unsigned off, uint32_t v);
uint16_t pci_read16 (struct pci_dev *d, unsigned off);
void pci_write16 (struct pci_dev *d, unsigned off, uint16_t v);
uint8_t pci_read8 (struct pci_dev *d, unsigned off);
void pci_write8 (struct pci_dev *d, unsigned off, uint8_t v);
void pci_enable (struct pci_dev *d, bool busmaster);
unsigned pci_find_cap (struct pci_dev *d, uint8_t id);
void pci_init (void);
void *pci_map_bar (struct pci_dev *d, unsigned bar);

/* Driver probe functions, called by pci_init for every device. */
bool ide_probe (struct pci_dev *d);
bool ahci_probe (struct pci_dev *d);
bool virtio_blk_probe (struct pci_dev *d);
bool nvme_probe (struct pci_dev *d);
bool e1000_probe (struct pci_dev *d);
bool rtl8139_probe (struct pci_dev *d);
bool virtio_net_probe (struct pci_dev *d);
bool ne2k_probe (struct pci_dev *d);

/*
 * DMA helpers: physically contiguous kernel buffers.
 */
struct dmabuf
{
  void *va;
  paddr_t pa;
  size_t size;
};
int dma_alloc (struct dmabuf *b, size_t size);

#endif
