# Bugs fixed in NUX

The NUX submodule (`nux/`, branch `xok-fixes`) carries these fixes. Each
was confirmed by a failure of Xok or by reading the code against the
specification it implements; none is worked around in the Xok tree.

| Area | Bug | Effect | Fix |
| --- | --- | --- | --- |
| `libplt_acpi/acpitbl.h` | The GSI field of the MADT Interrupt Source Override was declared `uint8_t`; ACPI defines it as 32 bits. | The flags were read from the GSI's upper bytes (always 0): level-triggered PCI interrupts (flags `0x000d` in QEMU's tables) were programmed as edge-triggered. | `uint32_t gsi`. |
| `libnux/cpu.c` | `cpu_kmapupdate()` returned when the CPU existed (inverted test). | Kernel mapping changes (`kmap_commit`) were never propagated to any CPU's TLB. | Return when the CPU does not exist. |
| `libnux/cpu.c` | `cpu_nmiop()` never cleared the requested operations. | Every NMI repeated all past operations. | Consume the bits atomically. |
| `libnux/cpu.c` | `cpu_tlbflush_broadcast_sync()` is declared in `nux.h` but was not implemented. | Link failure; no way to wait for a remote TLB shootdown. | Implemented with a per-CPU flush generation counter (odd while flushing). |
| `libnux/uctxt.c` | `uctxt_seta2()` set argument register 1. | Wrong register. | Set register 2. |
| `libnux/pfnalloc.c` | `pfn_free()` counted freed pages even for custom allocators, while only the S-tree allocator decremented the count. | Wrong `pfn_avail()`. | Count in the S-tree free function. |
| `include/nux/cpumask.h` | `cpumask_set/clear` shifted an `int`. | Wrong for CPU ids >= 31. | Shift a `cpumask_t`. |
| `include/nux/cache.h` | When evicting a slot of the PFN cache, its red-black tree node was re-keyed and re-inserted without being removed. | Tree corruption, kernel page fault, as soon as more distinct pages than cache slots (256) were accessed with `pfn_get()`. | Remove the node before changing its key. |
| `libhal_x86/i386/pae32.c` | `hal_umap_init()` left the L3 entries (PDPTEs) empty; mapping a page in an empty gigabyte of a *loaded* umap went through the linear mapping, which only mirrors the PDPTEs (that the CPU caches at CR3 load) and is read-only. | Kernel page fault when a running process mapped memory in a new gigabyte of its address space. | Allocate all user L2 tables in `hal_umap_init()`, as `hal_umap_bootstrap()` already did. |
| `libhal_x86/i386/pae32.c` | Debug `printf`s on every page table allocation and release. | Console noise. | Removed. |
| `libnux/kmem.c` | The zone allocator keeps sizes in 64-byte units but the boundary tags (`___mkptr`, `___freeptr`, `___get_neighbors`) used them as byte counts. | Tails written inside blocks, or 7 bytes *before* one-unit blocks (corrupting the neighbour); kernel heap corruption after frees. | Convert units to bytes for addresses. |

Not fixed (outside the i386 scope of this project): the same L3/L4
allocation pattern may affect `libhal_x86/amd64/pae64.c`.
