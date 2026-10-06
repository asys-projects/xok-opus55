/*
 * Xok system call numbers.
 *
 * System calls are issued with 'int $0x21': %eax holds the system call
 * number and %edi, %esi, %ecx, %edx, %ebx, %ebp the arguments (in this
 * order).  The result is returned in %eax (and %edx for calls that
 * return two values).  All other registers are preserved.
 *
 * Arguments named 'k' are indices in the caller's capability list: the
 * capability the caller presents as credential for the operation.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef XOK_SYSCALL_H
#define XOK_SYSCALL_H

#define T_SYSCALL 0x21

enum
{
  SYS_null = 0,			/* () */

  /* Console and time. */
  SYS_cputs,			/* (const char *s, size_t len) */
  SYS_cgetc,			/* () -> char or -E_AGAIN */
  SYS_gettime,			/* (uint64_t *nsec) */
  SYS_reboot,			/* (k, int poweroff) root capability required */

  /* Environments. */
  SYS_getenvid,			/* () -> envid */
  SYS_env_alloc,		/* (k) -> envid; guard of new env = cap[k] */
  SYS_env_free,			/* (k, envid) */
  SYS_env_set_status,		/* (k, envid, status) */
  SYS_env_set_tf,		/* (k, envid, const struct utf *) */
  SYS_env_setpriv,		/* (k, envid, flags) I/O privileges (root) */

  /* Capabilities. */
  SYS_cap_forge,		/* (k, slot, const struct cap *c) c dominated by cap[k] */
  SYS_cap_grant,		/* (ke, envid, k, slot) give cap[k] to envid */
  SYS_cap_clear,		/* (slot) */

  /* Physical memory and page tables. */
  SYS_insert_pte,		/* (k, pte_lo, pte_hi, va, ke, envid) */
  SYS_insert_pte_range,		/* (k, const xpte_t *ptes, n, va, ke, envid) */
  SYS_mod_pte_range,		/* (k, ke, envid, va, n, set | clr << 16) */
  SYS_vpt_refresh,		/* (va, n) refresh A/D bits in UVPT */
  SYS_ppage_acl,		/* (k, ppn, const struct cap *guard) */

  /* CPU. */
  SYS_quantum_alloc,		/* (k, q, cpu, envid) -> q */
  SYS_quantum_free,		/* (k, q, cpu) */
  SYS_quantum_set,		/* (k, q, cpu, envid) */
  SYS_cpu_revoke,		/* (k, cpu, envid) */
  SYS_yield,			/* (envid or -1) */
  SYS_wkpred,			/* (const struct wk_term *t, n) install & sleep */

  /* IPC. */
  SYS_ipc_call,			/* (envid, a0, a1, a2, a3) -> (eax, edx) */
  SYS_ipc_reply,		/* (envid, r0, r1) */
  SYS_msgring_setring,		/* (struct msgring_ent *ring, n) */
  SYS_msgring_delring,		/* () */
  SYS_ipc_sendmsg,		/* (envid, const void *msg, size) */

  /* Software regions. */
  SYS_sreg_create,		/* (k, size) -> id; guard = cap[k] */
  SYS_sreg_destroy,		/* (k, id) */
  SYS_sreg_read,		/* (k, id, off, void *buf, len) */
  SYS_sreg_write,		/* (k, id, off, const void *buf, len) */

  /* Network. */
  SYS_net_xmit,			/* (card, const struct sendrec *, n, uint32_t *notify) */
  SYS_dpf_insert,		/* (k, const struct dpf_atom *, n, ring) -> fid */
  SYS_dpf_delete,		/* (k, fid) */
  SYS_dpf_ref,			/* (k, fid, ke, envid) */
  SYS_dpf_pktring,		/* (k, fid, ring) */
  SYS_pktring_setring,		/* (const struct pktring_ent *, n) -> ring */
  SYS_pktring_modring,		/* (ring, idx, const struct pktring_ent *) */
  SYS_pktring_delring,		/* (ring) */

  /* Raw disk access (root capability only). */
  SYS_disk_request,		/* (k, dev, sector, nsect | write << 31, ppn, uint32_t *done) */

  /* XN: protected stable storage. */
  SYS_xn_format,		/* (k, dev) root capability */
  SYS_xn_type_install,		/* (dev, const struct xn_template *) -> type */
  SYS_xn_type_lookup,		/* (dev, const char *name) -> type */
  SYS_xn_root_install,		/* (k, dev, const struct xn_root *) */
  SYS_xn_root_lookup,		/* (dev, const char *name, struct xn_root *out) */
  SYS_xn_bind,			/* (dev, blk, parent) -> bc index */
  SYS_xn_readin,		/* (dev, blk, n) start reading bound blocks */
  SYS_xn_insert_pte,		/* (k, dev, blk, va | writable, ke, envid) */
  SYS_xn_alloc,			/* (k, dev, parent, const struct xn_op *) */
  SYS_xn_free,			/* (k, dev, parent, const struct xn_op *) */
  SYS_xn_modify,		/* (k, dev, blk, const struct xn_op *) */
  SYS_xn_writeback,		/* (dev, blk, n) */
  SYS_xn_unbind,		/* (dev, blk) */
  SYS_xn_lock,			/* (k, dev, blk, lock) */
  SYS_xn_lookup,		/* (dev, blk) -> bc index or -E_NOT_INCORE */
  SYS_xn_sync,			/* (dev) flush free map and catalogues */

  SYS_debug,			/* (op, arg) kernel debugging aids */

  NSYSCALLS
};

/* SYS_env_setpriv flags. */
#define ENV_PRIV_IO 1		/* Allow I/O port access (IOPL 3). */

/* SYS_debug operations. */
#define DBG_PRINT_ENVS 1
#define DBG_PRINT_PAGES 2
#define DBG_PANIC 3

#endif
