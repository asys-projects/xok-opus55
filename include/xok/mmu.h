/*
 * Xok memory management ABI: page table entry format and the layout
 * of the user virtual address space.
 *
 * Xok runs with x86 PAE paging; page table entries are exposed to
 * applications in the hardware format (64 bits).  All hardware defined
 * per-page attributes and the three software-available bits are under
 * the control of the application; non-present entries may contain any
 * value the library operating system wishes (they are kept only in the
 * exported page table, see UVPT).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef XOK_MMU_H
#define XOK_MMU_H

#define PGSHIFT 12
#define PGSIZE  4096
#define PGMASK  (PGSIZE - 1)

#define PGROUNDDOWN(_x) ((_x) & ~(PGSIZE - 1))
#define PGROUNDUP(_x)   (((_x) + PGSIZE - 1) & ~(PGSIZE - 1))

/* PTE bits (x86 PAE format). */
#define PTE_P      0x001ULL	/* Present. */
#define PTE_W      0x002ULL	/* Writable. */
#define PTE_U      0x004ULL	/* User accessible (always set by Xok). */
#define PTE_PWT    0x008ULL	/* Write-through. */
#define PTE_PCD    0x010ULL	/* Cache disable. */
#define PTE_A      0x020ULL	/* Accessed (hardware). */
#define PTE_D      0x040ULL	/* Dirty (hardware). */
#define PTE_G      0x100ULL	/* Global (not available to users). */
#define PTE_AVAIL0 0x200ULL	/* Software bits, for the libOS. */
#define PTE_AVAIL1 0x400ULL
#define PTE_AVAIL2 0x800ULL
#define PTE_AVAIL  0xe00ULL
#define PTE_NX     0x8000000000000000ULL
#define PTE_FRAME  0x000ffffffffff000ULL

/* Flags an application may request on a present mapping. */
#define PTE_USER_FLAGS (PTE_P | PTE_W | PTE_U | PTE_AVAIL | PTE_NX)

#define PTE_PPN(_pte)   ((uint32_t)(((_pte) & PTE_FRAME) >> PGSHIFT))
#define PPN2PTE(_ppn)   ((xpte_t)(_ppn) << PGSHIFT)

/*
 * User virtual address space layout.
 *
 *  0xc0000000 UTOP      +------------------------------+
 *                       | NUX kernel (inaccessible)    |
 *  0xc0000000           +------------------------------+
 *                       | Kernel-exported read-only    |
 *                       | structures (UXOK region):    |
 *                       |  UVPT, USYSINFO, UENVINFO,   |
 *                       |  UENVS, UBC, UPPAGES, UXN... |
 *  0xb8000000 UXOK_BASE +------------------------------+
 *                       | This env's u-area (RW)       |
 *  0xb7fff000 UAREA     +------------------------------+
 *                       | free for the libOS           |
 *  0x00001000           +------------------------------+
 *                       | invalid (NULL guard)         |
 *  0x00000000           +------------------------------+
 *
 * Mappings in [UXOK_BASE, UTOP) and the UAREA page are managed by the
 * kernel; applications cannot insert page table entries there.  Pages
 * in the UXOK region are mapped on demand (on the first fault) by the
 * kernel, read-only.
 */
#define UTOP        0xc0000000UL
#define UXOK_BASE   0xb8000000UL

/* Exported page table of the current environment: UVPT[va >> 12]. */
#define UVPT        0xb8000000UL
#define UVPT_SIZE   0x00600000UL	/* 3GB / 4KB * 8 bytes */

/* System information page(s): time, CPUs, quantum vectors, devices. */
#define USYSINFO    0xb8800000UL
#define USYSINFO_SIZE 0x00010000UL

/* Public environment information, struct envinfo[NENV]. */
#define UENVINFO    0xb8810000UL
#define UENVINFO_SIZE 0x00020000UL

/* All environments' u-areas, read-only: struct Uenv at UENVS + ENVX*4K. */
#define UENVS       0xb8900000UL
#define UENVS_SIZE  0x00100000UL

/* Buffer cache registry, struct bc_entry[]. */
#define UBC         0xb8c00000UL
#define UBC_SIZE    0x00400000UL

/* Physical page information, struct ppage_info[maxppn]. */
#define UPPAGES     0xb9000000UL
#define UPPAGES_SIZE 0x02000000UL

/* XN free map (one bit per disk block, 1 = free), per disk. */
#define UXNFREE     0xbb000000UL
#define UXNFREE_SIZE 0x01000000UL

/* XN type and root catalogues, read-only. */
#define UXNCAT      0xbc000000UL
#define UXNCAT_SIZE 0x00100000UL

#define UXOK_TOP    0xbc100000UL

/* The environment's own u-area, writable. */
#define UAREA       0xb7fff000UL

/* Highest address usable by library operating systems. */
#define ULIM        UAREA

#if !defined(_ASSEMBLER) && defined(_XOK_USER)
#include <xok/types.h>
#define vpt ((volatile xpte_t *) UVPT)
#endif

#endif
