/*
 * Xok kernel environments.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef XOK_KERN_ENV_H
#define XOK_KERN_ENV_H

#include "kern.h"
#include <xok/wk.h>
#include <xok/ipc.h>

#define VPT_NPAGES (UVPT_SIZE / PAGE_SIZE)	/* Mirror pages per env. */

struct wkpred;
struct msgring;

struct env
{
  envid_t id;
  uint32_t status;
  struct umap umap;

  /* U-area: kernel view and physical page. */
  struct Uenv *u;
  pfn_t upfn;

  /* Public information (exported, read-only to users). */
  volatile struct envinfo *info;

  /* Saved user context (valid when not running). */
  uctxt_t tf __attribute__ ((aligned (16)));

  int cpu;			/* CPU running this env, or -1. */
  bool need_prologue;		/* Deliver prologue at next resume. */
  bool in_epilogue;		/* Epilogue delivered, awaiting yield. */
  bool interrupted;		/* Critical section was interrupted. */
  uint32_t penalty;		/* Slices to forfeit (excess time). */
  uint32_t priv;		/* ENV_PRIV_* */

  /* FPU/SSE state, saved eagerly when the env is descheduled. */
  uint8_t fpu[512] __attribute__ ((aligned (16)));
  bool fpu_valid;

  /* Exported page table mirror: one page per 2MB of user space. */
  pfn_t *vpt;

  /* Wakeup predicate. */
  struct wkpred *wk;

  /* IPC. */
  envid_t ipc_peer;		/* Callee we wait on (ENV_IPC_WAIT). */
  bool ipc_pending;		/* IPC upcall to deliver. */
  bool ipc_return;		/* Resuming from IPC reply (no prologue). */
  uint32_t ipc_args[5];

  /* Message ring. */
  struct msgring *mr;
};

extern struct env envs[NENV];

void env_init (void);
struct env *env_bootstrap (void);
int env_alloc (struct env *parent, const struct cap *guard,
	       struct env **out);
void env_destroy (struct env *e);
void env_reap (struct env *e);
struct env *env_lookup (envid_t id);
int env_control (struct env *cur, unsigned ke, envid_t id, struct env **out);

/* Memory mappings. */
int env_map (struct env *e, vaddr_t va, pfn_t pfn, xpte_t pte);
int env_unmap (struct env *e, vaddr_t va, xpte_t keep);
xpte_t env_getpte (struct env *e, vaddr_t va);
void env_tlb_commit (struct env *e);
int env_vpt_refresh (struct env *e, vaddr_t va);
void env_export_map (struct env *e, vaddr_t va, pfn_t pfn);

static inline struct env *
curenv (void)
{
  return mycpu ()->cur;
}

/* Scheduling (sched.c). */
void sched_init (void);
void sched_init_cpu (void);
uctxt_t *sched_return (void);
void sched_yield_to (struct env *target);
void sched_switch_away (void);
void sched_env_freed (struct env *e);
int sched_quantum_alloc (struct env *cur, unsigned k, int q, unsigned cpu,
			 envid_t id);
int sched_quantum_free (struct env *cur, unsigned k, int q, unsigned cpu);
int sched_quantum_set (struct env *cur, unsigned k, int q, unsigned cpu,
		       envid_t id);
int sched_cpu_revoke (struct env *cur, unsigned k, unsigned cpu, envid_t id);
void sched_timer (void);
void sched_kick_idle (void);
void sched_save (struct env *e, uctxt_t * u);

/* Upcalls (trap.c). */
int upcall_push (struct env *e, uint32_t kind, uint32_t trapno, uint32_t err,
		 uint32_t va, const uint32_t * args);
void fpu_init_cpu (void);
void fpu_switch_out (struct env *e);
void fpu_switch_in (struct env *e);
bool fpu_trap (struct env *e);

/* Wakeup predicates (wk.c). */
int wk_install (struct env *e, uaddr_t terms, unsigned n);
bool wk_eval (struct env *e);
void wk_free (struct env *e);

/* IPC (ipc.c). */
int ipc_call (struct env *cur, envid_t to, const uint32_t * args);
int ipc_reply (struct env *cur, envid_t to, uint32_t r0, uint32_t r1);
int msgring_set (struct env *e, uaddr_t ring, unsigned n);
void msgring_free (struct env *e);
int ipc_sendmsg (struct env *cur, envid_t to, uaddr_t msg, unsigned size);
void ipc_env_freed (struct env *e);

/* Software regions (sreg.c). */
int sreg_create (struct env *cur, unsigned k, uint32_t size);
int sreg_destroy (struct env *cur, unsigned k, unsigned id);
int sreg_read (struct env *cur, unsigned k, unsigned id, uint32_t off,
	       uaddr_t buf, uint32_t len);
int sreg_write (struct env *cur, unsigned k, unsigned id, uint32_t off,
		uaddr_t buf, uint32_t len);

/* Syscall dispatcher (syscall.c). */
void syscall (struct env *e, unsigned long num, unsigned long a1,
	      unsigned long a2, unsigned long a3, unsigned long a4,
	      unsigned long a5, unsigned long a6);

/* Device hooks for environment destruction. */
void net_env_freed (struct env *e);
void xn_env_freed (struct env *e);

#endif
