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
half that is. Four figures have been obtained and all four are
footprints rather than speeds; nothing has been measured for time. The
gap between what is described and what runs is deliberate rather than
hidden, and it narrows one stage at a time.

## Status

**Milestone 6 is done: there are driver cohorts, and measuring them says
the mechanism is aimed at the wrong cost.** A cohort is a set of images
the build names, running in one address space. The storage stack is one:
a filesystem reading its files through a block store underneath it. A
member may hand a peer an address instead of a buffer for the kernel to
map, and a member that faults ends the whole cohort.

The same programs and the same recorded session, one line of a
build-time table apart:

| | out of the cohort | in it |
|---|---|---|
| pages remapped | 42 | 12 |
| page-table entries written | 84 | 24 |
| single-address invalidations | 42 | 12 |
| address space switches | 395 | 358 |

It also removes a copy per whole block, which is not in that table and
is the only thing a cohort uniquely does. The rest is bought more
cheaply with address space identifiers, which neither port has and which
would cost no isolation. So the milestone kept section 18 and marked it
rather than endorsing it, and wrote down what would settle it.

The cost is not a number:

```
$ peer_c
cohort peers: member peer_c faulted, instruction at 0x0, pc 0x81800042
  the whole cohort is the recovery unit, per section 18.2: peer_a peer_b peer_c
  tearing them down together and restarting them is M7, and is not built

panic: cohort peers cannot be recovered and must not be left running
```

The same program outside a cohort is `sh: peer_c: instruction fault` and
the shell carries on. A bug in the block driver takes down the
filesystem.

**Milestone 5 is done: there are paths, and no absolute ones.** A process
receives its namespace at launch as a set of directory handles with names
attached, and resolves a path against it in the C library. Nothing asks
the kernel: resolution is string handling over a table the process was
given, so a name matching no entry reaches nothing at all, and there is
no root anywhere that everyone has.

```
$ namespace
namespace: / is id 1
namespace: /files is id 4096
namespace: /u is id 4097
namespace: /u is id 4098
$ cat /files/motd
served from outside the kernel
$ only / namespace
    namespace exited with status 1
$ alone namespace
    namespace exited with status 0
```

`/` and `/files` are one filesystem under two names; `/u` is two
directories under one name, tried in order, which is a union. What a
child is launched with is a choice the shell makes out loud: everything
it holds, a named subset, or nothing at all.

**Milestone 4 is what that rests on: two protection tiers, from one
source.** Four configurations, two architectures by two tiers, and the
same recorded shell session runs against all four.

```
$ touch_addr 40080000                         (Tier 0)
touch_addr: 0xd53800a0 at 0x40080000
[-717750112]

$ touch_addr 40080000                         (Tier 2)
sh: touch_addr: access fault at 0x40080000, pc 0x40f000f4
[-16]
```

One program, one command line, two tiers: at Tier 0 it reads the kernel's
first instruction and prints it, and at Tier 2 it dies at the address it
touched. Nothing in the interface differs between them. Enforcing the
memory contract of section 6.1 cost three lines in the rendezvous, and no
operation, argument structure or error code exists at one tier and not
the other.

At Tier 2 every process has an address space of its own, so two live
copies of one image are two processes that cannot reach each other. A
message buffer is moved rather than shared: its pages leave the sender as
they enter the receiver and go back at the reply, so a program that
touches its own buffer during a call faults on it. A driver holds a grant
over its device registers rather than an address compiled into it. A
fault is an exit with a reason, with the kind, the address and the
instruction in the dead thread's status, which the shell reports. A page
fault is the exception: it is a message the faulting thread sends to its
pager, and priority inheritance across it was never implemented, it
simply happens, because a faulting thread is an ordinary blocked sender.

Underneath all of it: static object pools, threads and the context
switch, a strict-priority scheduler with priority inheritance,
per-process handle tables with rights, the operation entry point,
timelines, interrupts on a controller per board, timers with absolute
deadlines, programs loaded from a boot archive into user mode, exit and
join over completion timelines, spawn and thread creation as operations a
program can call, page tables and TLB maintenance in each port, and a C
library whose input and output reach a connection the process was handed
rather than a hole in the capability model.

`make check` feeds a recorded session to the machine twice, once paced
like a person typing and once as a burst like a pipe, and compares the
transcript against a recorded one.

What does not exist: multicore, the submission engine, and everything to
do with death beyond reaping, cancellation, killing and restart. What
is missing is the cohort teardown of section 18.2: a member that faults
still halts the machine. No address space is ever torn down and a
rendezvous carries no deadline, so a wedged server hangs its client
until a supervisor ends it, and nothing yet notices on its own that one
should.

The figures obtained so far are footprints rather than speeds, with one
exception. Kernel object memory is 232192 bytes, of which 143872 is
thread stacks. Those pools were sized by the boot check sequence for six
milestones, because nothing was reaped and a process that exited kept
its slot for ever; they are sized by what runs at the same time now,
which is thirteen processes and fifteen threads.
Having no wait-set costs this system one thread, the console's second,
at eight and a half kilobytes with both its stacks. An address space
costs a handful of frames per process, because the kernel is mapped into
each one rather than shared between them. And naming costs about two
kilobytes in a program that opens files and nothing at all in one that
does not: `status` is 3837 bytes with and without the whole of section
20 in the library beside it.

The exception is what a protection boundary costs to cross, which M6
measured because it had to: four page-table entries and two TLB
operations to move one page across and back, at Tier 2, and nothing at
all at Tier 0. Entries are per page and TLB operations are per half of
the move. That is a count of operations rather than a time, so it is
exact on an idle machine and does not become truer under load.

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

`bin` holds twenty-three programs: the shell, its utilities, the console
driver, the filesystem server and the block store under it, and the ones
written to break a rule on purpose or to die on purpose. `lib` builds
the C library they link against.

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
│       ├── path.c            # resolving a path against a namespace
│       ├── file.c            # opening one by name
│       └── compose.c         # what to hand a child
└── sys/                     # the kernel
    ├── sys/
    │   ├── config.h         # build-time pool sizes and the tier switch
    │   ├── pool.h           # static pools and the 20/44 identifier layout
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
    │   ├── kern_proc.c      # processes, spawn, and the loader
    │   ├── kern_pager.c     # a page fault, sent as a message
    │   ├── kern_cohort.c    # the build-time membership table, and the halt
    │   ├── subr_page.c      # the frame allocator
    │   ├── subr_vm.c        # the kernel's own address space, and what a
    │   │                   #   boundary crossing costs
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
selects the protection tier and defaults to `0`: flat memory, where the
memory contract is trusted. `TIER=2` is the same source with an MMU
behind it. Each `make` invocation builds exactly one configuration, so
building all four is four commands, not one:

```sh
make                       # aarch64, Tier 0 (the defaults)
make ARCH=riscv64          # the other architecture
make TIER=2                # the other tier
make ARCH=riscv64 TIER=2   # both
```

Outputs land in `_out/<target-triplet>-tier<N>/`, keyed by the clang
target triplet and the tier so that the four configurations' object
files, `.d` dependency files, libraries and images never collide.
The tree below it mirrors the
source tree, with `lib/libc.a` and `lib/crt1.o` staged where userland
programs link against them, and the final image at `sys/kernel.elf`. Header
dependencies are tracked automatically (via `-MMD -MP`) and picked up on the
next build, so editing a shared header like
[sys/sys/op.h](sys/sys/op.h) correctly rebuilds every `.c`/`.S`
file that includes it.

```sh
make clean                 # removes _out/aarch64-none-elf-tier0/ only
make ARCH=riscv64 clean    # removes _out/riscv64-none-elf-tier0/ only
make cleandir              # removes _out/ entirely
```

`make clean` only ever removes the current configuration's output
directory; it never touches another architecture's or another tier's.

## Running under QEMU

```sh
make run                       # aarch64, Tier 0 (the defaults)
make ARCH=riscv64 TIER=2 run   # the other of each
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
make check                       # aarch64, Tier 0 (the defaults)
make ARCH=riscv64 check          # the other architecture
make TIER=2 check                # the other tier
make ARCH=riscv64 TIER=2 check   # both
```

`make check` feeds [share/check/session.txt](share/check/session.txt) to
the machine twice and takes about half a minute. Once paced at two seconds
a line, which is how a person types, and the transcript from the shell's
banner to the shell's exit is compared against
[share/check/expected.txt](share/check/expected.txt). Then once as a
burst, which is how a pipe delivers a whole session before anything has
asked for any of it, where what is checked is that the machine reaches the
end without a panic.

Both are run because they catch different things. A console that lets a
completed line block whatever is typed behind it passes the paced session
and hangs the burst one; a receive buffer too small for a burst fails
both.

The session file is one file for every configuration, which is the
point of it. The resource report after the session is one file per
tier, because Tier 2 runs two processes where Tier 0 refuses the
second and an exact count of what a run used is worth more than a
line both tiers can share.

## Memory map notes

| Arch | Kernel load address | Why | UART |
| --- | --- | --- | --- |
| aarch64 | `0x40080000` | QEMU `virt` RAM starts at `0x40000000`; the `-kernel` loader places ELF images at `+0x80000` by default | PL011 @ `0x09000000` |
| riscv64 | `0x80000000` | QEMU `virt` RAM base; with `-bios none` the reset vector jumps straight here | 16550A @ `0x10000000` |

`USER_LOAD_ADDR` in [share/mk/nyc.own.mk](share/mk/nyc.own.mk) must agree
with the userland linker script in `share/ld/`. Every program also needs
its own slot above that base, assigned by hand in the program's own
Makefile, because at Tier 0 there is no MMU and a second copy of an image
would be loaded over the first.

At Tier 2 the slot is a virtual address and the memory behind it is
frames nobody else has, so the rule that one image has one live process
is compiled out. The slots stay because the loader still uses them to
decide where a program's memory is, and because one source has to build
both tiers.

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

- One `make` invocation builds exactly one architecture and one tier
  (selected via `ARCH` and `TIER`). This keeps the build system's
  variables simple and unambiguous instead of juggling per-configuration
  state in a single run; building all four is four commands.
- Memory is never reclaimed. Pool slots come back, but the megabyte a
  process was loaded into does not, nor do its page tables, and a spawn
  that fails partway leaks the frames it had allocated. Freeing them
  needs the loader to record which pages it allocated, because a
  demand-paged process runs on frames belonging to its pager.
- A rendezvous carries no deadline, so a server that stops answering
  hangs its client until something cancels it. Cancellation exists;
  nothing yet notices on its own that it is needed.
- A cohort cannot be recovered. Section 18.2 makes the cohort the unit of
  recovery, and tearing one down and restarting it is milestone 7, so a
  member that faults halts the machine rather than being restarted with
  its peers. The halt says what is missing.
- Neither port assigns address space identifiers, so every dispatch into
  a different address space discards the whole translation cache. That is
  the largest cost the milestone 6 measurements turned up and the one
  that would most change what a cohort is worth.

## Next steps

See [docs/ROADMAP.md](docs/ROADMAP.md). The next milestone is death:
reclamation, cancellation, deadlines, and the cohort teardown that
milestone 6 stubbed and named. It is the one thing every milestone so
far has deferred, and three of them in a row have paid for it by raising
a pool size instead.

The ordering that got here is in the same file, along with what each
milestone answered and what it left untested. The one choice most likely
to look wrong is timelines before message passing, and it is not:
interrupts and timers depend on timelines and not on message passing, so
building them first got the system responding to hardware several stages
earlier.
