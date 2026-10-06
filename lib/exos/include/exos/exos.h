/*
 * ExOS: a UNIX library operating system for Xok.
 *
 * Internal interfaces of the library OS.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef EXOS_EXOS_H
#define EXOS_EXOS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>
#include <xok/types.h>
#include <xok/mmu.h>
#include <xok/env.h>
#include <xok/sysinfo.h>
#include <xok/ppage.h>
#include <xok/wk.h>
#include <exos/sys.h>

/*
 * Address space layout used by ExOS (below ULIM).
 *
 *  ULIM (UAREA)  0xb7fff000
 *  XSTACK        0xb7fe0000 - 0xb7ff0000  exception stack (64KB)
 *  SHARED        0xa0000000 - 0xb0000000  shared regions (fd table,...)
 *  USTACKTOP     0x9ffff000  user stack, grows down to USTACKBOT
 *  USTACKBOT     0x9f000000
 *  BCWIN         0x90000000 - 0x94000000  buffer cache window
 *  MMAP          0x60000000 - 0x8f000000  mmap/anonymous/exec staging
 *  heap          _end ...    up to 0x60000000
 *  text/data     0x00800000
 */
#define XSTACKTOP   0xb7ff0000UL
#define XSTACKSIZE  0x00010000UL
#define USHARED     0xa0000000UL
#define USHARED_TOP 0xb0000000UL
#define USTACKTOP   0x9ffff000UL
#define USTACKBOT   0x9f000000UL
#define UMMAP       0x60000000UL
#define UMMAP_TOP   0x8f000000UL
#define BCWIN       0x90000000UL	/* Buffer cache window (XN_NBC pages). */
#define BCWIN_TOP   0x94000000UL
#define UHEAP_TOP   0x60000000UL
#define UTEXT       0x00800000UL

/* Software PTE bits used by ExOS. */
#define PTE_COW     PTE_AVAIL0	/* Copy-on-write. */
#define PTE_SHARE   PTE_AVAIL1	/* Shared across fork (not COW). */
#define PTE_NOFORK  PTE_AVAIL2	/* Not inherited across fork. */

/* Capability slot used for all ExOS resources. */
#define EXOS_CAP CAP_ROOT

/* This environment. */
extern envid_t __envid;
extern volatile struct Uenv *const __uenv;

/* Process information, kept in the library area of the u-area, so that
   every process' state is readable by everybody (ps, wait, kill). */
#define PROC_FREE    0
#define PROC_RUNNING 1
#define PROC_ZOMBIE  2
#define PROC_EXECED  3		/* Replaced by a new env (exec). */
#define PROC_MAGIC   0x45584f53	/* 'EXOS' */

struct exos_proc
{
  uint32_t magic;
  volatile uint32_t state;
  pid_t pid;
  pid_t ppid;
  pid_t pgid;
  uid_t uid, euid;
  gid_t gid, egid;
  volatile int32_t status;	/* wait() status once zombie. */
  volatile uint32_t sigpending;	/* Posted signals. */
  uint32_t start_sec;
  envid_t exec_env;		/* EXECED: env continuing this process. */
  char name[32];
  char args[128];		/* Command line, for ps. */
  volatile uint32_t child_events;	/* Bumped by children on exit. */
};

#define proc_of_env(_id) ((volatile struct exos_proc *) uenv_of (_id)->u_libos)
extern volatile struct exos_proc *const __proc;

/* Library runtime state that must never be copy-on-write: kept in the
   u-area, after the process information. */
struct exos_rt
{
  volatile uint32_t cow_depth;	/* Nesting of copy-on-write handling. */
};
#define __rt ((volatile struct exos_rt *) (UAREA + UENV_LIBOS_OFF + 512))

/* Startup and runtime. */
void exos_panic (const char *fmt, ...) __attribute__ ((noreturn));
void exos_init_upcalls (void);
void exos_crit_enter (void);
void exos_crit_leave (void);
void exos_yield (envid_t to);

/* Context switch and wakeup-predicate add-ons. */
void exos_add_cswitch (void (*fn) (void));
typedef int (*wk_addon_t) (struct wk_term *t, int max);
void exos_add_wk (wk_addon_t fn, void (*handler) (void));

/* Sleep until predicate holds (with add-ons appended). */
int exos_wkpred (struct wk_term *t, int n);
int exos_sleep_until_mem (volatile uint32_t *addr, uint32_t op, uint32_t val,
			  uint32_t timeout_ms);

/* Virtual memory. */
int exos_page_alloc (uintptr_t va, xpte_t perm);
int exos_page_unmap (uintptr_t va);
int exos_range_alloc (uintptr_t va, size_t len, xpte_t perm);
int exos_range_unmap (uintptr_t va, size_t len);
static inline xpte_t exos_pte (uintptr_t va)
{
  return vpt[va >> PGSHIFT];
}
static inline int exos_mapped (uintptr_t va)
{
  return (exos_pte (va) & PTE_P) != 0;
}
int exos_cow_fault (uintptr_t va);
void exos_vm_init (void);
void *exos_mmap_alloc (size_t len);
void exos_mmap_free (void *p, size_t len);
int exos_touch (const void *buf, size_t len, int write);

/* Processes. */
void exos_proc_init (int is_boot);
pid_t exos_spawn (const char *path, char *const argv[], char *const envp[]);
envid_t exos_pid2env (pid_t pid);

/* IPC requests (sys_ipc_call arguments: op, a, b, c). */
#define IPC_PING     1
#define IPC_SIGNAL   2		/* a = signal */
#define IPC_CHILD    3		/* a = child pid: child state changed */
typedef int (*ipc_handler_t) (envid_t from, uint32_t a, uint32_t b,
			      uint32_t c, uint32_t * r1);
void exos_ipc_register (unsigned op, ipc_handler_t h);
int exos_ipc_send (envid_t to, uint32_t op, uint32_t a, uint32_t b,
		   uint32_t c, uint32_t * r1);

/* Signals. */
void exos_sig_post (int sig);
void exos_sig_deliver (void);
void exos_fault_signal (int sig, struct utf *utf);

/* Console. */
int exos_cons_read (char *buf, size_t n);

/* Time. */
uint64_t exos_time_ns (void);

/* File descriptors (fd.c). */
void exos_fd_init (int is_boot);

#endif
