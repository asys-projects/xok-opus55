/*
 * Xok device infrastructure: interrupt dispatch, polling, DMA memory.
 *
 * Device drivers live in the kernel (as in Xok, where they derive from
 * OpenBSD's); what the kernel exports is not the device but a secure
 * multiplexing of it (XN for disks, DPF/packet rings for networks).
 *
 * Interrupts are delivered by NUX as platform IRQs (GSIs).  Drivers
 * are also polled at every timer tick, which makes the system robust
 * against lost edge-triggered interrupts.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "dev.h"

#define MAXIRQ 64
#define MAXHANDLERS 4
#define MAXPOLL 16

static struct
{
  irq_handler_t h;
  void *arg;
} irqs[MAXIRQ][MAXHANDLERS];

static struct
{
  void (*poll) (void *);
  void *arg;
} polls[MAXPOLL];
static unsigned npolls;

void
irq_register (unsigned irq, irq_handler_t h, void *arg)
{
  if (irq >= MAXIRQ || irq >= plt_irq_max ())
    {
      kprintf ("xok: cannot register IRQ %u\n", irq);
      return;
    }
  for (unsigned i = 0; i < MAXHANDLERS; i++)
    if (irqs[irq][i].h == NULL)
      {
	irqs[irq][i].h = h;
	irqs[irq][i].arg = arg;
	plt_irq_enable (irq);
	return;
      }
  kprintf ("xok: too many handlers on IRQ %u\n", irq);
}

void
irq_dispatch (unsigned irq)
{
  if (irq >= MAXIRQ)
    return;
  for (unsigned i = 0; i < MAXHANDLERS; i++)
    if (irqs[irq][i].h != NULL)
      irqs[irq][i].h (irq, irqs[irq][i].arg);
}

void
dev_register_poll (void (*poll) (void *), void *arg)
{
  KASSERT (npolls < MAXPOLL);
  polls[npolls].poll = poll;
  polls[npolls].arg = arg;
  npolls++;
}

void
dev_poll (void)
{
  for (unsigned i = 0; i < npolls; i++)
    polls[i].poll (polls[i].arg);
}

int
dma_alloc (struct dmabuf *b, size_t size)
{
  unsigned n = ROUNDUP (size, PAGE_SIZE) / PAGE_SIZE;
  pfn_t pfn = pmem_alloc_contig (n);

  if (pfn == PFN_INVALID)
    return -E_NO_MEM;
  b->pa = (paddr_t) pfn << PAGE_SHIFT;
  b->size = n * PAGE_SIZE;
  b->va = kva_physmap (b->pa, b->size, HAL_PTE_P | HAL_PTE_W);
  if (b->va == NULL)
    return -E_NO_MEM;
  memset (b->va, 0, b->size);
  return 0;
}
