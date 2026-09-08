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

**Milestone 15 is done: a client that sends to a server which stops
answering gets its thread back.** Until now a rendezvous carried no
deadline, so a wedged server hung its client for ever. Cancellation
existed; something outside had to notice and use it, and nothing
noticed.

```
                                    sends that gave up at a deadline
  queued, no server had seen it                                    1
  a server was holding the message                                 2
```

The same in all four configurations, and counted apart because the two
cases differ in what the server learns. A client removed from the queue
leaves nothing behind: no message was delivered and nothing was
allocated on its behalf. A client whose message a server holds gets its
lent buffer back with the permissions it had and its donated priority
back from the server, and the server discovers the outcome when its
reply is refused with `SYS_ERR_STALE`.

The design had specified all of this and the kernel had never done it.
What the building added beyond the specification was mostly about when
things happen rather than what: the expiry does only the pointer work in
the timer, because undonating and taking a buffer back charge their work
to a running thread and an interrupt may not have one, and a deadline
that arrives after the wait has already ended does nothing at all.

**Since then, in milestone 16, a program makes its own.** An absolute
deadline needs a clock. The system page of section 6.2 exists now, one
frame mapped read-only into every process and found through the startup
record, and it carries the frequency; the time itself comes from the
counter, which each port permits user mode to read with one instruction.
A program that reads the clock and exits enters the kernel once, to
exit.

**Milestone 14 is done: a program is mapped as what it is.** The boot
archive carried the size of an image and not the size of the memory a
program needs, so the loader mapped a whole megabyte slot as one region
that was readable, writable and executable, and hoped.

```
                                    t0 riscv  t0 aarch  t2 riscv  t2 aarch
what a process costs, in frames            0         0        15        14
  of which its image needs                 0         0         7         8
  and its address space                    0         0         8         6
what it cost before milestone 14           0         0       264       262
```

Carrying two offsets instead, where writable memory begins and where the
program's memory ends, bought three separate things.

**Memory.** A process costs what its image needs plus the space it runs
in. What the machine holds when the boot ends fell from 10732 frames to
8519, and the ten processes still running account for 119 of those where
they used to account for 2352.

**Protection.** One region means one set of permissions, and the only
set that lets a program run is all of them, so every program's text was
writable and every program's stack was executable. Now text is read and
executed and never written, and everything else is read and written and
never executed. Two programs had to be written to prove it: nothing in
the archive could write to an address it was handed or jump anywhere the
linker had not sent it.

**And the eager copy of writable data**, which section 15.1 has always
specified and which had never run. The loader left every page with bytes
in it to the pager, and a page supplied to a fault is mapped read-only,
so a paged program with initialised data would have faulted writing it.
No program had any, so nothing found out.

The boundaries come from the link, which was already creating them: the
linker script page-aligns the start of writable memory so that no page
is both text and data. The build records them beside each binary and the
archive tool reads them from there, so the numbers cannot disagree with
the image they describe.

**Milestone 13 is done: a process gives back what it was given.** Until
now nothing ever freed a frame, so how long this system could run was
bounded by how many processes it had ever started.

```
                                    t0 riscv  t0 aarch  t2 riscv  t2 aarch
what a process costs, in frames            0         0       264       262
how many come back                         0         0       264       262
what 8 of them leave behind                0         0         0         0
```

Those were the figures milestone 13 measured, and the reason for them
is what milestone 14 went on to remove: a process cost a whole megabyte
slot for an image of four kilobytes. At Tier 0 it costs nothing, then
and now: a flat image runs in memory the archive already occupies, so
there is nothing to give back and every frame this reclaims is a Tier 2
frame.

**A program was spawned and reaped 40000 times on a machine with 32768
frames, and the count came back to where it started.** That asked the
allocator for more than ten million frames, three hundred times what
the machine has. Before this the same loop stopped at about the
ninety-fifth iteration with no memory left.

The boot ends with a ledger of every frame the machine has spent, and
it balances to zero: what was spent before anything ran, what live
processes hold, what cohort address spaces hold, what grants hold, what
a cohort restart orphaned, what the kernel's own address space has
grown by, and nothing unaccounted. A ledger is weaker than "nothing
leaks", since most of those rows are frames nothing will ever return.
It is stronger than a total, because it fails when a new way of holding
a frame appears and nobody writes it down.

One row of it is a real loss. A cohort place becomes free when its
occupant exits rather than when it is reaped, so a restarted member is
loaded over the previous one's mappings and its frames become
unreachable. That is a consequence of how cohort restart is specified,
and the ledger reports it rather than hiding it. It was 512 frames
across a boot when a member was mapped a slot, and is 14 now that one is
mapped an image.

**Milestone 12 is done: the latency floor is the rendezvous, not the
switch.** It attempted the first remedy milestone 9 named, not
disabling interrupts across a context switch, and made three splits
that each lowered the region a little and none by more than the spread
between runs.

```
                              t0 riscv  t0 aarch  t2 riscv  t2 aarch
longest across a switch,
  a kernel thread                  449       559       465       530
longest with a program current      83       115        60        70
```

Microseconds, medians of five runs. **The second row is the one that
matters and this project had never measured it.** The longest region
that spans a switch is opened by one of the kernel's own threads in
every run, doing message-passing work in the checks; what a program is
delayed by is four to eight times less. Every latency figure published
here since milestone 9 was the first row.

Inside that region the switch itself is nought to three per cent. The
rest is the work that has to be atomic with it, so lowering the floor
further means making a rendezvous shorter or making less of it atomic,
and making the switch interruptible would buy the three per cent.

**Milestone 11 is done: an operation can take its target from a
register an earlier operation wrote.** The dependency that used to live
in a local variable between two calls lives in the batch instead, so a
client that walks to a file, reads it, closes it and gives the handle
back makes one submission where the plain form makes four.

```
fs_client  5 submissions, 15 checks, 5 rendezvous, 1 connections
fs_batch   4 submissions, 12 checks, 3 rendezvous, 0 connections
fs_chain   2 submissions, 12 checks, 5 rendezvous, 1 connections
```

Read across rather than down. `fs_chain` and `fs_client` agree on every
column but the first, so substitution changed the shape of the call and
not the work; the three checks it saves are the three the removed calls
would each have paid. `fs_batch` asks the server one compound question
instead of three, and saves two round trips and the connection.
**For this workload, changing the protocol beat changing the kernel.**

A reply now carries a status the kernel can read, so a request a server
refused is no longer a successful send, and a chain aborts on it rather
than reading from a connection that was never returned. A batch that
aborts force-signals the timelines it was going to signal, so a waiter
is told rather than left to a timeout.

The three issuing opcodes still refuse. Starting work needs a device
that transfers and this tree has none, and the one route that looks
open is an issuing send, which the design rules out on structural
grounds. What the tree does reach is the other half: several
completions outstanding at once, waited for in one submission, by one
thread with no worker pool.

**Milestone 10 is done.** One
system call can carry up to sixteen operations, checked before any of
them runs, each reporting its own status. Linked entries abort as a
group. A batch can be loaded once and executed many times without being
checked again.

```
8 signals, one call each: 8 crossings, 24 checks
the same 8 in one call:   1 crossings, 17 checks
it saves 7 crossings and 7 checks
```

The saving is the fixed cost of a call multiplied by one fewer than the
number of entries, and nothing per entry: every entry is checked before
it runs and again when it runs whichever form carried it.

What it does not answer is the question it was built for. The engine
exists for chains, where one operation names a handle an earlier one
produced, and an entry's target is fixed when the batch is submitted.
Nothing in the system used a batch: a run made about 750 submissions
and every one carried a single entry. Section 17 stayed, with its
claims cut back to what was measured and the deciding experiment
written down. Milestone 11 above is that experiment, and one client
uses a batch now.

It also priced the kernel lock that milestone 8 introduced. Section
13.3 says the batch bound does not appear in interrupt latency. It does
not on one core with no lock, and it does on two, where one lock is
held across the whole call and sixteen entries is one
interrupts-disabled region sixteen entries long.

**Milestone 9 is done: there are numbers, and each one says whether it
is about this kernel or about the machine it was taken on.** Both terms
of section 13.3 are measured on both ports and both tiers, along with
the IPC round trip and the cost of a donation.

The rule the milestone is built on is that a count is a property of this
kernel and belongs in the compared file, and a duration is a property of
the host and belongs above it where it can move. Nothing has ever run on
hardware, so every duration here has the right shape and the wrong
magnitude.

```
interrupts off: the longest region, per section 13.3
    core 0: 20457 regions, longest 6028 ticks (about 602 us),
        mean 54 ticks, begun at 0x80008f1e
        ended at 0x80008abc
interrupt to a program: section 13.3's second term
    core 0: 16 dispatched, longest 1680 ticks (about 168 us),
        0 points passed
```

The longest interrupts-disabled region begins in a reply and ends in the
next send, so it spans a context switch, and it contains no preemption
point. Section 13.3 said Tier 0's latency was bounded by where the
preemption points are; it is bounded by the scheduler, and the section
is rewritten to say so.

Crossing a core boundary costs about three times a donation that stays
on one core, which both ports agree on independently. That is the same
boundary milestone 8 priced at four inter-processor interrupts, in time.

`SYS_ERR_DEADLOCK` is reachable for the first time: a cycle of servers
is refused rather than wedging the machine silently.

**Milestone 8 is done: it runs on two cores, and where things run is
something you say rather than something it decides.** Each core has its
own ready queue and takes no lock another core could be holding to
decide what to run next. A thread runs on the core it was put on and
nothing moves it afterwards, because nothing balances and nothing
steals. A client on one core can raise the priority of a server on the
other, and the poke that tells that core to look is the whole of what
crosses.

This is Tier 2 only. Tier 0 keeps one core and no internal locking at
all, which is what buys it a latency bound you can inspect by reading
the preemption points.

The storage stack runs split across both cores, and what that costs is
a measurement rather than a claim:

```
placement: the storage stack across two cores
    on one core it cost                    0 pokes, 0 donations
    with the store on the other core       4 pokes, 2 donations
```

Four inter-processor interrupts for one read, per boundary crossed. The
recorded session is identical in all four configurations, and on two
cores it is produced with the stack still split: the shell reads its
files across a core boundary and the transcript does not know.

What it did not answer is whether the absence of automatic placement
would be intolerable. There was nothing here for a balancer to have done
better, because the stack has one obvious placement and it was placed by
hand once. That needs a workload whose right placement changes while it
runs, and there is not one yet.

**Milestone 7 is done: things can end, and something else finds out.** A
slot goes back to its pool when its object has finished and nothing can
ask about it any more. A thread can be broken out of any wait it can be
in. A process can be ended, and the two sides of a rendezvous it was
part of fail differently, because they are waiting on different things:
a server is at a receive point and collects a notice, a client is not
and is failed with an error. Nothing in the system uses a deadline to
discover a death.

The pools stopped tracking how long the boot sequence is, which is what
they had tracked for six milestones:

| | before | after |
|---|---|---|
| processes at once | 46 of 56 | 13 of 24 |
| threads at once | 65 of 96 | 16 of 32 |
| kernel object memory | 606976 | 232640 |

And a cohort recovers, which is what made this milestone sit where it
does. A member faults, every member goes with it including the ones that
did nothing wrong, and a supervisor starts them again on the same
channel:

```
cohort peers: member peer_c faulted, instruction at 0x0, pc 0x81800042
    peer_a goes with it
    peer_c goes with it
    the peer that did not fault, ended     with its cohort
    and the pair works again, through new processes
```

The restart policy is a program, because the kernel holds none. Given a
child that dies immediately every time it makes three starts and says
so, rather than relaunching for ever and turning a configuration mistake
into a system that looks busy.

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

The reference counts that decide when an object is freed were plain
increments, unsafe on two cores, and three milestones listed them
without fixing them. They are fixed: the count and the decision to free
are taken together, and a check that hammers one from both cores finds
it back where it started, where the old code left it at 230 instead of
1.

What does not exist: a device that does direct memory access, and with
it the half of section 17 about transfers in flight. The reference
counts that decide when an object is freed are still
plain increments, which two cores make unsafe and milestone 8 listed
without fixing.

The figures obtained so far are footprints rather than speeds, with one
exception. Kernel object memory is 232640 bytes, of which 143872 is
thread stacks. Those pools were sized by the boot check sequence for six
milestones, because nothing was reaped and a process that exited kept
its slot for ever; they are sized by what runs at the same time now,
which is thirteen processes and sixteen threads.

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

`bin` holds twenty-seven programs: the shell, its utilities, the console
driver, the filesystem server and the block store under it, a
supervisor, and the ones written to break a rule on purpose, to block on
purpose, or to die on purpose. `lib` builds the C library they link
against.

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
    │   ├── kern_proc.c      # processes, spawn, the loader, and reaping
    │   ├── kern_pager.c     # a page fault, sent as a message
    │   ├── kern_cohort.c    # membership, and tearing a cohort down
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
- A spawn that fails partway leaks the frames it had allocated.
  Milestone 13 made a process give back its slot and its page tables
  when it is reaped, and left the partial-spawn path alone: nothing
  records what a spawn had allocated at the point it failed, and every
  failure there is a panic today.
- A cohort restart loses the previous member's memory. A place becomes
  free when its occupant exits rather than when it is reaped, so the
  replacement is loaded over the mappings that named the old member's
  frames and they become unreachable. The boot's frame ledger counts
  them: 14 across a boot, where it was 512 before a member was mapped
  what its image needs.
- No cross-core TLB maintenance. Reclaiming a cohort member's memory
  while a peer runs on another core is not safe, and the checks reap
  processes that are in no cohort.
- No calendar time. A program makes its own deadlines now: it reads the
  counter with one instruction and takes the frequency from the system
  page, neither of them a system call, and libc turns a relative wait
  into the absolute instant a call takes. The page carries no UTC
  offset, so a program knows how long it has waited and not what day it
  is.
- A receive carries no deadline, deliberately: a server waiting for work
  is not waiting for a party that can fail it. What a server does about
  a client that never comes is not answered.
- A thread that dies while holding a client's message leaves that client
  waiting until its own deadline. The death of a whole process releases
  its clients with an error; one thread of a live server dying does not.
- Killing a process unblocks every thread of it and each leaves at its
  next return from the kernel. A thread computing in user mode and
  calling nothing is not stopped, and there is no hook in the trap path
  that would stop it.
- Nothing consumes a death notice. The kernel delivers one to a server
  whose client has gone, and no server here holds state per client, so
  whether a restarted server leaks its own per-connection records is
  still an open question rather than an answered one.
- Neither port assigns address space identifiers, so every dispatch into
  a different address space discards the whole translation cache. That is
  the largest cost the milestone 6 measurements turned up and the one
  that would most change what a cohort is worth.

## Next steps

See [docs/ROADMAP.md](docs/ROADMAP.md). Twelve milestones are finished.
The last two were not in the plan: each came out of the previous one's
retrospective, which named the experiment that would settle a question
it could only half answer. What the plan does not schedule, and what
the last milestones each said would be worth doing next, is written
down there rather than guessed at here.

One thing is named in every retrospective, which is what this project
treats as a signal: nothing has ever run on hardware, so every duration
published here is the right shape and the wrong magnitude. That is why
the milestones that could assert counts rather than durations did.

The other signals have been taken. The latency floor went to milestone
12, which left something narrower and harder: the floor is the work a
rendezvous has to do atomically, and lowering it means making that work
shorter or making less of it atomic. Memory never being reclaimed went
to milestone 13, which left the slot itself as the next question, and
milestone 14 answered it: the archive carries a layout, a process costs
what its image needs, and a slot is an address range rather than an
amount of memory. A rendezvous without a deadline went to milestone 15,
which left a program that cannot make one.

The ordering that got here is in the same file, along with what each
milestone answered and what it left untested. The one choice most likely
to look wrong is timelines before message passing, and it is not:
interrupts and timers depend on timelines and not on message passing, so
building them first got the system responding to hardware several stages
earlier.
