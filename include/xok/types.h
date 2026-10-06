/*
 * Xok/ExOS: an exokernel for i386 on NUX.
 *
 * Basic ABI types shared by the kernel and library operating systems.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef XOK_TYPES_H
#define XOK_TYPES_H

#include <stdint.h>

/*
 * Environment identifiers.
 *
 * An envid encodes a slot index (low ENVX_BITS bits) and a generation
 * number, so that stale identifiers of destroyed environments are
 * never confused with a new environment reusing the slot.  Valid
 * envids are always > 0.
 */
typedef int envid_t;

#define ENVX_BITS   8
#define NENV        (1 << ENVX_BITS)
#define ENVX(_id)   ((_id) & (NENV - 1))

/* Physical page numbers, as exposed to applications. */
typedef unsigned int ppn_t;

/* Page table entries, in the hardware (x86 PAE) format. */
typedef uint64_t xpte_t;

/* Disk block numbers (XN uses 4KB blocks). */
typedef unsigned int blk_t;

/* Maximum CPUs supported by the kernel. */
#define XOK_MAXCPU 8

#endif
