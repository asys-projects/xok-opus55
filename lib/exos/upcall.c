/*
 * ExOS event handling: Xok upcalls.
 *
 * All upcalls enter at __exos_upcall (entry.S) on the exception stack,
 * with the interrupted context in a struct utf.
 *
 *  - Prologue (start of a time slice): run the context-switch add-ons
 *    (e.g. TCP timers), deliver pending signals, resume.
 *  - Epilogue (end of the slice): the interrupted context is already
 *    saved in the utf; yield the CPU.  When we are given the CPU again
 *    the kernel resumes us here (through the prologue), and we resume
 *    the interrupted context.
 *  - Page fault: copy-on-write, stack growth, or SIGSEGV.
 *  - Exceptions: translated to signals.
 *  - IPC: protected control transfers from other environments.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <exos/exos.h>
#include <exos/fs.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

void __exos_upcall (void);

#define MAX_ADDONS 8
static void (*cswitch_addons[MAX_ADDONS]) (void);
static int ncswitch;

void
exos_add_cswitch (void (*fn) (void))
{
  if (ncswitch < MAX_ADDONS)
    cswitch_addons[ncswitch++] = fn;
}

#define NIPC 16
static ipc_handler_t ipc_handlers[NIPC];

void
exos_ipc_register (unsigned op, ipc_handler_t h)
{
  if (op < NIPC)
    ipc_handlers[op] = h;
}

static int
ipc_ping (envid_t from, uint32_t a, uint32_t b, uint32_t c, uint32_t * r1)
{
  *r1 = a;
  return 0;
}

/*
 * Robust critical sections: while in one, Xok defers the epilogue and
 * flags the interruption; we give up the CPU when leaving.
 */
void
exos_crit_enter (void)
{
  __uenv->u_in_critical++;
}

void
exos_crit_leave (void)
{
  if (--__uenv->u_in_critical == 0 && __uenv->u_interrupted)
    {
      __uenv->u_interrupted = 0;
      sys_yield (-1);
    }
}

void
exos_yield (envid_t to)
{
  sys_yield (to);
}

static void
run_addons (void)
{
  static volatile int running;

  if (__uenv->u_in_critical || running)
    return;
  running = 1;
  for (int i = 0; i < ncswitch; i++)
    cswitch_addons[i] ();
  running = 0;
}

static void
handle_fault (struct utf *utf)
{
  uintptr_t va = utf->utf_va;
  xpte_t pte = va < UXOK_BASE ? exos_pte (va) : 0;

  /* Spurious (already resolved, e.g. by a nested handler). */
  if ((pte & PTE_P) && (!(utf->utf_err & 2) || (pte & PTE_W)))
    return;
  /* Copy-on-write. */
  if ((utf->utf_err & 2) && (pte & PTE_P) && (pte & PTE_COW))
    {
      if (exos_cow_fault (va) == 0)
	return;
    }
  /* Stack growth. */
  if (!(pte & PTE_P) && va >= USTACKBOT && va < USTACKTOP)
    {
      if (exos_page_alloc (PGROUNDDOWN (va), PTE_P | PTE_W | PTE_U) == 0)
	return;
    }
  exos_fault_signal (SIGSEGV, utf);
}

/* Handlers of redirected INT instructions. */
static void (*int_handlers[256]) (struct utf *);

int
exos_set_int_handler (unsigned vec, void (*h) (struct utf *))
{
  if (vec < 0x20 || vec >= 256)
    return -1;
  int_handlers[vec] = h;
  if (h)
    __uenv->u_intmask[vec / 32] |= 1u << (vec % 32);
  else
    __uenv->u_intmask[vec / 32] &= ~(1u << (vec % 32));
  return 0;
}

static void
handle_except (struct utf *utf)
{
  int sig;

  if (utf->utf_trapno >= 0x20 && utf->utf_trapno < 256
      && int_handlers[utf->utf_trapno])
    {
      int_handlers[utf->utf_trapno] (utf);
      return;
    }

  switch (utf->utf_trapno)
    {
    case 0:
    case 16:
    case 19:
      sig = SIGFPE;
      break;
    case 1:
    case 3:
      sig = SIGTRAP;
      break;
    case 6:
      sig = SIGILL;
      break;
    case 17:
      sig = SIGBUS;
      break;
    default:
      sig = SIGSEGV;
      break;
    }
  exos_fault_signal (sig, utf);
}

static void
handle_ipc (struct utf *utf)
{
  envid_t from = utf->utf_arg[4];
  uint32_t op = utf->utf_arg[0], r0 = -E_INVAL, r1 = 0;

  if (op < NIPC && ipc_handlers[op])
    r0 = ipc_handlers[op] (from, utf->utf_arg[1], utf->utf_arg[2],
			   utf->utf_arg[3], &r1);
  sys_ipc_reply (from, r0, r1);
  /* We continue here when next scheduled. */
}

void
__exos_upcall_c (struct utf *utf)
{
  switch (utf->utf_kind)
    {
    case UPC_PROLOGUE:
      run_addons ();
      break;
    case UPC_EPILOGUE:
      sys_yield (-1);
      break;
    case UPC_FAULT:
      handle_fault (utf);
      break;
    case UPC_EXCEPT:
      handle_except (utf);
      break;
    case UPC_IPC:
      handle_ipc (utf);
      break;
    case UPC_REVOKE:
      /* Give memory back: our buffer cache mappings are only a cache
         (XN can then reclaim the pages; we map them again on use). */
      xnl_unmap_all ();
      __uenv->u_revoke_npages = 0;
      break;
    default:
      break;
    }
  /* Deliver pending signals before resuming normal execution, unless
     we interrupted the library OS in a critical section. */
  if (__proc->sigpending && !__uenv->u_in_critical
      && !(utf->utf_esp <= XSTACKTOP && utf->utf_esp > XSTACKTOP - XSTACKSIZE))
    exos_sig_deliver ();
}

void
exos_init_upcalls (void)
{
  uint32_t entry = (uint32_t) __exos_upcall;

  /* U-area first: after fork, our data is copy-on-write and touching it
     requires the fault handler. */
  __uenv->u_xstktop = XSTACKTOP;
  __uenv->u_xstksize = XSTACKSIZE;
  __uenv->u_entfault = entry;
  __uenv->u_entexcept = entry;
  __uenv->u_entipc = entry;
  __uenv->u_entepilogue = entry;
  __uenv->u_entprologue = entry;
  __uenv->u_entrevoke = entry;
  /* Our buffer cache window can be repossessed. */
  __uenv->u_revoke_lo = BCWIN;
  __uenv->u_revoke_hi = BCWIN_TOP;
  __uenv->u_ipc_accept = 1;
  exos_ipc_register (IPC_PING, ipc_ping);
}

int
exos_ipc_send (envid_t to, uint32_t op, uint32_t a, uint32_t b, uint32_t c,
	       uint32_t * r1)
{
  int r;

  for (int tries = 0; tries < 100; tries++)
    {
      r = sys_ipc_call (to, op, a, b, c, r1);
      if (r != -E_BUSY)
	return r;
      sys_yield (-1);
    }
  return r;
}
