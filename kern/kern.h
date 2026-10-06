/*
 * Xok kernel: common definitions.
 *
 * Xok is an exokernel: it securely multiplexes the machine (CPU,
 * physical memory, disk, network) and leaves every abstraction to
 * untrusted library operating systems.  This implementation is built
 * on the NUX kernel library: NUX provides the platform bring-up, the
 * low-level entry points, page table manipulation (umap) and the
 * kernel memory allocators; Xok implements the exokernel interface on
 * top of it.
 *
 * Concurrency: the kernel runs with interrupts disabled and is
 * serialised by a big kernel lock taken at every entry point.  The
 * kernel is not preemptible, as in the original Xok.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef XOK_KERN_H
#define XOK_KERN_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <nux/nux.h>
#include <nux/hal.h>
#include <nux/plt.h>
#include <nux/locks.h>

#include <xok/types.h>
#include <xok/error.h>
#include <xok/mmu.h>
#include <xok/cap.h>
#include <xok/env.h>
#include <xok/sysinfo.h>
#include <xok/ppage.h>
#include <xok/syscall.h>

#define XOK_VERSION "1.0"

#ifndef UINT64_MAX
#define UINT64_MAX 0xffffffffffffffffULL
#endif

int strcmp (const char *a, const char *b);

#define kprintf printf
#define kpanic(...) do { printf ("\nXOK PANIC (%s:%d): ", __FILE__, __LINE__); \
                         printf (__VA_ARGS__); printf ("\n"); \
                         nux_panic ("xok panic", NULL); } while (0)

#define KASSERT(_c) do { if (!(_c)) kpanic ("assertion failed: %s", #_c); } while (0)

#define MIN(_a,_b) ((_a) < (_b) ? (_a) : (_b))
#define MAX(_a,_b) ((_a) > (_b) ? (_a) : (_b))
#define ROUNDUP(_x,_a) (((_x) + (_a) - 1) / (_a) * (_a))

/* Time slice length. */
#define TICK_NSEC   (10 * 1000 * 1000)
/* Grace period given to an environment to handle the epilogue. */
#define GRACE_NSEC  (2 * 1000 * 1000)

/*
 * Big kernel lock.
 */
void bkl_lock (void);
void bkl_unlock (void);
bool bkl_held (void);

/*
 * Physical memory (pmem.c).
 */
struct ppage
{
  uint32_t pk_next;		/* Free list links. */
  uint32_t pk_prev;
  struct cap pk_acl;		/* Guard capability. */
};

extern struct ppage *ppages;
extern volatile struct ppage_info *ppinfo;
extern uint32_t npages;		/* Number of physical page frames. */

void pmem_init (void);
pfn_t pmem_alloc (int low);
pfn_t pmem_alloc_specific (pfn_t pfn);
pfn_t pmem_alloc_contig (unsigned n);
void pmem_free (pfn_t pfn);
void pmem_page_ref (pfn_t pfn);
void pmem_page_unref (pfn_t pfn);
void pmem_pin (pfn_t pfn);
void pmem_unpin (pfn_t pfn);
bool pmem_is_ram (pfn_t pfn);
void *pmem_export_area (size_t size, vaddr_t * kva);
pfn_t pmem_kva_pfn (vaddr_t va);
void pmem_zero (pfn_t pfn);
void pmem_copy (pfn_t dst, pfn_t src);
uint32_t pmem_nfree (void);

/*
 * Capabilities (cap.c).
 */
struct env;
struct cap *env_cap (struct env *e, unsigned k);
int cap_check (struct env *e, unsigned k, const struct cap *guard,
	       unsigned perm);
int cap_grants (const struct cap *c, const struct cap *guard, unsigned perm);
extern const struct cap cap_root;

/*
 * Copying from/to user memory of the current environment.
 */
int copyin (void *dst, uaddr_t src, size_t len);
int copyout (uaddr_t dst, const void *src, size_t len);
int copyin_str (char *dst, uaddr_t src, size_t max);

/*
 * Time.
 */
uint64_t ktime (void);		/* Nanoseconds since boot. */

/*
 * Exported structures.
 */
extern volatile struct sysinfo *sysinfo;
void export_init (void);
bool export_fault (struct env *e, vaddr_t va);

/*
 * Console (console.c).
 */
void cons_init (void);
void cons_intr (int c);
int cons_getc (void);
void cons_poll (void);

/*
 * Per-CPU state.
 */
struct pcpu
{
  unsigned id;
  struct env *cur;		/* Env running (or about to run). */
  unsigned q;			/* Current slice in the quantum vector. */
  uint64_t slice_end;		/* End of the current slice. */
  uint64_t grace_end;		/* Deadline for epilogue handling, or 0. */
  struct env *fpu_owner;	/* Env whose FPU state is live here. */
  volatile uint32_t ipi_pending;	/* IPI request bits. */
  uint64_t next_deadline;	/* Next timer event wanted by this CPU. */
  uctxt_t retframe __attribute__ ((aligned (16)));
};

#define IPI_RESCHED 1		/* Re-evaluate scheduling. */
#define IPI_TICK    2		/* Timer deadline passed. */
#define IPI_REVOKE  4		/* sys_cpu_revoke. */

extern struct pcpu pcpus[XOK_MAXCPU];
static inline struct pcpu *
mycpu (void)
{
  return (struct pcpu *) cpu_getdata ();
}

#endif
