/*
 * Xok network interface multiplexing: dynamic packet filters (DPF),
 * packet rings and packet transmission.
 *
 * Received packets are demultiplexed by the kernel with packet filters
 * downloaded by applications.  A filter is a conjunction of atoms; an
 * atom compares a masked header field with a constant.  Shift atoms
 * advance the base offset by a value computed from the packet (e.g.
 * the IP header length), allowing filters on variable-length headers.
 * Filters are merged in a trie; a packet is delivered to the most
 * specific (longest) matching filter.  A filter that would capture the
 * packets of an existing one (identical, or more specific) can only be
 * installed by presenting a capability dominating the existing
 * filter's guard: this is how a TCP library carves a connection out of
 * a listening end-point.
 *
 * Each filter is associated with a packet ring: a ring of buffers in
 * application memory, shared with the kernel.  Each entry has an
 * ownership word: zero means the kernel owns it.  The kernel copies a
 * matching packet into the current entry and writes the packet size in
 * the ownership word, thus handing the buffer to the application, which
 * returns it by writing zero.  If the current entry is not owned by
 * the kernel the packet is dropped.
 *
 * Transmission takes a gather list of <address, size> pairs and the
 * address of a counter that the kernel decrements when the packet
 * buffers can be reused.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef XOK_NET_H
#define XOK_NET_H

#include <stdint.h>

/* DPF atom operations. */
#define DPF_EQ    1		/* (load(size, base+off) & mask) == value */
#define DPF_SHIFT 2		/* base += (load(size, base+off) & mask) << value */

struct dpf_atom
{
  uint8_t a_op;
  uint8_t a_size;		/* 1, 2 or 4 bytes, network byte order. */
  uint16_t a_off;
  uint32_t a_mask;
  uint32_t a_val;
};

#define DPF_MAXATOMS 32
#define DPF_MAXFILTERS 256

/* Packet ring entry descriptor (passed to sys_pktring_setring). */
struct pktring_ent
{
  uint32_t pr_flag;		/* User VA of the 32-bit ownership word. */
  uint32_t pr_buf;		/* User VA of the buffer. */
  uint32_t pr_size;		/* Buffer size (buffer must not cross a page). */
};

#define PKTRING_MAXENTS 128
#define PKTRING_MAX 64

/* Gather list element for sys_net_xmit. */
struct sendrec
{
  uint32_t sr_va;
  uint32_t sr_len;
};

#define NET_MAXSENDRECS 8
#define NET_MAXFRAME 1514

#endif
