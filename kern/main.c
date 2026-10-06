/*
 * Xok kernel: initialisation and NUX entry points.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "env.h"
#include "dev.h"

volatile struct sysinfo *sysinfo;
static volatile bool xok_ready;

static lock_t bkl;
static volatile int bkl_owner = -1;

void power_init (void);
void blk_init (void);
void net_init (void);
void xn_init (void);
void trap_fault (struct env *e, vaddr_t va, hal_pfinfo_t info);
void trap_except (struct env *e, unsigned ex);
uint64_t rtc_read_unix (void);

void
bkl_lock (void)
{
  spinlock (&bkl);
  bkl_owner = cpu_id ();
}

void
bkl_unlock (void)
{
  bkl_owner = -1;
  spinunlock (&bkl);
}

bool
bkl_held (void)
{
  return bkl_owner == (int) cpu_id ();
}

static uctxt_t *
finish (void)
{
  uctxt_t *r = sched_return ();
  bkl_unlock ();
  return r;
}

int
main (int argc, char *argv[])
{
  struct env *init;
  vaddr_t kva;

  kprintf ("\nXok/ExOS %s: an exokernel on NUX (i386)\n", XOK_VERSION);
  spinlock_init (&bkl);
  bkl_lock ();

  sched_init_cpu ();
  pmem_init ();

  sysinfo = pmem_export_area (USYSINFO_SIZE, &kva);
  sysinfo->si_nppages = npages;
  sysinfo->si_nfreepages = pmem_nfree ();
  sysinfo->si_boot_unix = rtc_read_unix ();

  sched_init ();
  env_init ();
  power_init ();
  cons_init ();
  blk_init ();
  net_init ();
  pci_init ();
  xn_init ();

  init = env_bootstrap ();
  /* Give the boot environment the first slice of the boot CPU. */
  sysinfo->si_qvec[0][0].q_env = init->id;
  init->info->e_nquanta = 1;
  kprintf ("xok: %u CPUs, boot environment %x\n", sysinfo->si_ncpu,
	   init->id);

  xok_ready = true;
  bkl_unlock ();

  /* Start scheduling on every CPU. */
  for (unsigned c = 0; c < sysinfo->si_ncpu; c++)
    {
      pcpus[c].ipi_pending |= IPI_RESCHED;
      cpu_ipi (c);
    }
  return EXIT_IDLE;
}

int
main_ap (void)
{
  while (!xok_ready)
    hal_cpu_relax ();
  bkl_lock ();
  sched_init_cpu ();
  bkl_unlock ();
  return EXIT_IDLE;
}

uctxt_t *
entry_sysc (uctxt_t * u,
	    unsigned long a1, unsigned long a2, unsigned long a3,
	    unsigned long a4, unsigned long a5, unsigned long a6,
	    unsigned long a7)
{
  struct env *e;

  bkl_lock ();
  e = curenv ();
  KASSERT (e != NULL);
  sched_save (e, u);
  syscall (e, a1, a2, a3, a4, a5, a6, a7);
  return finish ();
}

uctxt_t *
entry_pf (uctxt_t * u, vaddr_t va, hal_pfinfo_t pfi)
{
  struct env *e;

  bkl_lock ();
  e = curenv ();
  KASSERT (e != NULL);
  sched_save (e, u);
  if (((va >= UXOK_BASE && va < UXOK_TOP) || (va & ~PAGE_MASK) == UAREA)
      && (pfi & HAL_PF_REASON_MASK) == HAL_PF_REASON_NOTP
      && !(pfi & HAL_PF_INFO_WRITE) && export_fault (e, va))
    return finish ();
  trap_fault (e, va, pfi);
  return finish ();
}

uctxt_t *
entry_ex (uctxt_t * u, unsigned ex)
{
  struct env *e;

  bkl_lock ();
  e = curenv ();
  KASSERT (e != NULL);
  sched_save (e, u);
  if (ex == 7 && fpu_trap (e))
    return finish ();
  trap_except (e, ex);
  return finish ();
}

uctxt_t *
entry_alarm (uctxt_t * u)
{
  bkl_lock ();
  sched_save (curenv (), u);
  sched_timer ();
  dev_poll ();
  return finish ();
}

uctxt_t *
entry_ipi (uctxt_t * u)
{
  bkl_lock ();
  sched_save (curenv (), u);
  return finish ();
}

uctxt_t *
entry_irq (uctxt_t * u, unsigned irq, bool lvl)
{
  bkl_lock ();
  sched_save (curenv (), u);
  irq_dispatch (irq);
  sched_kick_idle ();
  return finish ();
}
