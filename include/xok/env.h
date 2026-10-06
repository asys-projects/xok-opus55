/*
 * Xok environments.
 *
 * An environment is Xok's notion of a process at the lowest level: it
 * holds the hardware-specific state needed to run a program (an
 * address space and register context) and everything required to
 * deliver events to it (upcall entry points and an exception stack),
 * plus the binding between the process and its principal
 * (capabilities).
 *
 * Every environment has a u-area page (struct Uenv) shared between
 * the kernel and the application.  It is mapped read-write at UAREA in
 * the environment's own address space and read-only, for every
 * environment, at UENVS + ENVX(id) * PGSIZE.  The upper part of the
 * page is reserved to the library OS (ExOS keeps its process table
 * entry there, so that 'ps' can read every process' state).
 *
 * The kernel also maintains a public, read-only array of struct envinfo
 * at UENVINFO.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef XOK_ENV_H
#define XOK_ENV_H

#include <xok/types.h>
#include <xok/cap.h>

/* Environment status. */
#define ENV_FREE        0
#define ENV_RUNNABLE    1	/* Can be scheduled. */
#define ENV_NOT_RUNNABLE 2	/* Will not be scheduled until made runnable. */
#define ENV_WAITING     3	/* Sleeping on a wakeup predicate. */
#define ENV_IPC_WAIT    4	/* Waiting for an IPC reply. */
#define ENV_DYING       5

/*
 * User trap frame.  This is the format in which the kernel hands
 * register contexts to upcall handlers (pushed on the exception
 * stack).  The first eight words are in 'pushal' order.
 */
struct utf
{
  uint32_t utf_edi;
  uint32_t utf_esi;
  uint32_t utf_ebp;
  uint32_t utf_oesp;		/* Ignored. */
  uint32_t utf_ebx;
  uint32_t utf_edx;
  uint32_t utf_ecx;
  uint32_t utf_eax;
  uint32_t utf_eflags;
  uint32_t utf_eip;
  uint32_t utf_esp;
  /* Event description. */
  uint32_t utf_kind;		/* UPC_* */
  uint32_t utf_trapno;		/* Exception vector. */
  uint32_t utf_err;		/* Exception error code. */
  uint32_t utf_va;		/* Faulting address. */
  uint32_t utf_arg[5];		/* Event arguments (IPC). */
};

/* Upcall kinds (utf_kind, and %eax on entry to the handler). */
#define UPC_PROLOGUE  1		/* Beginning of a time slice. */
#define UPC_EPILOGUE  2		/* End of a time slice: save state and yield. */
#define UPC_FAULT     3		/* Page fault. */
#define UPC_EXCEPT    4		/* Other exception or redirected INT. */
#define UPC_IPC       5		/* Protected control transfer (IPC). */
#define UPC_REVOKE    6		/* Kernel requests pages back. */

/*
 * The kernel leaves this many bytes free above a utf pushed on a
 * stack, so that resume code can store the return address there.
 */
#define UTF_GAP 16

#define UENV_LIBOS_OFF 1024

struct Uenv
{
  /* Upcall entry points, set by the library OS.  Zero = none. */
  uint32_t u_entprologue;
  uint32_t u_entepilogue;
  uint32_t u_entfault;
  uint32_t u_entexcept;
  uint32_t u_entipc;
  uint32_t u_entrevoke;
  uint32_t u_xstktop;		/* Top of the exception stack. */
  uint32_t u_xstksize;		/* Its size (for recursion detection). */

  /* Robust critical sections: while u_in_critical != 0 the kernel
     does not deliver the epilogue upcall; it sets u_interrupted
     instead, and the library yields when leaving the section. */
  volatile uint32_t u_in_critical;
  volatile uint32_t u_interrupted;
  volatile uint32_t u_in_epilogue;	/* Set by kernel, cleared on yield. */

  /* IPC: accept protected control transfers. */
  volatile uint32_t u_ipc_accept;

  /* INT vectors (0x30-0xff) redirected to u_entexcept, as a bitmap. */
  uint32_t u_intmask[8];

  /* Revocation: number of pages the kernel asks back. */
  volatile uint32_t u_revoke_npages;

  /* Statistics maintained by the kernel. */
  volatile uint32_t u_prologue_count;
  volatile uint32_t u_epilogue_count;
  volatile uint32_t u_excess_count;
  volatile uint32_t u_fault_count;
  volatile uint32_t u_yield_count;
  volatile uint32_t u_ipc_count;

  /* Message ring head (kernel write index), see msgring.h. */
  volatile uint32_t u_msgring_head;

  uint8_t u_pad[UENV_LIBOS_OFF - 28 * 4];

  /* Reserved to the library operating system. */
  uint8_t u_libos[4096 - UENV_LIBOS_OFF];
};

/* Public environment information (UENVINFO). */
struct envinfo
{
  envid_t e_id;
  envid_t e_parent;
  uint32_t e_status;
  int32_t e_cpu;		/* CPU running this env, or -1. */
  uint32_t e_npages;		/* Pages mapped. */
  uint32_t e_nquanta;		/* Quanta allocated. */
  uint64_t e_ticks;		/* Ticks consumed. */
  struct cap e_envcap;		/* Guard of this environment. */
  struct cap e_caps[ENV_NCAPS];	/* Capabilities owned. */
  uint8_t e_pad[512 - 48 - 16 * ENV_NCAPS];
};

#define uenv_of(_id) ((volatile struct Uenv *)(UENVS + ENVX(_id) * 4096))
#define envinfo_of(_id) (&((volatile struct envinfo *)UENVINFO)[ENVX(_id)])

#endif
