# Nyctos

A microkernel operating system, built to be read.

Nyctos is designed so that the same application-level interface runs whether
or not the hardware has a memory management unit. Flat memory and a full MMU
are two levels of *enforcement* of one set of rules, not two systems. A
correct program behaves identically on both, and what changes is whether the
hardware can catch an incorrect one.

Above that sit synchronous message passing with priority inheritance,
per-process namespaces assembled in the C library rather than in the kernel,
and a filesystem that is a protocol rather than a layer. File systems, device
drivers, pagers, loaders and supervisors are all ordinary processes.

The most speculative part is the submission engine: a thread can hand the
kernel an ordered array of operations whose dependencies are expressed as
fences rather than as control flow, executed in the submitting thread's own
context at its own priority, with no worker pool anywhere. It is kept
deliberately separable, because it is novel enough to be worth being wrong
about.

**The design is not built yet.** `docs/design.md` describes the intended
system in detail and nothing in it has been implemented or measured. What
currently runs is much smaller than what is described there, and the gap is
deliberate rather than hidden.

## Status

What works today: a cross-compiled kernel that boots under QEMU on AArch64 and
RISC-V64, drops to unprivileged mode, and runs a single userland program that
prints a banner and reads a file through the system call path.

What does not exist yet: the scheduler, IPC, handles, timelines, the MMU, and
every process beyond the first. There is a VFS, but it is inside the kernel,
which is the opposite of the design and is known to be temporary.

`docs/ROADMAP.md` names the first milestone and the order of work. It is one
vertical slice, deliberately small: one thread, one channel, one driver, one
file read. That is the thinnest path that exercises handles, message passing
and the filesystem protocol together, and the smallest thing that could show
the design to be wrong.

## Why it is shaped this way

The long-term intent is a system that can be read end to end and explained,
in the tradition of the Lions commentary and xv6, covering the userland
utilities as well as the kernel. That intent is not a deliverable and there
is no book being written. It is a constraint on the code: small functions,
a complete system rather than a kernel alone, and a standing refusal of scope
creep. Ideas that do not fit are recorded in `docs/PROPOSALS.md` rather than
built.

Nothing here has earned the right to teach anyone anything yet. It has to
work first.

## Documentation

| File | What it holds |
| --- | --- |
| `docs/design.md` | The intended system, in detail. Builds to `nyctos.pdf`. |
| `docs/ROADMAP.md` | What exists, what is next, and in what order. |
| `docs/PROPOSALS.md` | Deliberately not built, with the reasoning kept. |
| `docs/CODING_STYLE.md` | C style: KNF variant, 4-space indent, no tabs. |
| `docs/TODO.md` | Notes on the document build toolchain. |

## Requirements

The build runs on **Linux** or on **macOS with [Homebrew](https://brew.sh)**.
Windows with Chocolatey is a plausible third host and is not supported yet.

Whatever the host, four things are needed:

- **Clang/LLVM**, which cross-compiles and assembles for arbitrary targets,
  including `aarch64-none-elf` and `riscv64-none-elf`, without a separate GCC
  cross-toolchain per architecture.
- **LLD** (`ld.lld`), the linker used for both targets.
- **QEMU** (`qemu-system-aarch64`, `qemu-system-riscv64`).
- **GNU Make**.

[share/mk/nyc.own.mk](share/mk/nyc.own.mk) locates the toolchain itself. If
`brew` is present it asks Homebrew where its keg-only LLVM and LLD live;
otherwise it takes `clang`, `llvm-ar`, `llvm-objcopy` and `ld.lld` from
`PATH`. No manual `PATH` or shell profile changes are required on either host.

## Linux setup

Install the toolchain with the system package manager:

```sh
# Debian / Ubuntu
sudo apt install clang lld make qemu-system-arm qemu-system-misc

# Fedora
sudo dnf install clang lld make qemu-system-aarch64 qemu-system-riscv

# Arch
sudo pacman -S clang lld make qemu-system-aarch64 qemu-system-riscv
```

Verify:

```sh
clang --version && ld.lld --version
qemu-system-aarch64 --version
qemu-system-riscv64 --version
```

For debugging, `gdb-multiarch` (Debian/Ubuntu) or the distribution's plain
`gdb` built with multiarch support understands both targets and can attach to
QEMU's gdbstub.

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
would shadow Apple's own clang, which is why the build asks `brew` where they
are rather than trusting `PATH`.

## Project layout

The tree follows the layout of a BSD source tree: `sys` is the kernel,
`lib` is the userland libraries, `bin` is the userland programs, `include`
is the headers those programs compile against, `tools` holds host-side
build utilities, `etc` holds data files staged onto the initramfs, and
`share/mk` holds the make fragments that tie it all together.

```
.
├── Makefile                 # top level: recurses into lib, bin, tools, sys in that order
├── LICENSES/                # Apache-2.0 and 0BSD license texts
├── _out/                    # generated: <target-triplet>/ mirroring the source tree
├── docs/                    # design, roadmap, proposals, coding style
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
lines, and no hard tabs.

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

See [docs/ROADMAP.md](docs/ROADMAP.md). The next milestone is the vertical
slice: one thread, one channel, one driver, one file read, with the driver
as a separate process rather than kernel code.

The bring-up order that gets there is in the same file. It puts timelines
before IPC, which looks wrong and is not: interrupts and timers depend on
timelines and not on message passing, so building them first gets the
system responding to hardware several stages earlier.
