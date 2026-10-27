# Nyctos

A microkernel operating system, built to be read.

Nyctos runs the same application interface whether or not the hardware has a
memory management unit. Flat memory and a full MMU are two levels of
enforcement of one set of rules. A correct program behaves the same on both;
what changes is whether the hardware can catch an incorrect one.

Above that sit synchronous message passing with priority inheritance,
per-process namespaces assembled in the C library rather than in the kernel,
and a filesystem that is a protocol rather than a layer. Filesystems, device
drivers, pagers, loaders and supervisors are all ordinary processes.

A thread can also hand the kernel an ordered array of operations whose
dependencies are expressed as fences rather than control flow, run in the
submitting thread's own context at its own priority, with no worker pool. This
submission engine is kept separable because it is experimental.

## Status

Roughly half the kernel exists, with enough userland to use it. `docs/design.md`
describes the intended system and says which half is built; the gap narrows as
work lands.

What runs today:

- Two architectures, aarch64 and riscv64, on the QEMU `virt` board.
- Two protection tiers from one source: Tier 0 is flat memory with a trusted
  contract, Tier 2 is the same source with an MMU enforcing it. Tier 2 runs on
  two cores; Tier 0 keeps one.
- Processes with spawn, exit, join, kill and reaping, and frames reclaimed when
  a process ends.
- Synchronous message passing with priority inheritance, rendezvous with
  deadlines, and per-process namespaces with no global root.
- A user-space virtio-mmio block store that reads and writes a real emulated
  disk. A value written in one boot is read back in the next.
- A filesystem server, a console driver, and a supervisor, all ordinary
  processes.

All four configurations (two architectures by two tiers) pass `make check`. See
`docs/CHANGELOG.md` for what has shipped and `docs/ROADMAP.md` for the plan.

Footprints are asserted, because a count is a property of this kernel.
Durations are reported and not asserted, because every one was taken under an
emulator. Nothing has run on hardware, so every duration here has the right
shape and the wrong magnitude.

## Design aims

Two long-term aims constrain the code without being deliverables, both stated
in `docs/design.md`.

The first is a system that can be read end to end, kernel and userland, in the
tradition of the Lions commentary and xv6. That is why functions are small, why
the whole system matters and not the kernel alone, and why ideas that do not
fit are recorded in `docs/PROPOSALS.md` rather than built.

The second is per-process environment isolation, in the spirit of Qubes OS but
at a fraction of the weight. It adds nothing new to the design: no ambient
authority, no global filesystem root, a memory contract that transfers rather
than shares, explicit protection domains, and a build-time-fixed set of
processes are all present for other reasons and together are most of what that
property means. There is no hypervisor and no promise of one. Nyctos isolates
processes, not machines.

Neither aim is being written up, and neither is scheduled. The system has to
work first.

## Documentation

| File | What it holds |
| --- | --- |
| `docs/design.md` | The intended system, in detail. Builds to `nyctos.pdf`. |
| `docs/ROADMAP.md` | The plan, what each item answered, and what would change it. |
| `docs/BACKLOG.md` | Work considered but not scheduled. |
| `docs/CHANGELOG.md` | What has shipped, per minor version. |
| `docs/PROPOSALS.md` | Deliberately not built, with the reasoning kept. |
| `docs/CODING_STYLE.md` | C style: a FreeBSD style(9) variant, 4-space indent, no tabs. |
| `docs/TODO.md` | Notes on the document build toolchain. |

## Requirements

The build runs on Linux, or on macOS with [Homebrew](https://brew.sh). Windows
with Chocolatey is a plausible third host and is not supported yet.

Four things are needed on any host:

- **Clang/LLVM**, which cross-compiles and assembles for `aarch64-none-elf` and
  `riscv64-none-elf` without a separate GCC cross-toolchain per architecture.
- **LLD** (`ld.lld`), the linker used for both targets.
- **QEMU** (`qemu-system-aarch64`, `qemu-system-riscv64`).
- **GNU Make**.

[share/mk/nyc.own.mk](share/mk/nyc.own.mk) locates the toolchain itself. If
`brew` is present it asks Homebrew where its keg-only LLVM and LLD live;
otherwise it takes `clang`, `llvm-ar`, `llvm-objcopy` and `ld.lld` from `PATH`.
No manual `PATH` or shell profile changes are required on either host.

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

For debugging, `gdb-multiarch` (Debian/Ubuntu) or a distribution `gdb` built
with multiarch support understands both targets and can attach to QEMU's
gdbstub.

## macOS setup

1. Install Homebrew, if you don't already have it:

   ```sh
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   ```

2. Install the Xcode Command Line Tools, which provide GNU Make and other basic
   build tools:

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

5. Optional, for debugging: install a target-aware GDB for each architecture:

   ```sh
   brew install aarch64-elf-gdb riscv64-elf-gdb
   ```

   These understand the target's registers and calling convention, unlike the
   host's own `lldb`/`gdb`, and can attach to QEMU's gdbstub
   (`qemu-system-aarch64 -s -S`, `qemu-system-riscv64 -s -S`) to step through
   boot code instruction by instruction.

Homebrew keeps LLVM and LLD keg-only (not linked onto `PATH`) because they
would shadow Apple's own clang, which is why the build asks `brew` where they
are rather than trusting `PATH`.

## Project layout

The tree follows a BSD source layout: `sys` is the kernel, `lib` is the
userland libraries, `bin` is the userland programs, `include` is the headers
those programs compile against, and `share/mk` holds the make fragments that tie
it together.

`bin` holds around forty programs: the shell and its utilities, the console
driver, the filesystem server and the block store beneath it, a supervisor, and
programs written to break a rule, block, or fault on purpose for the checks.
`lib` builds the C library they link against.

```
.
├── Makefile                 # top level: recurses into lib, bin, tools, sys
├── LICENSES/                # Apache-2.0 and 0BSD license texts
├── _out/                    # generated: <target-triplet>/ mirroring the tree
├── docs/                    # design, roadmap, proposals, coding style
├── share/
│   ├── mk/                  # toolchain config and shared build rules
│   └── ld/                  # userland linker scripts, one per arch
├── include/                 # userland headers
├── lib/
│   ├── csu/<arch>/crt1.S    # C runtime startup
│   └── libc/                # the C library
│       ├── path.c           # resolving a path against a namespace
│       ├── file.c           # opening one by name
│       └── compose.c        # what to hand a child
└── sys/                     # the kernel
    ├── sys/
    │   ├── config.h         # build-time pool sizes and the tier switch
    │   ├── pool.h           # static pools and the identifier layout
    │   ├── object.h         # the kernel object types and their pools
    │   ├── vm.h             # address spaces, and what a port provides
    │   ├── grant.h          # authority over a region, held as a handle
    │   ├── cohort.h         # a protection domain shared by several images
    │   └── systm.h          # kernel-internal prototypes
    ├── kern/                # machine-independent
    │   ├── init_main.c      # kernel_main and the boot checks
    │   ├── kern_object.c    # the pool instances
    │   ├── kern_syscall.c   # the trap path and what a fault means
    │   ├── kern_channel.c   # send, receive, reply, and the move contract
    │   ├── kern_proc.c      # processes, spawn, the loader, and reaping
    │   ├── kern_pager.c     # a page fault, sent as a message
    │   ├── kern_cohort.c    # membership, and tearing a cohort down
    │   ├── subr_page.c      # the frame allocator
    │   ├── subr_vm.c        # the kernel's own address space
    │   ├── subr_pool.c      # the pool mechanism
    │   └── subr_prf.c       # console output, formatting, panic
    └── arch/
        ├── aarch64/
        │   ├── boot.S           # reset entry, EL2->EL1 drop, .bss, stack
        │   ├── exception.S      # EL1 vector table, SVC and fault paths
        │   ├── pmap.c           # page tables, 39-bit VA, 4KB granule
        │   ├── fault_md.c       # every exception, mapped to a fault kind
        │   ├── mem_md.c         # what memory and devices this board has
        │   ├── halt.c           # PSCI power off and reset
        │   ├── machdep.S        # drop to EL0
        │   ├── uart.c           # PL011 UART (QEMU virt @ 0x09000000)
        │   └── conf/kern.ld     # places the kernel at 0x40080000
        └── riscv64/
            ├── boot.S           # reset entry, M-mode handover to S-mode
            ├── exception.S      # S-mode trap entry, ecall and fault paths
            ├── pmap.c           # page tables, Sv39
            ├── fault_md.c       # every exception, mapped to a fault kind
            ├── mem_md.c         # what memory and devices this board has
            ├── halt.c           # SiFive test finisher
            ├── machdep.S        # drop to U-mode
            ├── uart.c           # 16550A UART (QEMU virt @ 0x10000000)
            └── conf/kern.ld     # places the kernel at 0x80000000
```

## Building

`ARCH` selects the target architecture and defaults to `aarch64`. `TIER`
selects the protection tier and defaults to `0`, flat memory with a trusted
contract; `TIER=2` is the same source with an MMU behind it. Each `make`
invocation builds one configuration, so building all four is four commands:

```sh
make                       # aarch64, Tier 0 (the defaults)
make ARCH=riscv64          # the other architecture
make TIER=2                # the other tier
make ARCH=riscv64 TIER=2   # both
```

Outputs land in `_out/<target-triplet>-tier<N>/`, keyed by the clang target
triplet and the tier so the four configurations never collide. The tree below
it mirrors the source tree, with `lib/libc.a` and `lib/crt1.o` staged where
userland programs link against them and the final image at `sys/kernel.elf`.
Header dependencies are tracked with `-MMD -MP` and picked up on the next build,
so editing a shared header rebuilds every file that includes it.

```sh
make clean                 # removes this configuration's output directory only
make ARCH=riscv64 clean    # removes riscv64 Tier 0's output only
make cleandir              # removes _out/ entirely
```

`make clean` only ever removes the current configuration's output directory. It
never touches another architecture's or another tier's.

## Running under QEMU

```sh
make run                       # aarch64, Tier 0 (the defaults)
make ARCH=riscv64 TIER=2 run   # the other of each
```

Both run QEMU with `-nographic`, so console output appears in your terminal. The
boot checks run, then the shell:

```
$ echo one two three
one two three
$ cat motd
served from outside the kernel
$ exit
```

`exit` leaves the shell, after which the kernel powers the machine off and QEMU
exits. A panic does the same, having printed why. On riscv64 the exit status
distinguishes them, because the SiFive test finisher carries a code; PSCI
`SYSTEM_OFF` on aarch64 does not, so there the console is the only signal.

Two boot checks want input before the shell does: the console service reads two
lines and `greet` reads one, so a session starts by typing three lines of
anything.

## Running the recorded session

```sh
make check                       # aarch64, Tier 0 (the defaults)
make ARCH=riscv64 check          # the other architecture
make TIER=2 check                # the other tier
make ARCH=riscv64 TIER=2 check   # both
```

`make check` feeds [share/check/session.txt](share/check/session.txt) to the
machine twice and takes about half a minute. Once paced at two seconds a line,
which is how a person types, with the transcript from the shell's banner to its
exit compared against [share/check/expected.txt](share/check/expected.txt).
Then once as a burst, which is how a pipe delivers a whole session at once,
where the check is that the machine reaches the end without a panic.

Both run because they catch different things. A console that lets a completed
line block whatever is typed behind it passes the paced session and hangs the
burst one; a receive buffer too small for a burst fails both.

One session file serves every configuration. The resource report after the
session is one file per tier, because Tier 2 runs two processes where Tier 0
refuses the second, and an exact count of what a run used is worth more than a
line both tiers share.

## Memory map notes

| Arch | Kernel load address | Why | UART |
| --- | --- | --- | --- |
| aarch64 | `0x40080000` | QEMU `virt` RAM starts at `0x40000000`; the `-kernel` loader places ELF images at `+0x80000` by default | PL011 @ `0x09000000` |
| riscv64 | `0x80000000` | QEMU `virt` RAM base; with `-bios none` the reset vector jumps straight here | 16550A @ `0x10000000` |

`USER_LOAD_ADDR` in [share/mk/nyc.own.mk](share/mk/nyc.own.mk) must agree with
the userland linker script in `share/ld/`. Every program also needs its own slot
above that base, assigned by hand in the program's own Makefile, because at Tier
0 there is no MMU and a second copy of an image would load over the first.

At Tier 2 the slot is a virtual address and the memory behind it is frames
nobody else has, so the rule that one image has one live process is compiled
out. The slots stay because the loader still uses them to place a program's
memory, and because one source builds both tiers.

## Coding style

All C and assembly follows [docs/CODING_STYLE.md](docs/CODING_STYLE.md), a
variant of the FreeBSD kernel style(9) guide with 4-space indentation, 2-space
continuation lines, and no hard tabs.

## Licensing

This operating system is dual-licensed under either of:

* Apache License, Version 2.0 ([LICENSE-APACHE](LICENSES/Apache-2.0.txt))
* BSD Zero Clause License ([LICENSE-0BSD](LICENSES/0BSD.txt))

You may choose which license to follow when using, modifying, or distributing
this software. All inbound contributions must be submitted under this
dual-license structure.

Every source file carries an `SPDX-License-Identifier` line and a short
copyright comment near the top, per
[docs/CODING_STYLE.md](docs/CODING_STYLE.md#licensing).

## Known limitations

- One `make` invocation builds one architecture and one tier. Building all four
  is four commands.
- A spawn that fails partway leaks the frames it had allocated. Nothing records
  what a spawn allocated at the point it failed, and every failure there is a
  panic today.
- A cohort restart loses the previous member's memory. A place becomes free when
  its occupant exits rather than when it is reaped, so the replacement loads over
  the old member's mappings and those frames become unreachable. The boot's
  frame ledger counts them.
- There is no cross-core TLB maintenance on riscv64, whose fences reach only the
  issuing core. Reclaiming a cohort member's memory while a peer runs on another
  core is not safe there, and the checks only reap processes in no cohort.
  aarch64, whose fences are inner-shareable, does not have this limit.
- There is no calendar time. A program makes its own deadlines from the counter
  and the system page, but the page carries no UTC offset, so a program knows how
  long it has waited and not what day it is.
- At Tier 0 a program can write the shared system page, because there is one
  frame and flat memory has nothing to refuse the store. Only the MMU prevents
  it.
- A process that creates a thread and keeps the handle can never be reaped: the
  handle references the thread, the thread references its process, and the
  process closes its handle table only when reaped. Closing the handle lets it
  be reaped normally.
- No server here receives on one thread and replies from a pool of workers,
  though the kernel can name several replies a thread owes at once. The most any
  thread owes in a boot is two, which the console does.
- Address space identifiers are assigned on both ports, but buy nothing
  measurable under QEMU, which discards every translation when the root register
  is written. The kernel is correct and the saving is not visible here.
- Several guarded paths are unexercised by the checks: killing a thread
  interrupted mid-operation, a thread arriving at the kernel already marked for
  death, and a first dispatch that finds the kernel lock held. Each guard is
  present because the contract requires it, and removing it fails no check.

## Next steps

See [docs/ROADMAP.md](docs/ROADMAP.md) for the plan and
[docs/CHANGELOG.md](docs/CHANGELOG.md) for what has shipped. Work is chosen from
the roadmap and backlog as each piece of work ends, out of what it left open: a
question half answered, or a limitation this file still carries.
