# Xok/ExOS on NUX

A reimplementation of **Xok**, the MIT exokernel for x86, and of
**ExOS**, its UNIX library operating system, for i386, built on the
[NUX](https://github.com/glguida/nux) kernel library (a git submodule in
`nux/`).

Xok securely multiplexes the machine and leaves every abstraction to
untrusted library code:

* **CPU**: per-CPU vectors of time slices allocated by position, with
  prologue/epilogue upcalls at the beginning and end of each slice,
  directed yield, robust critical sections, wakeup predicates.
* **Memory**: physical pages exposed by name and guarded by capabilities;
  hardware page tables written only through system calls but exported
  read-only to applications (copy-on-write, paging policy etc. are in
  the library).
* **Disk**: XN, the extensible, protected stable storage system: library
  file systems describe their metadata with templates whose untrusted
  deterministic functions (UDFs) let the kernel verify every update;
  buffer cache registry shared by all applications; ordering rules for
  crash consistency.
* **Network**: dynamic packet filters (merged in a trie), packet rings
  in application memory, direct transmission.
* **Protection**: hierarchically named capabilities, explicit
  credentials on every system call, software regions, IPC by protected
  control transfer and message rings.

ExOS provides UNIX in a library: processes (`fork` with copy-on-write
over the exported page tables, `exec` with program text shared through
the buffer cache, `wait`, signals over IPC), a shared file table, pipes
on software regions, a TTY, the C-FFS library file system (embedded
inodes, on XN), and a TCP/IP stack linked in each application (sockets,
`poll`, DNS), plus `netd` (DHCP, ARP, ICMP). On top of it run a shell,
~50 utilities, and **httpd**, an event-driven web server in the spirit
of Cheetah.

See [doc/DESIGN.md](doc/DESIGN.md) for the design and how it maps to the
Xok papers, and [doc/NUX-FIXES.md](doc/NUX-FIXES.md) for the bugs fixed in
NUX.

## Building

Requirements: the `i686-unknown-elf` cross toolchain on `PATH`, a host C
compiler, autoconf, Python 3 and `qemu-system-i386` (for running and
testing).

```
git submodule update --init --recursive
./bootstrap.sh            # only if configure.ac changed
mkdir build && cd build
../configure              # ARCH=i386 is the default and only choice
make                      # NUX, the kernel, the library OS, the programs
make disk.img             # an XN disk with C-FFS holding /bin, /etc, /www
```

`build/xok.mb` is a multiboot image (NUX's APXH boot loader with the
kernel and the boot program, `init`, as payloads).

## Running

```
make qemu
```

boots QEMU (`-M pc`, 2 CPUs, IDE disk, e1000 NIC with user networking;
port 8080 of the host is forwarded to the guest's port 80 and UDP 5555
to the echo service).  `init` runs `/etc/rc` (which starts `netd` and
`httpd`) and a shell on the console. Try:

```
ls -l /bin
ps
sysinfo                       # what the exokernel exposes
ping 10.0.2.2
fetch http://localhost/status
cat /etc/motd | wc
```

and from the host: `curl http://localhost:8080/`.

Other configurations are supported (`QEMU_DISK`, `QEMU_NET` variables of
the Makefile): AHCI (`-M q35`), virtio-blk, NVMe, e1000e, RTL8139, NE2000 (`ne2k_pci`),
virtio-net.

## Testing

```
make check          # all configurations (takes a few minutes)
make check-quick    # main configuration, crash recovery, newfs
```

`tests/run-tests.py` boots the system in QEMU and drives the console:
it runs `xoktests` (tests of the Xok interfaces and their protection,
and of ExOS), shell and network tests (including requests from the host
to the web server), checks persistence across reboots and recovery
after the machine is killed without syncing, and verifies the disk
images on the host with `mkxnfs -c`, which runs the kernel's own UDF
interpreter over the file system.

## Layout

| Path | Contents |
| --- | --- |
| `include/xok/` | The Xok ABI: system calls, environments, capabilities, XN, DPF, ... |
| `kern/` | The Xok kernel (a NUX kernel) and its drivers |
| `lib/libc/` | The C library |
| `lib/exos/` | ExOS, the library operating system |
| `bin/` | `init`, the shell, utilities, `netd`, `httpd`, tests |
| `tools/mkxnfs.c` | Host tool: build and check XN/C-FFS disk images |
| `rootfs/` | Files installed in the disk image |
| `tests/` | QEMU-driven system tests |
| `nux/` | The NUX kernel library (submodule) |
