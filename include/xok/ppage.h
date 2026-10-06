/*
 * Xok physical pages.
 *
 * Physical memory is exposed by name (physical page numbers).
 * Applications allocate pages explicitly, possibly asking for a
 * specific page, and the kernel records the guard capability of each
 * page.  The kernel publishes, read-only at UPPAGES, one ppage_info per
 * physical page: the free list, the reference counts (number of
 * mappings), the inverse mapping to the buffer cache registry, and so
 * on.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef XOK_PPAGE_H
#define XOK_PPAGE_H

#include <xok/types.h>

/* Page states. */
#define PP_RESERVED 0		/* Not RAM, or unusable. */
#define PP_FREE     1		/* On the free list. */
#define PP_KERNEL   2		/* Used by the kernel. */
#define PP_USER     3		/* Allocated to applications. */
#define PP_BC       4		/* Holds a buffer cache (XN) block. */

struct ppage_info
{
  uint8_t pp_state;
  uint8_t pp_pinned;		/* Pinned by kernel/device (DMA, rings). */
  uint16_t pp_refcnt;		/* Number of user mappings. */
  envid_t pp_owner;		/* Env that allocated the page. */
  uint32_t pp_bc;		/* Buffer cache registry index + 1, or 0. */
  uint32_t pp_pad;
};

#define ppages_info ((volatile struct ppage_info *) UPPAGES)

#endif
