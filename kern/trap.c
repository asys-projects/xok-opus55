/*
 * Xok event delivery: exceptions, page faults and upcalls.
 *
 * Xok dispatches all hardware exceptions (save for system calls) to
 * the application: the kernel saves the interrupted register context
 * in a struct utf on the application's exception stack, and jumps to
 * the handler registered in the u-area, in user mode.  All the state
 * is in user memory, so applications resume from their own exceptions
 * without entering the kernel.  Exceptions that happen while already
 * running on the exception stack are stacked below the current frame.
 *
 * The FPU is switched lazily: a CPU's FPU contents belong to at most
 * one environment.  The state is saved eagerly when that environment
 * leaves the CPU, and restored on its first FPU instruction (#NM).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "env.h"

#define EFLAGS_TF 0x100
#define EFLAGS_DF 0x400
#define EFLAGS_IOPL 0x3000

static bool has_fxsr;
static uint8_t fpu_default[512] __attribute__ ((aligned (16)));

static inline unsigned long
read_cr0 (void)
{
  unsigned long r;
  asm volatile ("mov %%cr0, %0":"=r" (r));
  return r;
}

static inline void
write_cr0 (unsigned long r)
{
  asm volatile ("mov %0, %%cr0"::"r" (r));
}

static inline unsigned long
rd_cr4 (void)
{
  unsigned long r;
  asm volatile ("mov %%cr4, %0":"=r" (r));
  return r;
}

static inline void
wr_cr4 (unsigned long r)
{
  asm volatile ("mov %0, %%cr4"::"r" (r));
}

#define CR0_MP 0x2
#define CR0_EM 0x4
#define CR0_TS 0x8
#define CR0_NE 0x20
#define CR4_OSFXSR 0x200
#define CR4_OSXMMEXCPT 0x400

static void
fpu_save (uint8_t * area)
{
  if (has_fxsr)
    asm volatile ("fxsave (%0)"::"r" (area):"memory");
  else
    asm volatile ("fnsave (%0); fwait"::"r" (area):"memory");
}

static void
fpu_restore (const uint8_t * area)
{
  if (has_fxsr)
    asm volatile ("fxrstor (%0)"::"r" (area):"memory");
  else
    asm volatile ("frstor (%0)"::"r" (area):"memory");
}

void
fpu_init_cpu (void)
{
  uint32_t a, b, c, d;
  unsigned long cr0;

  asm volatile ("cpuid":"=a" (a), "=b" (b), "=c" (c), "=d" (d):"a" (1));
  has_fxsr = !!(d & (1 << 24));
  if (has_fxsr)
    {
      unsigned long cr4 = rd_cr4 ();
      cr4 |= CR4_OSFXSR;
      if (d & (1 << 25))
	cr4 |= CR4_OSXMMEXCPT;
      wr_cr4 (cr4);
    }
  cr0 = read_cr0 ();
  cr0 &= ~(CR0_EM | CR0_TS);
  cr0 |= CR0_MP | CR0_NE;
  write_cr0 (cr0);
  asm volatile ("fninit");
  fpu_save (fpu_default);
  write_cr0 (cr0 | CR0_TS);
  mycpu ()->fpu_owner = NULL;
}

/* Env E leaves this CPU: save its FPU state if it is live here. */
void
fpu_switch_out (struct env *e)
{
  struct pcpu *pc = mycpu ();

  if (pc == NULL || pc->fpu_owner != e)
    return;
  write_cr0 (read_cr0 () & ~CR0_TS);
  fpu_save (e->fpu);
  e->fpu_valid = true;
  pc->fpu_owner = NULL;
  write_cr0 (read_cr0 () | CR0_TS);
}

void
fpu_switch_in (struct env *e)
{
  struct pcpu *pc = mycpu ();

  if (pc->fpu_owner != e)
    write_cr0 (read_cr0 () | CR0_TS);
}

/* #NM: E uses the FPU. */
bool
fpu_trap (struct env *e)
{
  struct pcpu *pc = mycpu ();

  if (!(read_cr0 () & CR0_TS))
    return false;
  write_cr0 (read_cr0 () & ~CR0_TS);
  if (pc->fpu_owner != NULL && pc->fpu_owner != e)
    {
      fpu_save (pc->fpu_owner->fpu);
      pc->fpu_owner->fpu_valid = true;
    }
  fpu_restore (e->fpu_valid ? e->fpu : fpu_default);
  pc->fpu_owner = e;
  return true;
}

/*
 * Push an upcall frame for event KIND on E's exception stack and
 * redirect E to the corresponding handler.  E's address space must be
 * the current one.  Returns -1 if there is no handler or the frame
 * cannot be written.
 */
int
upcall_push (struct env *e, uint32_t kind, uint32_t trapno, uint32_t err,
	     uint32_t va, const uint32_t * args)
{
  struct Uenv *u = e->u;
  uctxt_t *tf = &e->tf;
  struct utf utf;
  uint32_t handler, sp, xtop, xsize;

  switch (kind)
    {
    case UPC_PROLOGUE:
      handler = u->u_entprologue;
      break;
    case UPC_EPILOGUE:
      handler = u->u_entepilogue;
      break;
    case UPC_FAULT:
      handler = u->u_entfault;
      break;
    case UPC_EXCEPT:
      handler = u->u_entexcept;
      break;
    case UPC_IPC:
      handler = u->u_entipc;
      break;
    case UPC_REVOKE:
      handler = u->u_entrevoke;
      break;
    default:
      return -1;
    }
  if (handler == 0 || handler >= UXOK_BASE)
    return -1;

  KASSERT (cpu_umap_current () == &e->umap);

  xtop = u->u_xstktop;
  xsize = u->u_xstksize ? u->u_xstksize : PAGE_SIZE;
  sp = tf->esp;
  if (xtop == 0 || (sp <= xtop && sp > xtop - xsize))
    /* No exception stack, or already on it: stack below. */
    sp = sp - UTF_GAP;
  else
    sp = xtop;
  sp -= sizeof (struct utf);
  sp &= ~3u;

  memset (&utf, 0, sizeof (utf));
  utf.utf_edi = tf->edi;
  utf.utf_esi = tf->esi;
  utf.utf_ebp = tf->ebp;
  utf.utf_oesp = tf->esp;
  utf.utf_ebx = tf->ebx;
  utf.utf_edx = tf->edx;
  utf.utf_ecx = tf->ecx;
  utf.utf_eax = tf->eax;
  utf.utf_eflags = tf->eflags;
  utf.utf_eip = tf->eip;
  utf.utf_esp = tf->esp;
  utf.utf_kind = kind;
  utf.utf_trapno = trapno;
  utf.utf_err = err;
  utf.utf_va = va;
  if (args)
    memcpy (utf.utf_arg, args, sizeof (utf.utf_arg));

  if (copyout (sp, &utf, sizeof (utf)) < 0)
    {
      kprintf ("xok: env %x: cannot write upcall frame at %x (pte %llx)\n",
	       e->id, sp, env_getpte (e, sp & ~PAGE_MASK));
      return -1;
    }

  tf->esp = sp;
  tf->eip = handler;
  tf->eax = kind;
  tf->eflags &= ~(EFLAGS_TF | EFLAGS_DF);
  return 0;
}

/*
 * Page fault in user mode, not resolved by the kernel.
 */
void
trap_fault (struct env *e, vaddr_t va, hal_pfinfo_t info)
{
  uint32_t err = 0;

  if ((info & HAL_PF_REASON_MASK) == HAL_PF_REASON_PROT)
    err |= 1;
  if (info & HAL_PF_INFO_WRITE)
    err |= 2;
  err |= 4;
  if (info & HAL_PF_INFO_EXE)
    err |= 16;

  e->u->u_fault_count++;
  if (upcall_push (e, UPC_FAULT, 14, err, va, NULL) < 0)
    {
      kprintf ("xok: env %x: unhandled page fault va %lx eip %x err %x\n",
	       e->id, va, e->tf.eip, err);
      env_destroy (e);
    }
}

/*
 * Exception in user mode.  General protection faults caused by 'int N'
 * with N redirected in the u-area are delivered as UPC_EXCEPT with
 * trapno N, after the int instruction.
 */
void
trap_except (struct env *e, unsigned ex)
{
  uint32_t err = e->tf.err;
  uint32_t trapno = ex;

  if (ex == 13 && (err & 7) == 2)
    {
      unsigned vec = err >> 3;
      if (vec < 256 && (e->u->u_intmask[vec / 32] & (1u << (vec % 32))))
	{
	  trapno = vec;
	  err = 0;
	  e->tf.eip += 2;
	}
    }
  else if (ex == 3 || ex == 4)
    ;

  if (upcall_push (e, UPC_EXCEPT, trapno, err, 0, NULL) < 0)
    {
      kprintf ("xok: env %x: unhandled exception %u at eip %x\n", e->id,
	       ex, e->tf.eip);
      env_destroy (e);
    }
}

/*
 * Sanitise a user-supplied register context.
 */
void
utf_to_tf (const struct utf *utf, uctxt_t * tf, uint32_t priv)
{
  hal_frame_init (tf);
  tf->edi = utf->utf_edi;
  tf->esi = utf->utf_esi;
  tf->ebp = utf->utf_ebp;
  tf->ebx = utf->utf_ebx;
  tf->edx = utf->utf_edx;
  tf->ecx = utf->utf_ecx;
  tf->eax = utf->utf_eax;
  tf->eip = utf->utf_eip;
  tf->esp = utf->utf_esp;
  /* Arithmetic flags, DF, TF and AC only; IF is always on. */
  tf->eflags = (utf->utf_eflags & 0x40dd5) | 0x202;
  if (priv & ENV_PRIV_IO)
    tf->eflags |= EFLAGS_IOPL;
}
