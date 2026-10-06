# Xok/ExOS on NUX: design

This document describes how this system implements the exokernel
architecture of Xok and its library operating system ExOS, as specified
in the MIT papers and theses:

* [SOSP97] Kaashoek et al., *Application Performance and Flexibility on
  Exokernel Systems*, SOSP 1997 — Xok, XN, ExOS.
* [ENG98] Engler, *The Exokernel Operating System Architecture*, PhD
  thesis, 1998 — principles, Xok/Aegis resource multiplexing, XN, UDFs.
* [ENG95] Engler, Kaashoek, O'Toole, *Exokernel: an Operating System
  Architecture for Application-Level Resource Management*, SOSP 1995.
* [TOCS02] Ganger et al., *Fast and Flexible Application-Level
  Networking on Exokernel Systems*, TOCS 2002 — Xok networking.
* [CHEN00] Chen, *Multiprocessing with the Exokernel Operating System*,
  2000 — SMP-Xok (quantum vectors per CPU, message rings, revocation).
* [DPF96] Engler, Kaashoek, *DPF: Fast, Flexible Message Demultiplexing
  using Dynamic Code Generation*, SIGCOMM 1996.
* [GANGER97] Ganger, Kaashoek, *Embedded Inodes and Explicit Grouping*,
  USENIX 1997 — C-FFS.

## 1. Structure

```
 +-------------------+  +------------------+  +---------------+
 | sh, utilities     |  | httpd (Cheetah-  |  | netd          |
 |                   |  |  like server)    |  | DHCP/ARP/ICMP |
 +-------------------+  +------------------+  +---------------+
 | libc + ExOS (UNIX in a library: processes, files, C-FFS,   |
 | TCP/IP, sockets, pipes, signals, TTY) - in every process   |
 +------------------------------------------------------------+
         system calls (int $0x21)      upcalls (u-area entry points)
 +------------------------------------------------------------+
 | Xok: environments, capabilities, physical memory, page     |
 | tables, CPU quantum vectors, wakeup predicates, IPC,       |
 | software regions, XN, DPF/packet rings, drivers            |
 +------------------------------------------------------------+
 | NUX: boot (APXH), HAL (i386 PAE), platform (ACPI, APIC,    |
 | HPET), per-CPU entry points, umap/kmap, kernel allocators  |
 +------------------------------------------------------------+
```

NUX provides a kernel as a set of event handlers: `entry_sysc`,
`entry_pf`, `entry_ex`, `entry_alarm`, `entry_ipi`, `entry_irq` each
receive the interrupted user context and return the context to resume.
Xok (`kern/`) implements these entries; it runs non-preemptively with
interrupts disabled, serialised by a big kernel lock on SMP, as the
original Xok did on uniprocessors [CHEN00 ch. 2 discusses finer
locking]. Every entry ends in `sched_return()`, which decides what
runs next on the CPU.

## 2. The kernel interface (Xok)

The ABI is in `include/xok/`. System calls use `int $0x21`, number in
`%eax`, up to six arguments in `%edi %esi %ecx %edx %ebx %ebp`.

### 2.1 Capabilities (`cap.h`, `kern/cap.c`)

"Xok performs access control through hierarchically-named capabilities
... All Xok calls require explicit credentials" [SOSP97 5.1]. A
capability is a name (up to 12 bytes) and permissions; A dominates B if
A's name is a prefix of B's. Each environment owns 16 capability slots
(kernel-maintained, readable by everyone in `UENVINFO`). Every system
call touching a protected resource takes the index of the capability
presented. Resources (pages, environments, quanta, software regions,
filters, XN roots) carry a guard capability. `sys_cap_forge` derives a
dominated capability; `sys_cap_grant` gives one to an environment the
caller controls.

ExOS uses `{'U', uid}` names for users (root has the empty name), and a
world capability `{'W'}` for the few shared tables.

### 2.2 Environments (`env.h`, `kern/env.c`)

An environment holds what is needed to run a program and deliver events
to it [ENG98 3.3.2]: an address space (a NUX umap), a saved context, the
upcall entry points and the exception stack, its capabilities. Each
environment has a **u-area** page (`struct Uenv`), mapped writable at
`UAREA` in its own address space and read-only for everybody at
`UENVS + slot * 4K`; its upper part is reserved to the library OS (ExOS
keeps the process table entry there, so `ps` and `wait` read every
process' state directly — the "process table partitioned across
application-reserved memory of Xok's environment structure" of
[SOSP97 5.2.1]).

### 2.3 Physical memory and page tables (`ppage.h`, `kern/pmem.c`)

Xok takes over the page allocator of NUX (`nux_set_allocator`) so that
physical memory is exposed by name: applications may allocate a
*specific* free page ("the kernel allows specific resources to be
requested during allocation" [SOSP97 3.1]) and the state of every page
(free list, reference counts, buffer-cache inverse mapping, pinning) is
exported read-only at `UPPAGES`. Pages are guarded by the capability
used to allocate them and are freed when no mapping refers to them
[TOCS02 3.2].

"Since the hardware does not verify that the physical page of a
translation can be mapped by a process, applications are prevented
from directly modifying the page table and must instead use system
calls" [ENG98 3.1.2]: `sys_insert_pte` (single or batched),
`sys_mod_pte_range` (batched flag changes, used by `fork`). The page
table is exported read-only at `UVPT` in the hardware (PAE) format,
including the three software bits and arbitrary values in non-present
entries ("Invalid entries can contain any value the library operating
system wishes"). The kernel keeps this exported table itself (one page
per 2MB of address space), so it is independent of NUX's page table
layout; `sys_vpt_refresh` copies the hardware accessed/dirty bits.

Kernel-exported regions (`UVPT`, `USYSINFO`, `UENVINFO`, `UENVS`,
`UPPAGES`, the XN registry, free maps and catalogues) are mapped on
demand, read-only, by the kernel's page-fault handler.

### 2.4 CPU (`sysinfo.h`, `kern/sched.c`)

"The CPU is represented as a linear vector, where each element
corresponds to a time slice ... Scheduling is done round robin"
[ENG98 3.3.1]; SMP-Xok has one vector per processor [CHEN00 3.4].
`sys_quantum_alloc/free/set` allocate slices by position (or the first
free one), guarded by capabilities; the vectors are exported in
`USYSINFO`. `sys_yield` donates the rest of the slice to a named
environment (directed yield) or ends it. `sys_cpu_revoke` forces the end
of a remote environment's slice.

"Timer interrupts denote the beginning and end of time slices, and are
delivered in a manner similar to exceptions": at the beginning of a
slice the environment is entered at its **prologue** upcall; at the end
it gets the **epilogue** upcall and must save its state and yield within
a grace period. An environment exceeding it is preempted and forfeits a
subsequent slice ("applications pay for each excess time slice consumed
by forfeiting a subsequent time slice"); environments without an
epilogue handler are context-switched by the kernel (the "more friendly
implementation"). While `u_in_critical` is set the epilogue is deferred
and `u_interrupted` set — Xok's robust critical sections "implemented
by disabling software interrupts" [SOSP97 3.3].

NUX exposes one platform alarm (the HPET): each CPU records its next
deadline, the alarm is programmed with the earliest one and the CPU
receiving it forwards expired deadlines to the others with IPIs.

The FPU is switched lazily (CR0.TS / #NM) and saved eagerly when an
environment leaves a CPU, so environments can migrate between CPUs.

### 2.5 Wakeup predicates (`wk.h`, `kern/wk.c`)

"Xok provides applications with the ability to inject wakeup predicates
into the kernel ... evaluated by the kernel when an environment is
about to be scheduled" [SOSP97 5.1]. A predicate is a sum of products
of comparisons between memory words, constants and the clock. Memory
operands are translated to physical addresses (and pinned) at
installation, so evaluation needs no address space switch. Operands can
also be words of software regions (used by pipes). Predicates are
interpreted, not compiled: the language has no loops and a bounded
number of terms ([ENG98 6.1] notes that a good interpreter is adequate
for such events).

### 2.6 IPC (`ipc.h`, `kern/ipc.c`)

Protected control transfer [ENG98 3.4.2]: `sys_ipc_call` transfers the
CPU atomically to the callee's IPC upcall with the arguments, donating
the rest of the slice; `sys_ipc_reply` transfers back and returns two
values. Message rings [CHEN00 3.2]: an environment registers a ring of
32-byte message buffers in its memory; `sys_ipc_sendmsg` copies a
message into the next free entry.

### 2.7 Software regions (`kern/sreg.c`)

"Areas of memory that can only be read or written through system calls
[provide] sub-page protection and fault isolation" [SOSP97 3.3], guarded
by a capability. ExOS pipes keep their data in software regions, and
readers sleep on a wakeup predicate over the region ("pipes are
implemented using Xok's software regions" [SOSP97 5.2.1]).

### 2.8 Events: exceptions and faults (`kern/trap.c`)

All exceptions are dispatched to the application [ENG98 3.4.1]: the
kernel pushes the interrupted context (`struct utf`) on the exception
stack (nesting below the current frame if already on it) and jumps to
the handler; the application resumes the context itself, without
entering the kernel. `INT n` instructions can be redirected to the
application (`u_intmask`), the facility Xok used for binary emulation
[SOSP97 7.1].

### 2.9 XN (`xn.h`, `kern/xn.c`, `kern/udf.c`)

XN follows [SOSP97 4] and [ENG98 4]:

* **Templates and UDFs.** A library file system installs a template for
  each of its metadata types in the (persistent) type catalogue. A
  template has an *owns-udf* (the blocks a metadata block points to,
  with their types), an *acl-uf* (access control, which may *defer* to
  the parent block) and a *size-uf*, written in a pseudo-RISC language
  (`UDF_*`) that the kernel verifies at installation (opcodes,
  registers, jump targets; owns-udfs cannot read credentials or
  arguments, which makes them deterministic) and interprets safely
  (bounds-checked loads, step budget). The acl-uf can see the proposed
  new contents of a block, to judge a modification.
* **Verification by induction.** Every metadata update is a list of
  byte modifications. XN applies them to a copy, runs owns-udf on old
  and new contents and checks that the owned set is unchanged
  (`sys_xn_modify`), grew by exactly the allocated blocks of the
  declared type (`sys_xn_alloc`, which also checks the blocks are free)
  or shrank by exactly the freed ones (`sys_xn_free`, which checks the
  freed metadata owns nothing). New types must own nothing when zeroed.
* **Buffer cache registry.** Maps cached blocks to physical pages, with
  their state (valid, dirty, in transit, uninitialised, locked), type,
  parent and taint; exported read-only (`UBC`). Blocks are bound to the
  registry through their parent (`sys_xn_bind` runs the parent's
  owns-udf to learn the child's type); reads may be started before
  (`sys_xn_readin`, "speculative reads"). Access control is done when a
  page is mapped (`sys_xn_insert_pte`), not when it is read. Metadata is
  never mapped writable. Registry pages are shared by all applications
  (ExOS maps program text from it); clean, unmapped buffers are
  reclaimed least recently used first when memory runs out.
* **Ordering rules.** A block pointing to an uninitialised block is
  *tainted* and cannot be written ("never create persistent pointers to
  structures before they are initialized"); a freed block is not reused
  until its parent, without the pointer, has been written ("never reuse
  an on-disk resource before nullifying all previous pointers to it").
  Roots can be temporary (no ordering constraints).
* **Roots and recovery.** Library file systems register their roots in
  the root catalogue. After an unclean shutdown XN rebuilds the free map
  by traversing the metadata from the roots with the owns-udfs.
* Locks on registry entries (`sys_xn_lock`) give libFSes atomic
  multi-step updates; any environment may write back any dirty block.

### 2.10 Networking (`net.h`, `kern/net.c`)

As in [TOCS02 3.2]: applications install packet filters
(`sys_dpf_insert`) associated with packet rings they own
(`sys_pktring_setring`: buffers and ownership words in their memory,
pinned by the kernel). Filters are conjunctions of atoms (masked
compares, and shifts for variable-length headers); they are merged in a
trie so that common prefixes are evaluated once, and a packet goes to
the most specific matching filter. Installing a filter identical to, or
more specific than, an existing one requires a capability dominating
the existing filter's guard ("so long as the creator has the proper
capability for the filter being subsetted"). Received packets are
copied into the current ring entry, whose ownership word receives the
packet size. `sys_net_xmit` sends a gather list and decrements a
notification counter. Frames sent to the interface's own address are
looped back through the filters.

### 2.11 Visible revocation (`kern/revoke.c`)

"Expose revocation" [SOSP97 3.1]; "when it is necessary to revoke an
allocation from a process, the kernel makes an upcall to that process —
the process can then pick a page ... and give it back" [TOCS02 3.2].
When free memory falls below a watermark the kernel asks the
environment holding most pages to release some (`UPC_REVOKE`, with the
count in `u_revoke_npages`); ExOS gives back its buffer-cache mappings.
If the environment does not comply within a grace period, the kernel
applies the *abort protocol* [ENG95]: it breaks the environment's
buffer-cache mappings in the range the library declared revocable
(`u_revoke_lo/hi`) and records them in the u-area's repossession
vector. XN reclaims clean, unmapped buffers when allocations fail.

### 2.12 Devices (`kern/`)

As in Xok, drivers are in the kernel and what is exported is a secure
multiplexing (XN, DPF), not the device. PCI enumeration; disks: ATA on
PCI IDE with bus-master DMA (PIO fallback, LBA48), AHCI, virtio-blk, NVMe;
NICs: Intel e1000 (82540/82545) and e1000e (82574), RTL8139,
NE2000 (RTL8029), AMD PCnet-PCI II, virtio-net; console on the serial line and VGA (NUX) with input from
COM1 and the PS/2 keyboard; CMOS RTC; ACPI power-off and reset. Drivers
are interrupt-driven and also polled at every tick, so a lost edge never
hangs a device.

## 3. The library OS (ExOS)

* **Runtime** (`start.c`, `entry.S`, `upcall.c`): all upcalls enter at
  one assembly stub on the exception stack. The epilogue handler yields;
  the prologue runs *context-switch add-ons* (the TCP timers); page
  faults implement copy-on-write and stack growth or become `SIGSEGV`.
* **Virtual memory** (`vm.c`): heap (`sbrk`), lazily grown stack,
  anonymous mappings, copy-on-write with a software PTE bit and the
  exported page table, run inside a robust critical section.
* **Processes** (`proc.c`): `fork` scans the exported page table and
  marks writable pages copy-on-write in batches ("ExOS scans through its
  page tables, which are exposed by Xok, marking all pages as
  copy-on-write except those data segment and stack pages that the fork
  call itself is using" [SOSP97 5.2.1]); pages pinned by the kernel are
  copied. `exec` builds a new environment: read-only segments are mapped
  straight from the XN buffer cache (shared by every process running the
  program), writable ones copied; the new environment inherits the pid
  and descriptors. `wait` sleeps on a wakeup predicate over the
  children's u-areas. Signals are IPC; senders are checked against the
  kernel-maintained capabilities of the caller.
* **Files** (`fd.c`, `posix.c`): the open file table is in shared
  memory, as in ExOS ("they name entries in a global file descriptor
  table, which is currently stored in shared memory"), with per-type
  operation tables; console TTY with line discipline; pipes on software
  regions; `/dev/null`, `/dev/zero`.
* **C-FFS** (`cffs.c`, `cffs_templates.c`, `xnlib.c`): inodes embedded in
  directory entries [GANGER97], file blocks allocated next to their
  directory (explicit grouping); templates for superblock, directory,
  indirect and data blocks with UDFs that encode UNIX permissions
  (owner/other bits, ownership required for chmod/chown, directory write
  permission for create/rename/unlink); locks on directory blocks during
  creation; a shared mount table; `cffs_mkfs` creates a file system from
  user space.
* **Networking** (`netif.c`, `socket.c`): every process runs its own
  TCP/IP stack [TOCS02 3.3]: Ethernet, ARP (cache shared with `netd`),
  IPv4, ICMP, UDP, TCP (handshakes, sliding window, retransmission,
  FIN/TIME_WAIT, RST), BSD sockets, `poll`/`select`, DNS. Listening
  sockets get a filter and a ring; accepted connections share the
  listener's ring and are demultiplexed in the library. Packets are
  processed when the application calls the stack, in the prologue
  add-on, and when a *wakeup-predicate add-on* (the rings and the TCP
  timers, OR'ed into every predicate the process installs) fires.

## 4. Deviations and limitations

* **Paging**: NUX's i386 HAL uses PAE, so exported PTEs are 64-bit PAE
  entries (Xok used 32-bit entries). Cache-control bits (PCD/PWT) are not
  settable because the NUX HAL does not expose them.
* **UDFs, DPF and wakeup predicates are interpreted**, not compiled to
  native code.
* **Concurrency**: one big kernel lock serialises the kernel on SMP.
* **XN** has no "move" operation, so C-FFS renames files across
  directories by copying them (directories cannot be moved); C-FFS has
  no hard links (inodes are embedded). Owned-set verification recomputes
  whole sets (no state partitioning).
* **Sockets** belong to the process that created them (the stack is in
  its memory): they are not shared across `fork`/`exec`.
* **ExOS** is statically linked (no shared ExOS library); program text
  is nevertheless shared through the buffer cache. No job control,
  process groups are minimal, `alarm` is not implemented.
* **Hardware**: ATAPI, floppy, USB, sound and graphics adapters are not
  supported; the network drivers cover e1000/e1000e, RTL8139,
  NE2000, PCnet and virtio-net (not the i8255x/eepro100 family, vmxnet3
  or ISA-only NICs). Disks: IDE, AHCI, virtio-blk, NVMe (no SCSI HBAs
  such as lsi/megasas/virtio-scsi).

## 5. NUX

The kernel uses NUX as a library; bugs found in NUX were fixed in the
submodule (branch `xok-fixes`), see [NUX-FIXES.md](NUX-FIXES.md).
