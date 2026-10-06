/*
 * Xok inter-process communication.
 *
 * Two mechanisms are provided:
 *
 * 1. Protected control transfer (as in Aegis/Xok): sys_ipc_call()
 *    transfers the CPU, atomically, to the callee's u_entipc upcall,
 *    donating the remainder of the caller's time slice; the kernel
 *    passes the arguments in the upcall frame (utf_arg) and never
 *    touches anything else.  The callee answers with sys_ipc_reply(),
 *    which transfers control back and makes the caller's sys_ipc_call
 *    return the reply values.
 *
 * 2. Message rings (as in SMP-Xok): an environment registers a ring of
 *    message buffers in its own memory; sys_ipc_sendmsg() copies a
 *    small message into the next free entry of the target's ring
 *    without transferring control.  The receiver frees an entry by
 *    clearing its flag, and can sleep with a wakeup predicate on the
 *    flag of the next entry.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef XOK_IPC_H
#define XOK_IPC_H

#include <xok/types.h>

#define MSGRING_MSGSIZE 32
#define MSGRING_MAXENTS 64

struct msgring_ent
{
  volatile uint32_t m_flag;	/* 0: free (kernel may fill); 1: full. */
  envid_t m_from;		/* Sender. */
  uint32_t m_size;		/* Message size. */
  uint8_t m_buf[MSGRING_MSGSIZE];
};

#endif
