/*
 * Xok system information, exported read-only at USYSINFO.
 *
 * Following the exokernel principle of exposing information, the
 * kernel publishes time, the per-CPU quantum vectors (the CPU
 * schedule), physical memory occupancy and the devices it multiplexes.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef XOK_SYSINFO_H
#define XOK_SYSINFO_H

#include <xok/types.h>

/* Number of time slices in each CPU's quantum vector. */
#define NQUANTA 64

struct quantum
{
  envid_t q_env;		/* Env scheduled on this slice (0 = free). */
  uint32_t q_ticks;		/* Ticks consumed by the slice. */
};

#define XOK_MAXDISK 4
#define XOK_MAXNET  4

struct diskinfo
{
  uint32_t d_present;
  uint32_t d_nsectors;		/* 512-byte sectors. */
  uint32_t d_nblocks;		/* XN 4KB blocks. */
  uint32_t d_xn;		/* 1 if an XN file system is mounted. */
  char d_name[16];		/* Driver / controller name. */
};

struct netinfo
{
  uint32_t n_present;
  uint8_t n_mac[6];
  uint16_t n_mtu;
  volatile uint32_t n_link;
  volatile uint32_t n_rxpkts;
  volatile uint32_t n_txpkts;
  volatile uint32_t n_rxdrop;	/* No filter / ring full. */
  char n_name[16];
};

struct cpuinfo
{
  volatile envid_t c_env;	/* Env currently running (0 = idle). */
  volatile uint32_t c_q;	/* Current slice in the quantum vector. */
  volatile uint64_t c_idle_ticks;
};

struct sysinfo
{
  volatile uint64_t si_nsec;	/* Nanoseconds since boot. */
  volatile uint64_t si_ticks;	/* Timer ticks since boot. */
  uint64_t si_boot_unix;	/* Seconds since the epoch at boot (RTC). */
  uint32_t si_tick_nsec;	/* Length of a time slice. */
  uint32_t si_ncpu;
  uint32_t si_nquanta;
  uint32_t si_nppages;		/* Number of physical pages (max ppn). */
  volatile uint32_t si_nfreepages;
  volatile uint32_t si_cons_in;	/* Count of console input characters. */
  uint32_t si_ndisk;
  uint32_t si_nnet;
  struct diskinfo si_disk[XOK_MAXDISK];
  struct netinfo si_net[XOK_MAXNET];
  struct cpuinfo si_cpu[XOK_MAXCPU];
  struct quantum si_qvec[XOK_MAXCPU][NQUANTA];
};

#define sysinfo_page ((volatile struct sysinfo *) USYSINFO)

#endif
