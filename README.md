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

**Roughly half the kernel exists, and there is enough userland to talk
to it.** `docs/design.md` describes the intended system and says which
half that is. Two figures have been obtained and both are footprints
rather than speeds; nothing has been measured for time. The gap between
what is described and what runs is deliberate rather than hidden, and it
narrows one stage at a time.

## Status

**Milestone 3 is done: there is a shell.** It reads a line from a console
driver that is an ordinary process, starts what the line names, waits on
its completion timeline, and reports the status.

```
$ echo one two three
one two three
$ cat motd
served from outside the kernel
$ cat nothing
cat: nothing: no such file
[3]
```

`cat` reads through a filesystem server and prints through a console
service, and both speak the protocol of section 25. That is the first
evidence for the claim that a filesystem here is a protocol rather than a
layer: the console is one.

Underneath it: static object pools, threads and the context switch, a
strict-priority scheduler with priority inheritance, per-process handle
tables with rights, the operation entry point, timelines, interrupts on a
controller per board, timers with absolute deadlines, programs loaded from
a boot archive into user mode, exit and join over completion timelines,
spawn and thread creation as operations a program can call, and a C
library whose input and output reach a connection the process was handed
rather than a hole in the capability model. All of it runs on AArch64 and
RISC-V64 from one source.

`make check` feeds a recorded session to the machine twice, once paced
like a person typing and once as a burst like a pipe, and compares the
transcript against a recorded one. The transcript is identical on both
architectures.

What does not exist: the MMU and everything resting on it, driver cohorts,
multicore, per-process namespaces, the submission engine, and everything
to do with death. Nothing is reaped, nothing is cancelled, and a
rendezvous carries no deadline, so a server that stops answering hangs its
client rather than failing it.

Two figures have been obtained, and both are footprints rather than
speeds. Kernel object memory is 365248 bytes, almost all of it thread
stacks and process handle tables sized by two constants, which is section
8 of the design holding and also saying the defaults are wrong for a small
target. And having no wait-set costs this system one thread, the console's
second, at eight and a half kilobytes with both its stacks; a system with
N devices that must stay responsive while serving pays N of them.

`docs/ROADMAP.md` says what each milestone answered, what it cost, and what
it left untested.

## Why it is shaped this way

Two long-term aims constrain the code without being deliverables. Section
1.4 of `docs/design.md` states both, and the second more carefully.

The first is a system that can be read end to end, kernel and userland
alike, in the tradition of the Lions commentary and xv6. That is why
functions are small, why the whole system matters rather than the kernel
alone, and why ideas that do not fit are recorded in `docs/PROPOSALS.md`
rather than built.

The second is per-process environment isolation, in the spirit of Qubes OS
but at a fraction of the weight. **It adds nothing.** No ambient authority,
no global filesystem root, a memory contract that transfers rather than
shares, explicit protection domains, and a build-time-fixed set of processes
are all already in the design for other reasons, and together they are most
of what that property means. There is no hypervisor here and no promise of
one: Nyctos isolates processes, not machines. If the aim ever seems to need
a mechanism that is not already designed, the aim gets re-examined rather
than the design extended.

Neither is being written up, and neither is scheduled. Nothing here has
earned the right to teach anyone anything yet. It has to work first.

## Documentation

| File | What it holds |
| --- | --- |
| `docs/design.md` | The intended system, in detail. Builds to `nyctos.pdf`. |
| `docs/ROADMAP.md` | Milestones, the plan, and what would change it. |
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

The tree follows the layout of a BSD source tree: `sys` is the kernel, `lib`
is the userland libraries, `bin` is the userland programs, `include` is the
headers those programs compile against, and `share/mk` holds the make
fragments that tie it all together.

`bin` is empty and `lib` builds a C library nothing links against yet. Both
are kept because the stage that spawns the first process needs them, and
letting them rot in the meantime would cost more than building them does.

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
└── sys/                     # the kernel
    ├── sys/
    │   ├── config.h         # build-time pool sizes
    │   ├── pool.h           # static pools and the 20/44 identifier layout
    │   ├── object.h         # the kernel object types and their pools
    │   ├── syscall.h        # the old numbered ABI, awaiting replacement
    │   └── systm.h          # kernel-internal prototypes
    ├── kern/                # machine-independent
    │   ├── init_main.c      # kernel_main
    │   ├── kern_object.c    # the pool instances
    │   ├── kern_syscall.c   # the trap path
    │   ├── subr_mem.c       # memset, memcpy
    │   ├── subr_pool.c      # the pool mechanism
    │   └── subr_prf.c       # console output, formatting, panic
    └── arch/
        ├── aarch64/
        │   ├── boot.S           # reset entry, EL2->EL1 drop, .bss, stack
        │   ├── exception.S      # EL1 vector table and SVC path
        │   ├── halt.c           # PSCI power off and reset
        │   ├── machdep.S        # drop to EL0
        │   ├── uart.c           # PL011 UART (QEMU virt @ 0x09000000)
        │   └── conf/kern.ld     # places the kernel at 0x40080000
        └── riscv64/
            ├── boot.S           # reset entry, .bss, stack, mtvec
            ├── exception.S      # M-mode trap entry and ecall path
            ├── halt.c           # SiFive test finisher
            ├── machdep.S        # PMP setup and drop to U-mode
            ├── uart.c           # 16550A UART (QEMU virt @ 0x10000000)
            └── conf/kern.ld     # places the kernel at 0x80000000
```

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

Both run QEMU with `-nographic`, so console output appears directly in your
terminal. The boot checks run, and then the shell:

```
$ echo one two three
one two three
$ cat motd
served from outside the kernel
$ exit
```

`exit` leaves the shell, after which the kernel powers the machine off and
QEMU exits on its own. A panic does the same, having printed why. On
riscv64 the exit status distinguishes them, because the SiFive test
finisher carries a code; PSCI `SYSTEM_OFF` on aarch64 does not, so there
the console is the only signal.

Two of the boot checks want input before the shell does: the console
service reads two lines and `greet` reads one, so a session starts by
typing three lines of anything.

## Running the recorded session

```sh
make check                  # aarch64
make ARCH=riscv64 check     # riscv64
```

`make check` feeds [share/check/session.txt](share/check/session.txt) to
the machine twice and takes about half a minute. Once paced at two seconds
a line, which is how a person types, and the transcript from the shell's
banner onward is compared against
[share/check/expected.txt](share/check/expected.txt). Then once as a
burst, which is how a pipe delivers a whole session before anything has
asked for any of it, where what is checked is that the machine reaches the
end without a panic.

Both are run because they catch different things. A console that lets a
completed line block whatever is typed behind it passes the paced session
and hangs the burst one; a receive buffer too small for a burst fails
both.

## Memory map notes

| Arch | Kernel load address | Why | UART |
| --- | --- | --- | --- |
| aarch64 | `0x40080000` | QEMU `virt` RAM starts at `0x40000000`; the `-kernel` loader places ELF images at `+0x80000` by default | PL011 @ `0x09000000` |
| riscv64 | `0x80000000` | QEMU `virt` RAM base; with `-bios none` the reset vector jumps straight here | 16550A @ `0x10000000` |

`USER_LOAD_ADDR` in [share/mk/nyc.own.mk](share/mk/nyc.own.mk) must agree
with the userland linker script in `share/ld/`. Every program that can
be live at the same time as another also needs its own slot above that
base, assigned by hand in the program's own Makefile, because there is
no MMU and a second copy of an image would be loaded over the first.
Section 15.2's loader is what removes the restriction.

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
- Nothing runs in user mode. The privilege drop and the trap path exist but
  no process reaches them, so the whole system is the kernel.
- There is no memory protection. Tier 0 is flat memory by design, and the
  MMU arrives in milestone 4.

## Next steps

See [docs/ROADMAP.md](docs/ROADMAP.md). The next milestone is the vertical
slice: one thread, one channel, one driver, one file read, with the driver
as a separate process rather than kernel code.

The bring-up order that gets there is in the same file. It puts timelines
before IPC, which looks wrong and is not: interrupts and timers depend on
timelines and not on message passing, so building them first gets the
system responding to hardware several stages earlier.
