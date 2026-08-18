# Nyctos

A minimal, cross-compiled operating system targeting both **AArch64** and
**RISC-V64**, built on macOS and run under QEMU. It builds a kernel, its
own C library, and the userland programs that run on top of them, all from
one source tree laid out in the BSD tradition (`sys`, `lib`, `bin`,
`include`, `share`).

The first userland program, `/bin/motd`, prints the project banner and
exits — but it does so as an unprivileged program, calling into libc,
which traps into the kernel, which drives the UART.

## Goals

- Build a freestanding ELF kernel image for two different target
  architectures from the same macOS host, using Homebrew-provided tools
  only.
- Build a project-owned C library and C runtime startup object, and link
  userland programs against them.
- Boot each image directly in QEMU's `virt` machine, drop to unprivileged
  mode, and let a userland program print the banner over the serial
  console, proving that:
  - the cross compiler/assembler produces correct machine code for the
    target,
  - the linker scripts place kernel and userland at the addresses each is
    actually loaded and executed at,
  - the privilege transition and system call path work in both
    directions,
  - the emulator's memory map and UART model match what the kernel code
    assumes.
- Keep the build entirely driven by a GNU Make-based build system with
  shared `share/mk` fragments, selecting the target architecture via a
  single `ARCH` variable, with binaries and intermediaries for each
  architecture kept in their own output directory.

## Non-goals

This is intentionally *not* a general-purpose operating system yet. There
is no MMU/paging setup, no interrupt handling, no scheduler, no multi-core
support (secondary cores are parked), and no dynamic memory. Privilege
separation exists (userland runs at EL0 / U-mode) but is not backed by
memory protection, since the MMU is off and RISC-V PMP is configured
permissively. There is a file system, but it is read-only, has no
subdirectories, and is built once at compile time rather than written to
at runtime. It's a starting point to build further OS experiments on top
of.

## Requirements

- macOS (Apple Silicon or Intel) with [Homebrew](https://brew.sh) installed.
- Homebrew LLVM (`clang`), which can cross-compile and assemble for
  arbitrary targets — including `aarch64-none-elf` and `riscv64-none-elf` —
  without needing a separate GCC cross-toolchain per architecture.
- Homebrew LLD (`ld.lld`), the linker used for both targets. As of recent
  Homebrew versions this ships as its own formula, separate from `llvm`.
- QEMU (`qemu-system-aarch64`, `qemu-system-riscv64`), for emulating both
  target machines.
- GNU Make, to drive the build (ships with the Xcode Command Line Tools).

## macOS setup

1. Install Homebrew, if you don't already have it:

   ```sh
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   ```

2. Install the Xcode Command Line Tools (provides GNU Make and other basic
   build tools):

   ```sh
   xcode-select --install
   ```

3. Install the cross-compiler toolchain, linker, and emulator:

   ```sh
   brew install llvm lld qemu
   ```

4. Verify everything is in place:

   ```sh
   brew --prefix llvm && brew --prefix lld
   qemu-system-aarch64 --version
   qemu-system-riscv64 --version
   ```

5. (Optional, but recommended for debugging) Install a target-aware GDB
   for each architecture:

   ```sh
   brew install aarch64-elf-gdb riscv64-elf-gdb
   ```

   These understand the target's registers and calling convention, unlike
   the host's own `lldb`/`gdb`, and can attach to QEMU's built-in gdbstub
   (`qemu-system-aarch64 -s -S`, `qemu-system-riscv64 -s -S`) to step
   through kernel boot code instruction by instruction.

Homebrew keeps LLVM and LLD "keg-only" (not linked onto `PATH`) because they
would shadow Apple's own clang, so
[share/mk/nyc.own.mk](share/mk/nyc.own.mk) locates them itself via
`brew --prefix llvm` / `brew --prefix lld` — no manual `PATH` or shell
profile changes are required.

## Project layout

The tree follows the layout of a BSD source tree: `sys` is the kernel,
`lib` is the userland libraries, `bin` is the userland programs, `include`
is the headers those programs compile against, `tools` holds host-side
build utilities, `etc` holds data files staged onto the initramfs, and
`share/mk` holds the make fragments that tie it all together.

```
.
├── Makefile                 # top level: recurses into lib, bin, tools, sys in that order
├── LICENSE                  # Apache-2.0 license text
├── _out/                    # generated: <target-triplet>/ mirroring the source tree
├── docs/                    # design notes, coding style, AI prompt log
├── etc/
│   └── motd                 # a data file staged onto the initramfs, read by /bin/motd
├── share/
│   ├── mk/                  # toolchain config and shared build rules
│   └── ld/                  # userland linker scripts, one per arch
├── include/                 # userland headers: stdio.h, string.h, unistd.h, fcntl.h, sys/types.h
├── lib/
│   ├── csu/<arch>/crt1.S    # C runtime startup: stack, .bss, main(), exit()
│   └── libc/                # the C library
│       ├── arch/<arch>/sys/ # system call trap stub
│       ├── sys/             # system call wrappers (write, open, read, close, _exit)
│       ├── stdio/ stdlib/ string/
│       └── include/         # libc-private headers
├── bin/
│   └── motd/                # the first userland program: prints the banner and /etc/motd
├── tools/
│   └── mkfsimg/mkfsimg.c         # host tool: builds the initramfs image at build time
└── sys/                     # the kernel
    ├── sys/
    │   ├── syscall.h        # kernel/libc syscall ABI
    │   ├── tfs.h             # the initramfs on-disk format; ABI between mkfsimg and ramfs.c
    │   └── vfs.h              # the kernel-internal VFS interface
    ├── kern/                 # machine-independent: init, console, loader, syscalls, VFS
    │   ├── vfs.c              # the VFS layer: today, a thin forward to ramfs
    │   ├── ramfs.c            # the ramdisk driver: parses the TFS image
    │   └── kern_file.c        # the open file table backing the read/write/close syscalls
    └── arch/
        ├── aarch64/
        │   ├── boot.S           # reset entry, EL2→EL1 drop, .bss, stack, vectors
        │   ├── exception.S      # EL1 vector table and SVC system call path
        │   ├── machdep.S        # drop to EL0
        │   ├── uart.c           # PL011 UART driver (QEMU virt @ 0x09000000)
        │   └── conf/kern.ld     # places the kernel at 0x40080000
        └── riscv64/
            ├── boot.S           # reset entry, .bss, stack, mtvec
            ├── exception.S      # M-mode trap entry and ecall system call path
            ├── machdep.S        # PMP setup and drop to U-mode
            ├── uart.c           # 16550A UART driver (QEMU virt @ 0x10000000)
            └── conf/kern.ld     # places the kernel at 0x80000000
```

## The initramfs

`/bin/motd` and `/etc/motd` live on a RAM disk populated at boot time,
rather than being embedded in the kernel image directly. The pieces:

- **The on-disk format** ([sys/sys/tfs.h](sys/sys/tfs.h)) is TFS: a
  header, a flat table of `{ name, offset, size }` directory entries, and
  the file data itself. There are no subdirectories — a name like
  `bin/motd` is one opaque string compared whole — which is the same
  shape QNX's IFS and Plan 9's boot floppy use for a boot-time root.
- **`tools/mkfsimg`** is a *host* tool: it runs on the machine doing the
  build, not the target, and assembles a TFS image out of ordinary files
  on disk (`bin/motd=<path>`, `etc/motd=<path>`, ...). This is the same
  division of labour as `makefs`/`genromfs`: a small offline tool builds
  the boot filesystem once, so the on-target driver only ever has to read
  it.
- **QEMU loads the image itself**: the top-level `Makefile`'s `run`
  target hands the built `initramfs.img` to QEMU's generic
  `-device loader,file=...,addr=INITRAMFS_ADDR`, which places it verbatim
  in RAM at a fixed physical address (`INITRAMFS_ADDR` in
  [share/mk/nyc.own.mk](share/mk/nyc.own.mk)) alongside the kernel — the
  same mechanism QEMU uses to load firmware/coprocessor blobs, not a
  Linux-specific boot convention. The kernel doesn't need to be told the
  image's size: the TFS header is self-describing (magic, file count,
  and each entry's own offset/size), so `ramfs_init()` only needs the
  fixed address.
- **`sys/kern/ramfs.c`** is the RAM disk driver: it parses the TFS image
  by linear scan of its directory table.
- **`sys/kern/vfs.c`** is the VFS layer proper. There is exactly one
  filesystem today, so `vfs_lookup()` forwards straight to `ramfs.c`; this
  is the seam where a table of mounted filesystems goes if a second
  backing store ever shows up.
- **`sys/kern/kern_file.c`** is a small open file table backing the
  `open`/`read`/`close` system calls, mapping a file descriptor to a
  vnode and a read offset.

`exec_user()` in [sys/kern/kern_exec.c](sys/kern/kern_exec.c) now looks
`bin/motd` up through the VFS instead of trusting a linker symbol, which
is what makes it possible for `/bin/motd` itself to `open()` and `read()`
`/etc/motd` off the same filesystem from userland.

## How a userland program reaches the console

`/bin/motd` is no longer embedded in the kernel image directly; it's
looked up on the initramfs at boot (see "The initramfs" above) and copied
to the address it was linked for before the kernel drops to unprivileged
mode at its entry point:

```
bin/motd  puts()  ─────▶  libc write()  ─────▶  svc / ecall
                                                    │
                                                    ▼
           UART  ◀─────  kernel console  ◀─────  trap handler
```

`/bin/motd` also opens and reads `/etc/motd` off the same initramfs,
through the same trap path:

```
bin/motd  open()/read()  ───▶  libc  ───▶  svc / ecall
                                              │
                                              ▼
                          kern_file.c  ◀───  trap handler
                              │
                              ▼
                          vfs.c ──▶ ramfs.c ──▶ initramfs.img
```

System call numbers live in [sys/sys/syscall.h](sys/sys/syscall.h), which
is the single ABI header shared by the kernel and libc.


## Building

`ARCH` selects the target architecture and defaults to `aarch64`. Each
`make` invocation builds exactly one architecture; to build another,
re-invoke `make` with a different `ARCH`. Building both is two commands,
not one:

```sh
make                    # builds ARCH=aarch64 (the default)
make ARCH=riscv64       # builds riscv64 instead
```

Outputs land in `_out/<target-triplet>/`, keyed by the clang target triplet
so the two architectures' object files, `.d` dependency files, libraries and
images never collide. The tree below `_out/<target-triplet>/` mirrors the
source tree, with `lib/libc.a` and `lib/crt1.o` staged where userland
programs link against them, and the final image at `sys/kernel.elf`. Header
dependencies are tracked automatically (via `-MMD -MP`) and picked up on the
next build, so editing a shared header like
[sys/sys/syscall.h](sys/sys/syscall.h) correctly rebuilds every `.c`/`.S`
file that includes it.

```sh
make clean                 # removes _out/aarch64-none-elf/ only
make ARCH=riscv64 clean    # removes _out/riscv64-none-elf/ only
make cleandir              # removes _out/ entirely
```

`make clean` only ever removes the current `ARCH`'s output directory; it
never touches another architecture's build outputs.

## Running under QEMU

```sh
make run                  # aarch64 (the default)
make ARCH=riscv64 run     # riscv64
```

Both run QEMU with `-nographic`, so console I/O appears directly in your
terminal. Expected output looks like:

```
Nyctos/aarch64 kernel booting
init: exec /bin/motd
    _   __           __
   / | / /_  _______/ /_____  _____
  /  |/ / / / / ___/ __/ __ \/ ___/
 / /|  / /_/ / /__/ /_/ /_/ (__  )
/_/ |_/\__, /\___/\__/\____/____/
      /____/


Hello, world from userspace on aarch64!

Welcome to Nyctos. This message was read from /etc/motd on the
initramfs by /bin/motd, using open(2) and read(2).
init: /bin/motd (pid 1) exited; nothing left to run; system halted
```

To exit QEMU, press `Ctrl-A` then `X`.

## Memory map notes

| Arch    | Kernel load address | Initramfs load address | Userland load address | Why                                                                                       | UART            |
|---------|----------------------|------------------------|------------------------|--------------------------------------------------------------------------------------------|-----------------|
| aarch64 | `0x40080000`         | `0x40100000`           | `0x40200000`           | QEMU `virt` RAM starts at `0x40000000`; the `-kernel` loader places ELF images at `+0x80000` by default | PL011 @ `0x09000000` |
| riscv64 | `0x80000000`         | `0x80100000`           | `0x80200000`           | QEMU `virt` RAM base; with `-bios none` the reset vector jumps straight here                | 16550A @ `0x10000000` |

The userland load address appears in two places that must agree: the
userland linker script in `share/ld/`, and `USER_LOAD_ADDR` in
[share/mk/nyc.own.mk](share/mk/nyc.own.mk), which the kernel's loader is
compiled with. The initramfs load address works the same way:
`INITRAMFS_ADDR` is compiled into the kernel and is also the `addr=`
QEMU's `-device loader` is given, so both sides agree on where the image
landed without needing a devicetree lookup.

## Coding style

All C and assembly source in this project follows the style guide in
[docs/CODING_STYLE.md](docs/CODING_STYLE.md), a variant of the FreeBSD
kernel style(9) guide with 4-space indentation, 2-space continuation
lines, no hard tabs, and an Apache-2.0 default license.

## License

## Licensing

This operating system is dual-licensed under either of the following:

* Apache License, Version 2.0 ([LICENSE-APACHE](LICENSES/Apache-2.0.txt))
* BSD Zero Clause License ([LICENSE-0BSD](LICENSES/0BSD.txt))

You may choose which license you wish to follow when utilizing,
modifying, or distributing this software. All inbound contributions
must be submitted under this dual-license structure.

Every source file carries an `SPDX-License-Identifier` line and a short
copyright comment near the top, per [docs/CODING_STYLE.md](docs/CODING_STYLE.md#licensing).

## Known limitations

- One `make` invocation builds exactly one architecture at a time (selected
  via `ARCH`). This keeps the build system's variables simple and
  unambiguous instead of juggling per-architecture state in a single run;
  building for both architectures just means running `make` twice.
- The filesystem is a read-only RAM disk built once at compile time by a
  host tool; there is no way to create, write, or delete files at runtime,
  and no subdirectories.
- Userland runs unprivileged, but with the MMU off there is no memory
  protection behind that: a userland bug can still scribble on the kernel.

## Next steps

Ideas for extending this now that the kernel, libc, userland, and a
minimal filesystem are proven out:
- MMU/paging setup and a proper virtual memory layout, so the privilege
  separation is backed by real memory protection.
- An interrupt controller driver (GICv3 / PLIC) and a timer.
- A serial console input path (read side of the UART) and a shell in
  `bin/`.
- More of libc: `printf`, `malloc`, and the rest of `string.h`.
- Subdirectories, a real writable filesystem, and more system calls
  (`write` to a file, `stat`, `lseek`, `unlink`).
