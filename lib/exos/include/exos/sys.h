/*
 * ExOS: Xok system call stubs.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef EXOS_SYS_H
#define EXOS_SYS_H

#include <stdint.h>
#include <stddef.h>
#include <xok/types.h>
#include <xok/syscall.h>
#include <xok/error.h>
#include <xok/cap.h>
#include <xok/env.h>

static inline int
__sys6 (int num, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
	uint32_t a5, uint32_t a6)
{
  int r = num;
  /* %ebp carries the sixth argument.  The operand is pushed first: a
     memory operand may be %esp-relative, and push computes its address
     before decrementing %esp. */
  asm volatile ("pushl %6\n\t"
		"pushl %%ebp\n\t"
		"movl 4(%%esp), %%ebp\n\t"
		"int $0x21\n\t"
		"popl %%ebp\n\t"
		"addl $4, %%esp":"+a" (r):"D" (a1), "S" (a2), "c" (a3), "d" (a4),
		"b" (a5), "g" (a6):"memory", "cc");
  return r;
}

static inline int
__sys5 (int num, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4,
	uint32_t a5)
{
  int r = num;
  asm volatile ("int $0x21":"+a" (r):"D" (a1), "S" (a2), "c" (a3), "d" (a4),
		"b" (a5):"memory", "cc");
  return r;
}

#define __sys0(n) __sys5 ((n), 0, 0, 0, 0, 0)
#define __sys1(n,a) __sys5 ((n), (uint32_t)(a), 0, 0, 0, 0)
#define __sys2(n,a,b) __sys5 ((n), (uint32_t)(a), (uint32_t)(b), 0, 0, 0)
#define __sys3(n,a,b,c) __sys5 ((n), (uint32_t)(a), (uint32_t)(b), (uint32_t)(c), 0, 0)
#define __sys4(n,a,b,c,d) __sys5 ((n), (uint32_t)(a), (uint32_t)(b), (uint32_t)(c), (uint32_t)(d), 0)

static inline int sys_cputs (const char *s, size_t n)
{ return __sys2 (SYS_cputs, s, n); }
static inline int sys_cgetc (void)
{ return __sys0 (SYS_cgetc); }
static inline int sys_gettime (uint64_t *ns)
{ return __sys1 (SYS_gettime, ns); }
static inline int sys_reboot (unsigned k, int poweroff)
{ return __sys2 (SYS_reboot, k, poweroff); }

static inline envid_t sys_getenvid (void)
{ return __sys0 (SYS_getenvid); }
static inline envid_t sys_env_alloc (unsigned k)
{ return __sys1 (SYS_env_alloc, k); }
static inline int sys_env_free (unsigned k, envid_t id)
{ return __sys2 (SYS_env_free, k, id); }
static inline int sys_env_set_status (unsigned k, envid_t id, unsigned st)
{ return __sys3 (SYS_env_set_status, k, id, st); }
static inline int sys_env_set_tf (unsigned k, envid_t id, const struct utf *tf)
{ return __sys3 (SYS_env_set_tf, k, id, tf); }
static inline int sys_env_setpriv (unsigned k, envid_t id, unsigned flags)
{ return __sys3 (SYS_env_setpriv, k, id, flags); }

static inline int sys_cap_forge (unsigned k, unsigned slot, const struct cap *c)
{ return __sys3 (SYS_cap_forge, k, slot, c); }
static inline int sys_cap_grant (unsigned ke, envid_t id, unsigned k, unsigned slot)
{ return __sys4 (SYS_cap_grant, ke, id, k, slot); }
static inline int sys_cap_clear (unsigned slot)
{ return __sys1 (SYS_cap_clear, slot); }

static inline int sys_insert_pte (unsigned k, xpte_t pte, uint32_t va,
				  unsigned ke, envid_t id)
{ return __sys6 (SYS_insert_pte, k, (uint32_t) pte, (uint32_t) (pte >> 32), va, ke, id); }
static inline int sys_self_insert_pte (unsigned k, xpte_t pte, uint32_t va)
{ return sys_insert_pte (k, pte, va, 0, 0); }
static inline int sys_insert_pte_range (unsigned k, const xpte_t *ptes,
					unsigned n, uint32_t va, unsigned ke,
					envid_t id)
{ return __sys6 (SYS_insert_pte_range, k, (uint32_t) ptes, n, va, ke, id); }
static inline int sys_mod_pte_range (unsigned k, unsigned ke, envid_t id,
				     uint32_t va, unsigned n, uint32_t set,
				     uint32_t clr)
{ return __sys6 (SYS_mod_pte_range, k, ke, id, va, n, (set & 0xffff) | (clr << 16)); }
static inline int sys_vpt_refresh (uint32_t va, unsigned n)
{ return __sys2 (SYS_vpt_refresh, va, n); }
static inline int sys_ppage_acl (unsigned k, ppn_t ppn, const struct cap *g)
{ return __sys3 (SYS_ppage_acl, k, ppn, g); }

static inline int sys_quantum_alloc (unsigned k, int q, unsigned cpu, envid_t id)
{ return __sys4 (SYS_quantum_alloc, k, q, cpu, id); }
static inline int sys_quantum_free (unsigned k, int q, unsigned cpu)
{ return __sys3 (SYS_quantum_free, k, q, cpu); }
static inline int sys_quantum_set (unsigned k, int q, unsigned cpu, envid_t id)
{ return __sys4 (SYS_quantum_set, k, q, cpu, id); }
static inline int sys_cpu_revoke (unsigned k, unsigned cpu, envid_t id)
{ return __sys3 (SYS_cpu_revoke, k, cpu, id); }
static inline int sys_yield (envid_t id)
{ return __sys1 (SYS_yield, id); }
static inline int sys_wkpred (const void *terms, unsigned n)
{ return __sys2 (SYS_wkpred, terms, n); }

/* Protected control transfer: returns r0, stores r1 in *r1 if not NULL. */
static inline int
sys_ipc_call (envid_t to, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
	      uint32_t *r1)
{
  int r = SYS_ipc_call;
  uint32_t d = a3;
  asm volatile ("int $0x21":"+a" (r), "+d" (d):"D" (to), "S" (a0), "c" (a1),
		"b" (a2):"memory", "cc");
  /* Note: %edx carried a3 in and carries r1 out. */
  if (r1)
    *r1 = d;
  return r;
}

static inline int sys_ipc_reply (envid_t to, uint32_t r0, uint32_t r1)
{ return __sys3 (SYS_ipc_reply, to, r0, r1); }
static inline int sys_msgring_setring (void *ring, unsigned n)
{ return __sys2 (SYS_msgring_setring, ring, n); }
static inline int sys_msgring_delring (void)
{ return __sys0 (SYS_msgring_delring); }
static inline int sys_ipc_sendmsg (envid_t to, const void *msg, unsigned n)
{ return __sys3 (SYS_ipc_sendmsg, to, msg, n); }

static inline int sys_sreg_create (unsigned k, unsigned size)
{ return __sys2 (SYS_sreg_create, k, size); }
static inline int sys_sreg_destroy (unsigned k, unsigned id)
{ return __sys2 (SYS_sreg_destroy, k, id); }
static inline int sys_sreg_read (unsigned k, unsigned id, unsigned off,
				 void *buf, unsigned len)
{ return __sys5 (SYS_sreg_read, k, id, off, (uint32_t) buf, len); }
static inline int sys_sreg_write (unsigned k, unsigned id, unsigned off,
				  const void *buf, unsigned len)
{ return __sys5 (SYS_sreg_write, k, id, off, (uint32_t) buf, len); }

static inline int sys_net_xmit (unsigned card, const void *recs, unsigned n,
				volatile uint32_t *notify)
{ return __sys4 (SYS_net_xmit, card, recs, n, notify); }
static inline int sys_dpf_insert (unsigned k, const void *atoms, unsigned n,
				  unsigned ring)
{ return __sys4 (SYS_dpf_insert, k, atoms, n, ring); }
static inline int sys_dpf_delete (unsigned k, unsigned fid)
{ return __sys2 (SYS_dpf_delete, k, fid); }
static inline int sys_dpf_ref (unsigned k, unsigned fid, unsigned ke, envid_t id)
{ return __sys4 (SYS_dpf_ref, k, fid, ke, id); }
static inline int sys_dpf_pktring (unsigned k, unsigned fid, unsigned ring)
{ return __sys3 (SYS_dpf_pktring, k, fid, ring); }
static inline int sys_pktring_setring (const void *ents, unsigned n)
{ return __sys2 (SYS_pktring_setring, ents, n); }
static inline int sys_pktring_modring (unsigned ring, unsigned idx, const void *ent)
{ return __sys3 (SYS_pktring_modring, ring, idx, ent); }
static inline int sys_pktring_delring (unsigned ring)
{ return __sys1 (SYS_pktring_delring, ring); }

static inline int sys_disk_request (unsigned k, unsigned dev, uint32_t sector,
				    unsigned nsect, int write, ppn_t ppn,
				    volatile uint32_t *done)
{ return __sys6 (SYS_disk_request, k, dev, sector, nsect | (write ? 0x80000000u : 0), ppn, (uint32_t) done); }

/* XN. */
static inline int sys_xn_format (unsigned k, unsigned dev)
{ return __sys2 (SYS_xn_format, k, dev); }
static inline int sys_xn_type_install (unsigned dev, const void *t)
{ return __sys2 (SYS_xn_type_install, dev, t); }
static inline int sys_xn_type_lookup (unsigned dev, const char *name)
{ return __sys2 (SYS_xn_type_lookup, dev, name); }
static inline int sys_xn_root_install (unsigned k, unsigned dev, const void *r)
{ return __sys3 (SYS_xn_root_install, k, dev, r); }
static inline int sys_xn_root_lookup (unsigned dev, const char *name, void *out)
{ return __sys3 (SYS_xn_root_lookup, dev, name, out); }
static inline int sys_xn_bind (unsigned dev, uint32_t blk, uint32_t parent)
{ return __sys3 (SYS_xn_bind, dev, blk, parent); }
static inline int sys_xn_readin (unsigned dev, uint32_t blk, unsigned n)
{ return __sys3 (SYS_xn_readin, dev, blk, n); }
static inline int sys_xn_insert_pte (unsigned k, unsigned dev, uint32_t blk,
				     uint32_t va, int writable, unsigned ke,
				     envid_t id)
{ return __sys6 (SYS_xn_insert_pte, k, dev, blk, va | (writable ? 1 : 0), ke, id); }
static inline int sys_xn_alloc (unsigned k, unsigned dev, uint32_t parent, const void *op)
{ return __sys4 (SYS_xn_alloc, k, dev, parent, op); }
static inline int sys_xn_free (unsigned k, unsigned dev, uint32_t parent, const void *op)
{ return __sys4 (SYS_xn_free, k, dev, parent, op); }
static inline int sys_xn_modify (unsigned k, unsigned dev, uint32_t blk, const void *op)
{ return __sys4 (SYS_xn_modify, k, dev, blk, op); }
static inline int sys_xn_writeback (unsigned dev, uint32_t blk, unsigned n)
{ return __sys3 (SYS_xn_writeback, dev, blk, n); }
static inline int sys_xn_unbind (unsigned dev, uint32_t blk)
{ return __sys2 (SYS_xn_unbind, dev, blk); }
static inline int sys_xn_lock (unsigned k, unsigned dev, uint32_t blk, int lock)
{ return __sys4 (SYS_xn_lock, k, dev, blk, lock); }
static inline int sys_xn_lookup (unsigned dev, uint32_t blk)
{ return __sys2 (SYS_xn_lookup, dev, blk); }
static inline int sys_xn_sync (unsigned dev)
{ return __sys1 (SYS_xn_sync, dev); }

static inline int sys_debug (unsigned op, uint32_t arg)
{ return __sys2 (SYS_debug, op, arg); }

#endif
