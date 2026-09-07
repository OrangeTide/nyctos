---
title: Nyctos
subtitle: Roadmap
revision: "2"
---

**The floor is the switch (M12).** *Answered: it is not.* The region
that spans a switch is nought to three per cent switch; the rest is the
rendezvous work that has to be atomic with it. Section 13.3's account
of what bounds latency is corrected for the second time, having been
"preemption point placement" until M9 and "the switch" until now. What
would change the plan further is finding that the rendezvous itself can
be made shorter or less of it atomic, which nothing here attempted.

# Roadmap

This document says what exists, what is being built, and in what order.
`design.md` says what the system is meant to be. `PROPOSALS.md` holds ideas
that are not being built.

Milestones 1, 2 and 3 are done. What each settled is recorded in sections
4, 5 and 6 below, and section 4 of `design.md` carries the design decisions
they forced; everything not yet reached by one of them is still a guess.

**There are no dates here.** There is no evidence on which to base one. Each
milestone carries a rough size, and sizes are relative to each other and to
nothing else.

---

## 1. What Exists Today

A cross-compiled kernel that boots under QEMU on aarch64 and riscv64,
initialises its object pools, reports them, and powers the machine off.

- Boot to a known state on both architectures.
- Static object pools with generation-checked identifiers. Stage 1.
- Kernel threads and the context switch, with a guard word below each
  kernel stack. Stage 2.
- A strict-priority scheduler: a bitmap of non-empty levels and a FIFO per
  level, so the dispatch decision is a count-leading-zeros and a head-of-
  list read. Preemption when a higher-priority thread becomes runnable.
  Stage 3.
- Console output with a small formatter, `panic`, and a halt path that ends
  the machine rather than spinning.
- A project-owned libc and C runtime startup object.
- Per-process handle tables with rights, attenuating duplication and
  generation-checked staleness, and the section 23 operation entry point
  with SYS_OP_NOP behind it. Stage 4.
- Timelines: monotonic counters, waiting on a value reached or not yet
  reached, and the error state a timeline enters when its last signal right
  goes. Stage 5.
- Interrupts. A device bound to a timeline on both boards, a handler that
  advances it and returns, and a real idle state the core leaves on an
  interrupt. Stage 6.
- Timers and deadlines. A monotonic clock, one-shot and periodic timers
  with absolute phase, catch-up as a single counter update, and absolute
  deadlines on waits. No tick: the hardware is armed for the earliest
  pending event and for nothing else. Stage 7.
- Message passing: send, receive and reply, a receive queue ordered by
  sender priority, and priority inheritance along a bounded chain.
  Connections, handle transfer by move, process spawn with explicit
  handle inheritance, and a filesystem server. Stage 8.
- Programs, loaded from a boot archive and running in user mode, with
  the startup record of section 20.4 and the trap edge under them.
  Stage 9.
- A shell and the machinery it needs: exit and join over completion
  timelines, spawn and thread creation as operations, an interrupt
  controller on each board, a console driver, and a C library that
  reaches it through a handle. Milestone 3, in eight stages of its own.

**Milestone 1 is done.** A client process, handed one connection at spawn,
walks it to a file, reads the file, and prints it. The file is served by a
different process over the protocol of section 25, and nothing on that path
is inside the kernel except the message passing.

**Milestone 2 is done.** The vertical slice executes a second time with
every participant a loaded program: a boot archive in the kernel's rodata,
images copied to the addresses they were linked for, and `fs_server`,
`fs_client` and `fs_batch` in EL0 on AArch64 and U-mode on RISC-V. Every
operation on that path crossed the trap edge.

The same file read two ways answers section 25.7:

```
fs_client   3 rendezvous, 1 connections
fs_batch    1 rendezvous, 0 connections
```

Batching pays, and the second column is why rather than the first.
Section 25.2 makes the connection pool a build-time bound on every file
open at once, and a compound never draws from it.

Threads carry an explicit wait state and the kernel keeps a ring of
blocking transitions, so the stage checks that the two programs actually
blocked against each other rather than inferring it from the byte counts.
That check found a scheduler defect, since fixed: a server whose base
priority exceeded its client's corrupted the run queues.

Programs can print, over the debug console of section 22.4: a handleless
write to the port's polled UART, gated at build time and again at run
time. It is permanent rather than scaffolding, because a driver on a board
nobody has run this system on has to be debuggable before the console
service it would otherwise report through can exist.

Identifiers are now one machine word rather than a fixed 64 bits, with the
index and generation split sized to the pools: 20 and 44 bits on a 64-bit
build, 10 and 22 on a 32-bit one. Nothing in the tree targets 32 bits, so
what is verified is that the headers compile at both widths and that the
split round-trips.

Kernel object memory went from 6720 bytes to 271424 the moment threads
carried their own stacks, and 266240 of that is stack. That is the property
in section 8 doing its job rather than a problem: the number is a
consequence of `SYS_MAX_THREADS` and `SYS_KSTACK_SIZE` and of nothing a
program can do. It does say the defaults are guesses, and that sixty-four
threads at four kilobytes is not a Tier 0 configuration.

**Milestone 3 is done.** There is a shell. It reads a line from a console
driver that is an ordinary process, starts what the line names, waits on
its completion timeline, and reports the status:

```
$ echo one two three
one two three
$ cat motd
served from outside the kernel
$ cat nothing
cat: nothing: no such file
[3]
```

`cat` reads through the same filesystem server milestone 1 built, and
prints through a console service that speaks the same protocol, which is
the first evidence for section 25's claim that a filesystem is a protocol
rather than a layer: the console is one.

Underneath it: exit advancing a completion timeline and a joiner reading
the status (10.4), spawn and thread creation as operations, an interrupt
controller on each board with the UART routed to it, user mode taking
interrupts for the first time, a console driver with two threads and the
line discipline in it, and a C library whose `read` and `write` reach a
connection the process was handed rather than a hole in the capability
model.

`make check` feeds a recorded session to the machine twice, once paced
like a person typing and once as a burst like a pipe, and compares the
transcript. It runs on both architectures and the transcript is identical
between them, and between tiers: four configurations, one file.

The one figure this milestone owed is what having no wait-set costs. It is
one thread, the console's second, at eight and a half kilobytes with both
its stacks. A system with N devices that must stay responsive while
serving pays N of them, which is linear and, at this size, cheap.

**Milestone 4 is done, and it is the one the design exists to be tested
by.** There is a second protection tier. The same source builds four
configurations, two architectures by two tiers, and `make check` runs the
same recorded shell session against all four.

At Tier 2 every process has an address space of its own. A program's
image is mapped into frames nobody else has, so two live copies of one
image are two processes that cannot reach each other, which is the first
thing this tier buys that Tier 0 cannot have at any price. The move
contract of section 6.1 is enforced rather than trusted: the pages of a
message buffer leave the sender as they enter the receiver and go back at
the reply, so a program that touches its own buffer during a call faults
on it.

```
$ touch_addr 40080000                         (Tier 0)
touch_addr: 0xd53800a0 at 0x40080000
[-717750112]

$ touch_addr 40080000                         (Tier 2)
sh: touch_addr: access fault at 0x40080000, pc 0x40f000f4
[-16]
```

That is one program, one command line and two tiers. Nothing in the
interface differs between them: enforcement cost three lines in the
rendezvous, and no operation, argument structure or error code exists at
one tier and not the other.

A fault is an exit with a reason. Every exception each port can raise is
mapped to a kind of section 16.1, exhaustively and checked at boot, and a
faulting thread dies with the kind, the address and the instruction in
its status, which the shell reports. A page fault is the exception: it is
a message the faulting thread sends to its pager, and the reply carries
the page. That needed nothing added to message passing, which is the
single most valuable borrowed idea in the design paying for itself:
priority inheritance across a page fault was never implemented, it simply
happens, because a faulting thread is an ordinary blocked sender.

A driver holds a grant over its registers rather than an address compiled
into it, and a program that touches those registers without one dies at
the address of the register. Program text can be demand-paged: `paged`
adds up the bytes of its own image, which arrives five pages at a time,
and gets the same answer as the kernel computes from the archive.

Kernel object memory is 504896 bytes, of which 360960 is thread stacks
and most of the rest is the process pool. Both pools are sized by the
boot check sequence rather than by a workload, because nothing is reaped
until M7.

**Milestone 5 is done: there are paths, and no absolute ones.** A process
receives its namespace at launch as a set of directory handles with names
attached, and resolves a path against it in the C library. Nothing asks
the kernel: the resolver is string handling over a table the process was
given, so a name that matches no entry reaches nothing at all and there
is no root anywhere that everyone has.

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

`/` and `/files` are one filesystem under two names, and `/u` is two
directories under one name, tried in order, which is a union. What a
child is launched with is a choice the shell makes: everything it holds,
a named subset, or nothing at all, which is `NS_SHARE`, `NS_CLONE` and
`NS_NEW` and is thirty lines of library rather than a mechanism.

Dot-dot is handled in two places for two reasons and neither is
redundant. The library reduces it before choosing a handle, so a path
crossing a namespace boundary reaches the server that can answer it. The
server refuses it whatever the client did, because a program may build
its own messages and must not thereby reach above the connection it is
using.

**Milestone 6 is done: there are cohorts, and the measurement they were
built for says the mechanism is aimed at the wrong cost.** A cohort is a
set of images the build names, running in one address space, and the
storage stack is one: a filesystem reading through a block store
underneath it. A member may hand a peer an address rather than a buffer
to be mapped, and a member that faults ends the whole cohort.

The same programs, the same session, one line of a build-time table
apart:

| | out of the cohort | in it |
|---|---|---|
| pages remapped | 42 | 12 |
| page-table entries written | 84 | 24 |
| single-address invalidations | 42 | 12 |
| address space switches | 395 | 358 |

It also removes a copy per whole block, which is not in that table and
turns out to be the only thing a cohort uniquely does. The rest is
better bought with address space identifiers, which neither port has and
which would keep every process apart. Section 9 has the argument and
what would settle it.

And the cost, which is not a number:

```
$ peer_c
cohort peers: member peer_c faulted, instruction at 0x0, pc 0x81800042
  the whole cohort is the recovery unit, per section 18.2: peer_a peer_b peer_c
  tearing them down together and restarting them is M7, and is not built

panic: cohort peers cannot be recovered and must not be left running
```

The same program outside a cohort is `sh: peer_c: instruction fault`,
and the shell carries on. A bug in the block driver takes down the
filesystem, and that is what section 18.2 is for.

**Milestone 7 is done: things can end, and something else finds out.**
A slot goes back to its pool when the object has finished and nothing
can ask about it any more. A thread can be broken out of any wait it can
be in. A process can be ended, and the two sides of a rendezvous it was
part of fail differently, because they are waiting on different things:
a server is at a receive point and collects a notice, a client is not
and is failed with an error. Neither depends on a deadline, and nothing
in the system uses one to discover a death.

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

The restart policy is a program, because section 18.3 keeps it out of
the kernel. Given a child that dies immediately every time, it makes
three starts and says so, rather than relaunching for ever and turning a
configuration mistake into a system that looks busy.

**Milestone 8 is done.** The kernel runs on two cores at Tier 2, with a
ready queue per core, threads that stay where they are put, and donation
that crosses a core boundary. Tier 0 is unchanged and single-core, which
section 13.3 decided before this milestone started.

Placement is explicit and nothing moves on its own. A thread is moved by
`sys_thread_migrate()` and by nothing else, so where it runs is a fact
about the program that placed it.

The storage stack runs split across the two cores, and what that costs
is measured rather than argued:

```
placement: the storage stack across two cores
    on one core it cost                    0 pokes, 0 donations
    with the store on the other core       4 pokes, 2 donations
```

Four inter-processor interrupts per read, per boundary crossed. The
recorded session is byte-identical in all four configurations, and at
Tier 2 it is produced with the stack still split: the shell reads its
files across a core boundary and the transcript does not know.

No scheduling decision takes a lock another core could hold, which is a
counter asserted at zero across the decision rather than a reading of
the source. Threads arriving from another core come through a list
pushed with one compare and exchange.

**Milestone 9 is done.** Both terms of section 13.3 are measured, on
both ports and both tiers, along with the IPC round trip and what a
donation costs. Every figure says which kind of number it is: a count is
a property of this kernel and is compared exactly, a duration is a
property of the emulator this ran on and is reported where it can move.

| | Tier 0 riscv64 | Tier 0 aarch64 | Tier 2 riscv64 | Tier 2 aarch64 |
|---|---|---|---|---|
| longest region | 596 to 1009 us | about 500 us | 571 to 733 us | about 800 us |
| mean region | 16 us | 16 us | 5 us | 7 us |
| interrupt to a program | 82 us | 147 us | 168 us | 189 us |
| round trip, best | 40 us | 31 us | 42 us | 34 us |
| donation, across cores | none | none | 56 us | 75 us |

The longest interrupts-disabled region begins in a reply and ends in the
next send, so it spans a context switch and contains no preemption
point. Section 13.3 claimed Tier 0's latency was bounded by preemption
point placement and it is bounded by the scheduler; the section is
rewritten around the measurement.

Crossing a core costs about three times a donation that does not, on
both ports independently, which is milestone 8's four inter-processor
interrupts seen in time.

`SYS_ERR_DEADLOCK` is reachable: a cycle of servers is refused instead
of wedging the machine with nothing said.

**Milestone 10 is done.**

A submission carries up to sixteen entries, validated before any of them
runs, each reporting its own status. Linked entries abort as a group.
A batch can be loaded once and executed many times.

What it saves is countable and small: a batch of eight costs one
crossing of the trap edge where eight calls cost eight, and seventeen
validation checks where they cost twenty-four. The saving is the fixed
cost of a call, multiplied by one fewer than the number of entries, and
nothing per entry.

What it does not answer is the question it was built for. The pattern
the engine exists for is a chain, where one entry names a handle an
earlier entry produced, and an entry's target is fixed when the batch
is submitted. Nothing in the system uses a batch: a run makes about 750
submissions and every one carries a single entry. Section 17 stays,
with its claims narrowed to what was measured and the deciding
experiment written down.

It also found what M8's kernel lock costs. Section 13.3 says the batch
bound does not appear in interrupt latency; it does not at Tier 0 and
it does at Tier 2, where one lock is held across the whole call, so
sixteen entries is one interrupts-disabled region sixteen entries long.

**The reference counts are fixed**, which three retrospectives had named
and none had done. `p_refs`, `t_refs` and `cn_refs` decide when an
object is freed, and the decision reads the count and other state
together, so an atomic counter would not have been enough. The count
and the decision are taken under one exclusion and the freeing is done
after it is released, because reaping an object closes handles that
release references to others and would arrive back at the same lock.

A check hammers one count from both cores, four thousand rounds each,
and requires it to come back where it started. With the plain
increments it comes back at 230 instead of 1.

**Milestone 11 is done, and with it the deciding experiment M10 named.**
An entry can take its target from a register another entry wrote, so a
batch can carry a dependency that used to live in a local variable
between two calls. A client that walks to a file, reads it, closes it
and gives the handle back makes one submission where the plain form
makes four.

What that saves is three crossings of the trap edge and three
validation checks, which is the fixed cost of a call multiplied by one
fewer than the number of entries and nothing per entry. It is the same
law M10 measured for batching without dependencies, so substitution
adds no saving of its own. What it adds is that the batch can be
written at all.

Set beside the compound protocol request of section 25.7, which saves
two rendezvous and never draws a connection from the server's pool:
for this workload, changing the protocol beat changing the kernel.

A reply now carries a status the kernel can read, so a request a server
refused is no longer a successful send and a linked chain aborts on it.
A batch that aborts force-signals the timelines it was going to signal,
so a waiter is told rather than left to a timeout.

**Milestone 12 is done, and it changed what section 13.3 claims rather
than what the kernel does.** It attempted the first of the two remedies
M9 named for the interrupts-off term: not disabling interrupts across a
context switch. Three splits were made and none of them moved the
figure by more than the spread between runs.

What it established instead is what the region is made of. The switch
is nought to three per cent of it. The rest is the rendezvous work that
has to be atomic with the switch, and lowering the floor now means
making that work shorter or making less of it atomic.

And the figure this project has published since M9 is the wrong one.
The longest region that spans a switch is opened by a kernel thread in
every run, doing rendezvous work in the checks. What a program is
delayed by is four to eight times shorter: sixty to a hundred and
fifteen microseconds against four hundred and fifty to five hundred and
sixty. Section 13.3 carries both now and says which is which.


What does not exist: a device that does direct memory access, and with
it the half of section 17 about transfers in flight. M11 gave that
absence a reason rather than leaving it a gap: starting work needs
something that transfers, and the one route that looks open through the
block store is an issuing send, which section 17.3 rules out on
structural grounds.

**M13 made a process give back what it was given**, and **M14 made it
stop taking a megabyte in the first place.** A process costs 15 frames
on riscv64 at Tier 2 and 14 on aarch64, where it cost 264 and 262, and
all of it comes back when the last thing that could ask about the
process lets go. A program was spawned and reaped 40000 times on a
machine with 32768 frames and the count returned to where it started;
before M13 the same loop stopped at about the ninety-fifth iteration.
At Tier 0 a process costs nothing, because a flat image runs in memory
the archive already occupies.

The archive carries a layout now and not only a length, so the loader
maps what an image describes rather than the whole of the slot it sits
in. That bought three things at once. Memory, which is the figure above.
Protection: a program's text is read and executed and never written, and
its stack is read and written and never executed, where one region had
meant one set of permissions and the only workable set was all of them.
And section 15.1's eager copy of writable segments, which the loader
could not obey while it could not see where text ended, and which had
never run because no paged program had any initialised data.

The boot ends with a ledger of every frame the machine has spent, and
it balances: what was spent before anything ran, what live processes
hold, what cohort address spaces hold, what grants hold, what a cohort
restart orphaned, what the kernel's own space has grown by, and nothing
left over. One row of it is a real loss: section 18.2 frees a cohort
place when its occupant exits rather than when it is reaped, so a
restart maps over the previous member's frames and they become
unreachable. That was 512 frames across a boot and is now 14, for the
same reason the rest of the figures fell.

A rendezvous still carries no deadline. Nothing has run on hardware, so
every duration this project has published is the right shape and the
wrong magnitude. The counts are not durations, which is why this
milestone asserts counts.

---

## 2. How This Plan Is Organised

Four rules shape the ordering, and they are worth stating because they explain
choices that otherwise look arbitrary.

**Every milestone ends with something that runs.** Not a subsystem that
compiles, and not a layer waiting for the layer above it. A milestone that
cannot be demonstrated cannot be shown to be wrong.

**A test has to be able to fail.** This was learned the hard way and is
listed with the rules because it now costs more than the others put
together. Four stages in a row shipped a check that passed while testing
less than it claimed: a format string that silently read the wrong
argument, waiters that never ran because they sat below the thread meant to
wake them, an idle path nothing could reach, and a phase check that would
have passed while the timer drifted had it been written against the
previous activation instead of the origin. In three of the four, repairing
the test immediately found a real fault in the code it was supposed to
cover. Before trusting that a check passed, make it fail.

One shape accounts for most of them: a thread waiting for another thread
it outranks. The boot thread yields, the scheduler correctly picks the
highest runnable thread, which is the boot thread again, and the test
spins for ever without the thing it is waiting for ever running. This
happened five times across four stages, twice after it had been written
down. It stopped happening when the boot thread was moved to priority one,
where breaking the rule takes deliberate effort rather than inattention.
A convention that has to be remembered is not a fix.

**Each milestone names what it would falsify.** The design is a set of guesses
and some of them are bad. The point of building in this order is to reach the
bad ones early, so the milestone descriptions below say what result would count
as the design being wrong rather than the implementation being unfinished.

**The three provisional mechanisms are scheduled where they can actually be
tested.** Timelines are exercised in milestone 1, per-process namespaces in
milestone 5, and the submission engine in milestone 10. Each is placed at the
first point where the question about it can be answered, not at the first point
where it could be written.

**A milestone is scheduled when something needs it, not when it becomes
possible.** This rule replaced an earlier one that put teardown second, on
the argument that death paths are where the subtle bugs live and are easier
to add to a system that works than to build alongside one. That argument is
still true and was still the wrong schedule: nothing in the first six
milestones has anything worth reclaiming, since programs that run to
completion, utilities that exit, and faults that panic all reach their goals
without it. Building cancellation, pulses, reaping and restart ahead of a
caller would have meant four milestones of maintaining machinery nothing
ever entered. Death is now M7, immediately before the first mechanism that
cannot be demonstrated without it, which is cohort recovery.

**Measurement comes late.** It needs a system and a real workload, and a
number taken before both exist is a number about nothing.

---

## 3. The Milestones

### M1. The Vertical Slice

**One thread, one channel, one driver, one file read.** Tier 0, single core.

The thinnest path that exercises handles, synchronous IPC and the filesystem
protocol together. Chosen because it is the smallest thing that could falsify
the design, rather than the largest thing that could demonstrate it.

A client process opens a file through a directory handle it was given at spawn,
reads from it, and prints the result. The file is served by a separate driver
process over the protocol in section 25. Nothing on that path is inside the
kernel except the message passing itself.

*Needs.* A handle table with rights over static pools (sections 7, 8).
Channels, send, receive and reply (section 12). Priority inheritance on that
path, built in from the start rather than retrofitted (sections 12.3, 21).
Timelines (section 9). Interrupts landing on timelines (section 13.2). Process
spawn with handle inheritance (section 10). Enough scheduler to run two
processes (section 11). The operation entry point (section 23). The filesystem
operations the read path uses, and no others.

*Not needed.* The MMU, cohorts, SMP, paging, namespaces, introspection, and the
submission engine. The slice uses one operation per trap and nothing more.

*Done when* the client prints the file’s contents, served from outside the
kernel.

*Unexercised so far.* Every caller of the operation entry point is kernel
code calling it directly. That is the same entry point and the same
implementation, which is the whole point of section 23.1, but the trap edge
itself waits on there being user code to trap from.

*Would falsify.* Whether send, receive and reply with donation is as small as
assumed. **Whether timelines earn a separate object at all**, or whether
blocking in receive is sufficient and the object is ceremony. Whether
handle-relative naming with no ambient root is workable rather than merely
defensible. Whether a protocol carrying the offset in the message survives a
real client.

*On timelines.* Answered, in section 4. The short form: they earn a
separate object but not the exclusive one section 9 claimed, and what they
cost is three obligations that section does not mention.

*Size.* Large. It is most of a kernel.

**Done.** Section 4 says what it answered, what it cost, and what it left
untested.

### M2. Two Processes

**A real scheduler running two user programs, and the file read between
them.** Tier 0, single core.

Milestone 1 read a file from outside the kernel, but every participant was
a kernel thread and every caller of the operation entry point was kernel
code calling it directly. The interface was exercised and the boundary was
not. This milestone puts the same slice across a real one: independently
loaded programs, a trap edge with user code on the far side of it, and a
scheduler that blocks and resumes them as the rendezvous requires.

It comes second because it is what makes everything after it demonstrable.
A shell needs processes to spawn. The MMU needs two address spaces to be
worth having. Cohorts need members. None of that is testable while the
system is one address space of kernel threads that happen to send each
other messages.

*Built, and running on both architectures.* The slice runs a second time
with every participant a loaded program, and then the same read a second
way to answer section 25.7:

```
fs_client  read 31 bytes in 3 rendezvous, 1 connections
fs_batch   read 31 bytes in 1 rendezvous, 0 connections
```

- **The startup record of section 20.4.** A versioned record carrying
  argc, argv, envp and a namespace table, written by `proc_spawn` and
  consumed by libc before `main()`. The table is a flat array of
  identifier and handle pairs rather than named entries: eight slots cost
  128 bytes on a 64-bit build and 64 on a 32-bit one, and a lookup is an
  integer compare. Identifiers below `SYS_NS_PRIVATE` mean the same thing
  everywhere and the rest are a convention between a parent and its child.
- **crt1 into `__libc_startup` into `main`,** on both architectures. libc
  copies the table rather than holding a pointer into loader memory, and
  refuses a record whose version it does not know.
- **The trap edge, both ports.** SP_EL0 in the AArch64 exception frame and
  the mscratch swap on RISC-V, so a thread switch inside a system call
  does not lose a user stack. `enter_user` carries the startup record in
  the first argument register.
- **The userland call interface.** `include/os/sys.h` and the wrappers
  behind it, each filling in one descriptor and submitting it through the
  single entry point of section 23.1.
- **The boot archive of section 24.4, and loading from it.** A header, a
  table of fixed-size records, and page-aligned payloads, written on the
  host by `mkbootarch` and carried in the kernel's rodata, which is the
  closest thing this port has to the flash that section assumes. Loading
  is the RAM loader of section 15.2 rather than execute-in-place, because
  the programs are linked at absolute addresses.
- **`fs_server`, `fs_client` and `fs_batch` as ordinary programs.** The
  server is handed its receive handle at spawn and creates nothing,
  because there is no operation for creating a channel and there should
  not be.
- **Blocked-state introspection.** Threads carry an explicit wait state
  and a fixed ring records transitions, which is the smallest thing
  section 22.2 means by enough to build a tracer. A rendezvous cannot be
  observed by sampling, since whichever thread samples is running. Not
  `sys_stat_read`, which needs thread handles and a supervisor to hold
  them; that is M9.
- **Compound requests, section 25.7.** Walk and read in one message, with
  implicit linear chaining and abort at the first failure. Bulk data moved
  to a buffer field on the message rather than a grant, which is a
  lifetime distinction: a payload lives for one call and a grant outlives
  the call carrying it, so grants stay for device registers, pager pages
  and scatter-gather and are deferred with them to M6.
- **Exit.** `SYS_OP_THREAD_EXIT` records a status and takes the thread off
  the scheduler. Nothing else, per the note below.
- **The debug console of section 22.4.** A handleless write to the port's
  polled UART, with a libc macro layer so a program's diagnostics leave no
  trace in an image built without them. Gated at build time and again at
  run time, and permanent: a driver on a new board has to be debuggable
  before the console service it would report through can exist. The
  V7-numbered libc stubs went with it, having never worked.
- **What Tier 0 checks and what it trusts** on a descriptor crossing the
  boundary, in section 23.2, in section 27 as a port obligation, and in
  the trap path where someone reading it will find it. There is no address
  space to validate a user pointer against, so what was owed was the
  statement rather than the check, and section 23.2's real test waits for
  the MMU.
- **Halt on an exit nobody was waiting for.** The kernel announces every
  exit and stops on an unclaimed one, naming the process. That is the
  M2-shaped placeholder for the supervision of section 24.3.

*Not needed.* The MMU, cohorts, SMP, paging, the submission engine, and
**anything to do with death**. Programs run to completion or the system
halts. See below.

*A defect this found, and fixed.* **A server whose base priority exceeded
its client's corrupted the run queues.**

`channel_send` handed the message over and called `sched_ready` on the
server, which dispatches at once when the readied thread outranks the
running one. The server ran, replied, and readied a sender that the
preemption had already queued and that had never reached `sched_block`.

Found by raising `fs_server` above `fs_client` while testing the tracer,
which is the whole argument for building the tracer. Nothing else reaches
it: donation raises a server to exactly its client's priority, and equal
priority does not preempt, so only a base priority above the client's gets
there.

`sched_ready_nopreempt` makes a thread runnable and takes no scheduling
decision, for a caller that is about to take it in `sched_block`. A server
at 26 serving a client at 22 is now a permanent check, and it panics
without the fix.

**The first estimate of this was wrong and is worth recording as such.** It
said the fix needed a scheduler change with deferral nesting thought
through. It did not: `sched_ready` does two things and this caller wants
one of them, and separating them does not touch deferral at all. The estimate came
from reading `sched_defer` as the only tool for making two scheduler
operations atomic, rather than asking whether the second operation was
needed here.

*A loose end this created, and closed.* libc carried V7-numbered stubs for
`open`, `read`, `write` and `close` that passed a call number where the
trap path expects a descriptor pointer and a count, so they would have
failed had anything called them. The debug console settled it: `write`
goes to the console, the other three are deleted rather than left as
plausible-looking traps, and there are no file descriptors to pretend
otherwise about.

*Done when* two programs load and run as separate processes, the client
walks a directory handle it was given at spawn, reads the file, prints it,
and the same read done as one compound request produces the same bytes in
one rendezvous instead of three.

Met, including the printing, which the debug console of section 22.4 made
possible after this clause had been written off as unmeetable.

*On death, deliberately.* When a process exits, `SYS_OP_THREAD_EXIT`
records a status and takes the thread off the scheduler. Nothing is
reaped, no slot is reused, no completion timeline is advanced, nothing can
join, and a `TODO` in the exit path names M7. This is the whole of what M2
needs from teardown, and pulling any more of it forward would mean
building cancellation and pulses for a system with nothing to cancel.

*Would falsify.* All three are now answered.

**Whether the operation descriptor survives crossing a real protection
boundary.** It does, unchanged, and nothing about it wanted a different
shape. Tier 0 validates nothing, so this is as far as the question goes
until M4.

**Whether the rendezvous of section 12 is what a server actually wants**,
now that a server is a program rather than a function called by the test.
It needed no change.

**Whether the compound request of section 25.7 pays.** It does, and the
reason is not the one the section leads with. Three rendezvous become one,
which was expected; the connection pool goes from one allocation to none,
which is the structural saving, because section 25.2 makes that pool a
build-time bound on every file open at once. Two things in the section
were wrong and building it found them: an entry index placed in the field
a read needs for its file position, and a close entry that does nothing
because the server releases an intermediate node when it replies.

*Watch for.* **The scheduler was the part most likely to pass while
testing less than it claimed**, and it now does not. A demonstration where
one process runs to completion before the other starts exercises no
blocking at all and produces identical byte counts, so the stage checks
the states each thread passed through rather than inferring them from the
result. `fs_client` must queue behind a busy server and `fs_batch` must
not, and claiming otherwise for either fails the stage.

The trace of a three-message read is the shape section 12.1 describes:

```
fs_client  reply-blocked      the server was waiting, so this never queued
fs_client  send-blocked       the server was busy; this one did
fs_client  reply-blocked      taken from the queue without running
fs_client  send-blocked
fs_client  reply-blocked
fs_server  recv-blocked       nothing left to serve
```

**What was proved, and how.** Each of these was confirmed by breaking it
and watching the stage fail. Entering at EL1h instead of EL0t makes the
program's `svc` reach the current-EL vector and panic, so the working
build is at EL0 rather than merely appearing to be. Demanding `mcause` 11
instead of 8 panics with `cause=0x8`, which is ecall-from-U-mode, so the
RISC-V side is in U-mode for the same reason. Changing the server's string
to a twelve-byte one moved the client's exit status to 12, so the bytes
executing are the bytes built. A compound walking a name that does not
exist reports zero entries completed and entry zero failed, so a failed
sequence genuinely stops. A compound claiming four entries with payload
for two is refused before any of it runs. And the two clients report the
same numbers in either order, which is what makes the pool measurement
mean anything: `highwater` is monotonic, so without a reset whichever ran
second could not raise it and would have reported zero connections even if
it allocated one. The high-priority-server check panics without the
scheduler fix, and the debug console leaves no banner, no program output
and a still-passing stage when it is compiled out.

*Size.* Small, from large, and spent.

**Done.** Section 5 says what it answered, what it cost, and what it left
untested.

### M3. A System You Can Talk To

**Console input, a shell, and a few utilities as separate processes.**

The design claims a complete system rather than a kernel. That claim is
unsupported until there are utilities to point at. This milestone also produces
the workload every later milestone is tested against, which is why it comes
before the MMU rather than after it.

*Needs.* The read side of the UART as a driver process. A console service. A
shell that spawns a utility, waits on its completion timeline, and reports
status. Enough libc to write the utilities.

*Five things it needs that were not listed, and each is a stage rather than
a detail.* M2 reached its own goal without any of them.

**No program can spawn another.** M2's three programs are spawned by the
kernel's boot thread calling `proc_spawn_image`, and a shell that cannot
spawn is not a shell. Spawn becomes an operation naming a member of the boot
archive, with the namespace array the caller assembles from handles it
already holds. That leaves the archive walk and the image copy inside the
kernel, which section 24.1 says they should not be. The debt is M2's rather
than this milestone's and it is named here because this is where it becomes
visible: it is paid when there is a process able to load an image, which is
the loader of section 15.2 and the root process of section 24, and neither
is worth building before the MMU gives them something to do.

**No program can create a thread.** A console driver waits on its device and
on its clients at once, and section 19.3 answers that with two threads and
no shared state. There is no operation for creating one, so the shape this
design prescribes for every driver from here on cannot be written at all.
The stack comes from the caller, because the kernel allocates no user
memory.

**Neither board has an external interrupt.** The only interrupt in the
system is the per-core timer, which is a private peripheral interrupt on
AArch64 needing no routing and the machine timer on RISC-V needing no
controller at all. A UART is neither. AArch64 needs the distributor
programmed for a shared peripheral interrupt, and RISC-V needs a PLIC, which
the port does not have in any form: `md_irq_unmask` knows one interrupt by
name and `md_irq_claim` reads a variable the trap path set. This is port
work under section 27, and it is the first evidence for or against the claim
that the interrupt path of section 13.2 generalises past the device that was
easiest to reach.

**No program has ever been interrupted.** Both ports enter user mode with
interrupts masked, deliberately and with the reason written down: the
AArch64 lower-EL interrupt vector is the fatal entry, so an interrupt taken
while a program runs would panic, and the RISC-V port leaves `MPIE` clear to
match. A console driver is a program that waits for a device, so this has to
be undone on both sides, and undoing it means a thread can now be switched
out from an interrupt taken in user mode. The discipline for that exists,
since M2 built it for the system call path, but nothing has ever entered it
from an interrupt.

**An interrupt a handler cannot finish with.** Section 13.2 requires the
handler to be done with the device before it advances anything, because
advancing can switch to a waiter that would otherwise re-enter a still
asserting device. A timer is finished with by reprogramming it, which the
kernel does. A UART is finished with by reading the byte, which is driver
logic and belongs in a process. Nothing in the design resolves that, so this
milestone has to: the handler silences the source at the controller, and the
driver unsilences it after draining. That makes an interrupt an object a
program holds a handle to, which is also half of the missing opcode section
13.2 admits to.

*Exit and join, which is not death.* A process that exits advances a
completion timeline, the handle stays valid afterwards and carries the
status, and a joiner waits on it (section 10.4). This is the normal path and
it belongs here, with the shell that is the first thing to need it. Nothing
is reaped, no slot is reused, and nothing is killed; M7 is still where all of
that lives. The freezing of a completion timeline into the holder's handle
slot is part of reaping and goes with it.

*Stages.* Eight, ordered so that each leaves something that runs and so
that the part which can fail for reasons outside this tree comes early.
The port work is second rather than fourth for that reason: it is the
only part that depends on hardware nobody here has run this system
against, and it blocks the console rather than the other way round.

**1. Exit, completion timelines, and join.** *Built.* The timeline is
embedded in the thread and the process per section 10.4, so obtaining it
cannot fail, and the wait is the existing fence wait rather than a new
primitive. `SYS_OP_OBJECT_COMPLETION` hands out a wait-only handle and
`SYS_OP_PROC_STATUS` reads the code a counter cannot carry. A process
finishes when its last thread does, which is why threads are counted now
rather than in stage 4. The M2 placeholder that halts on an unwatched exit
now asks whether anything holds the completion timeline. Cost: 5632 bytes
of kernel object memory, which is what embedding two timelines per thread
and per process comes to.

**2. The first external interrupt.** *Built.* The riskiest stage and the
one with no dependencies, so it went first. It ends with the kernel
printing characters somebody typed, which is a long way short of a
console and is the whole of what it claims.

- Interrupts unmasked in user mode on both ports: `DAIF` cleared in the
  `SPSR` that `enter_user` builds, `MPIE` set on RISC-V, and the AArch64
  lower-EL interrupt vector wired to a real entry rather than the fatal one.
  That entry needs the `SP_EL0` handling the system call path already has,
  because an interrupt from user mode can switch threads exactly as a system
  call can.
- Routing on AArch64: the distributor programmed for the PL011, which is
  shared peripheral interrupt 1 and therefore INTID 33 on the virt board.
- A PLIC on RISC-V, which is the whole controller and not an addition to
  one: priority, enable for the machine-mode context of hart 0, and threshold,
  with claim and complete wired into `md_irq_claim` and `md_irq_eoi`. The
  16550 is source 10 there.
- One numbering for both. The interrupt table is indexed by `mcause` on
  RISC-V today, where the machine timer is 7, and PLIC source numbers start
  again at 1. The two spaces have to stop overlapping before the table has a
  second occupant.
- `md_irq_mask`, and the rule it exists for. An interrupt bound to a
  timeline is masked by the handler before the advance, and stays masked
  until its driver says otherwise. An interrupt bound to a kernel handler,
  which is only the timer, is not, because that handler does finish with its
  device.
- An interrupt as an object a program can hold: `SYS_OBJ_IRQ` filled in,
  and `SYS_OP_IRQ_UNMASK` behind a handle carrying `SYS_RIGHT_IRQ`. The
  name follows section 26, which already lists `sys_irq_unmask()` without
  saying anywhere what it is for. Attaching stays kernel-internal, per
  section 19.2's preference for the launcher attaching rather than the
  driver, and section 13.2 still calls the missing opcode an omission.

*Check.* The kernel waits on the UART receive timeline, reads what
arrived, acknowledges the interrupt, and repeats until three characters
have come in. A second check spawns a program that busy-loops with a
timer armed behind it, and counts the interrupts taken while a program
rather than a kernel thread was running.

Each was confirmed by breaking it. Without the mask the machine wedges
in the handler before it prints anything, which is the failure with the
least information attached in the whole milestone and the reason the
rule exists. Without the acknowledgement the second keystroke never
arrives and the five-second deadline says so. With the AArch64 lower-EL
vector put back to the fatal entry, the spinning program panics with an
unhandled trap at its own program counter.

Two things it settled that the plan had guessed at. **Characters and
interrupts are different counts**: three characters piped in arrive as
one interrupt and one drain, and three characters typed with gaps
arrive as three, so a check written against a count of interrupts would
have hung on exactly the input a scripted session provides. And **the
RISC-V side needed no unmasking at all**: the privileged specification
enables machine interrupts whenever a hart is in a less privileged
mode, regardless of MPIE, which was confirmed by removing the bit and
watching the interrupt arrive anyway. It is set for the state to be
intended rather than accidental, and the AArch64 side is where the
actual change was.

*Repaired in stage 4.* The second half of this check was flaky and
nothing noticed for two stages. The timer was armed once, one
millisecond out, and under emulation the virtual clock runs on wall
time while the first execution of a path pays for translating it: the
activation could expire before the child had run an instruction, land
in the kernel, and leave the program with nothing to be interrupted
by. It passed six times and then failed two runs in three. The timer
is periodic now, so a program that runs for eighty milliseconds has
eighty chances rather than one, and it reports how many it took.

**3. Spawn as an operation.** *Built.* What a shell needs, and no more
of section 24 than that.

- `SYS_OP_PROC_SPAWN`: an image name, a priority, and a namespace array the
  caller assembles. It returns a handle to the child carrying the rights the
  parent needs to join it, which stage 1 already defines.
- Every handle in the array is moved out of the caller's table, per section
  7.4, and a caller that names a handle it does not hold gets an error rather
  than a partly built child.
- A failure that leaves nothing behind. The image is looked up and the array
  is validated before a process exists, because the alternative is a process
  half assembled and no way to unmake it before M7.
- The load slot stays a build-time constant. There is no MMU, so two
  programs live at once need two slots, and that is what keeps this
  milestone's shell to one child at a time. Section 15.2 removes it.

*Check.* `spawner` starts `status`, joins it, and reports what it read,
with both of them ordinary members of the boot archive and every
operation on that path taken from user mode. It then makes five spawns
that must fail, and then spawns again. A handle it does not hold, a handle
it holds and may not transfer, an image that is not in the archive, a
priority above its own, and an image that already has a live process.
The status it exits with is the child's if all of them held and a
negative code of its own if any did not, which is how a program reports a
number before there is any formatting in libc.

Removing the pre-validation panics with the handover failing after it
was checked, which is the partly built child the ordering exists to
prevent. Removing the priority rule ends with the spawner exiting -7, and
removing the live-image refusal ends with it exiting -8, each with a line
from the program saying which check it was.

Three things this settled. **A child may not outrank its parent**, which
section 11 did not say and now does: priority is authority over the core
and a program cannot hand out time it was not given. `SYS_PRIO_INHERIT`
goes with it, because a program has no operation that tells it its own
priority and the common case is starting something alongside itself.
**The process name has to be copied**, which running it found: the
name in a spawn argument is a stack frame in a program, and the kernel
says that name again when the process exits, so the first run printed
sixteen bytes of whatever the frame had become.

And **one live process per image**, which reviewing it found rather
than running it. Loading copies a member to the address it was linked
for, so a second copy of a running program writes over its text and its
data, and nothing notices. What makes it worth refusing rather than
documenting is that it does not fail reliably: two short programs that
never overlap in time both finish normally, which is what the first
attempt at demonstrating it produced. Two smaller repairs went with it.
The namespace array is copied before it is validated, because the
caller's array is memory a program owns and section 23.2's "once" means
the check and the use have to read the same thing; without the copy a
second thread could change it between them, which stage 4 is about to
make possible. And the rights the spawn hands out are enforced by the
two operations that use them, which they were not: the handle carried
SYS_RIGHT_WAIT and SYS_RIGHT_STAT and neither was ever checked.

**4. Threads a program creates.** *Built.* Nothing new was decided here,
only reached: section 10.2 already said the creator supplies the stack.

- `SYS_OP_THREAD_CREATE`: a stack the caller owns, an entry point, one
  argument, and a priority. The stack is memory in the program's own image,
  because the kernel allocates no user memory at any tier.
- A handle to the new thread, so that `SYS_OP_OBJECT_COMPLETION` reaches a
  thread and not only a process, which is the arm of stage 1 that has had no
  caller.
- `SYS_OP_THREAD_STATUS`, for the same reason.
- The process thread count from stage 1 stops being decoration: a process
  with two threads finishes when the second one exits, and that has never
  been exercised.

*Check.* `threads` creates a thread that spawns a program and waits for
it, while the thread that created it waits on that thread's completion:
two threads blocked on different objects at once, which is the shape
section 19.3 prescribes and the first time anything here has had it. It
then creates a second thread and exits while that one is still blocked.
The process reports 9, which only the second thread could have written,
and not the 0 the first thread exited with.

Breaking the last-thread rule does not wait for that check to notice.
The kernel's own process has many threads, so the first kernel thread
to exit ends it, and the machine halts on an exit nobody was waiting
for within the first few checks.

Three things this settled. **A check that goes through the library cannot
test what the library fixes up.** The misaligned stack this refuses was
first checked by calling `sys_thread_create` with a misaligned array,
which succeeded, because aligning the region it is handed is part of
what the wrapper does. The check builds a descriptor and submits it
instead, which is the operation and not the convenience over it.

**A thread costs 4320 bytes of kernel object memory before it has a
user stack**, 4096 of which is the kernel stack of section 8. That is
the first real number for the question this milestone exists to answer,
and the program's own stacks are 4096 bytes each on top of it.

And **the state a status call reports had been invented twice**, which
reviewing it found. Section 26 has named `SYS_STATE_READY`,
`SYS_STATE_RUNNING`, `SYS_STATE_BLOCKED` and `SYS_STATE_EXITED` all
along; stage 1 added `SYS_STATUS_LIVE` and `SYS_STATUS_EXITED` beside
them, and the second of those had a different value from the constant
it shared most of a name with, so a program written against the
document would have read a live thread as an exited one. This is the
third time the same shape of mistake has cost something: the V7 stubs
in M2, the interrupt acknowledgement in stage 2, and now this. A thread
reports which of the three live states it is in rather than a flag,
since the states exist and the kernel knows them.

**5. The console.** *Built.* One process, two threads, holding the UART.

The roadmap said a driver process and a console service, and this builds
them as one program. The split costs a rendezvous per line and buys nothing
until there is a second character device to put behind the same service, and
a milestone that is measuring what threads cost should not add a process to
avoid one. Say so plainly rather than quietly, because it is a deviation
from section 19's shape.

- The receive thread: blocked on the interrupt timeline, draining the FIFO,
  acknowledging the interrupt, and putting bytes into a ring. Echo and
  the two editing keys a person needs live here, because a line the user
  cannot correct is a demonstration nobody can drive.
- The request thread: blocked in receive, serving reads and writes over
  section 25 on connections it mints itself.
- A reader waits when there is no line, and that wait is the reply it has not
  sent yet, per section 12.1. There is no queue and no second buffer of
  requests, since a console with two readers is not something this milestone
  has.
- The transmit path is polled, at the priority of whoever asked. The kernel's
  own console stays exactly as it is, per section 22.4, which means two
  writers to one device and a rule saying the kernel's writes are for panics
  and boot and the driver owns everything else.
- Its device authority is the register base, compiled in. There is no grant
  object until M4, and section 19.2 wants one; what Tier 0 has instead is a
  region the process may already address, so what is written down is the
  deviation and where it goes.

*Check.* Two reads, because a console has two paths and a filesystem
has neither. The first finds a line already in the ring, which is what
a scripted session always produces, since a pipe delivers the whole
session before anything asks for any of it. The second arrives at an
empty ring, is held rather than refused, and is answered by the other
thread when the line comes in. The trace of section 22.2 shows both
threads blocked on different objects at once, which is the shape
section 19.3 claims and the first time anything has demonstrated it.

Answering a read with nothing instead of holding it fails with a
zero-byte read. Removing the unmask hangs the machine after the first
line, which is the failure with the least information attached in this
milestone and stays that way: a rendezvous carries no deadline, so a
client that will never be answered waits for ever. Section 12.1
describes a timeout on a rendezvous and the operation has no field for
one; that is a gap, and M7 is where cancellation and timeouts arrive.

Four things this settled. **The two threads share state, and section
19.3 now says what protects it.** A driver that only programs a device
needs nothing shared, and a driver that carries data has to move bytes
from the thread that receives them to the thread that answers for them.
There is no mutex to reach for. What makes it safe is that both threads
run at the same priority, so neither preempts the other, and every
point that touches the ring sits between two blocking calls. It is a
uniprocessor argument with a stated expiry, and M8 is the expiry.

**Input is a ring, and a full ring drops the newest byte.** The first
attempt kept one line and dropped what followed, which works for a
person typing and loses everything after the first line of a pipe. A
ring makes a burst and a keystroke the same thing to a reader, which
is what a scripted session and a person both need.

**A line longer than one reply broke the boundary between lines**,
which reviewing it found and no check would have. The reply carries
forty-eight bytes; a sixty-byte line returned forty-eight and the line
count was decremented anyway, so the remainder stayed in the ring with
nothing recording that it was there. The next read was held, and then
answered with the tail of the line before it while the line the reader
was actually waiting for sat behind it. A line stays counted until its
newline has gone, so a partial read is finished before the next line
starts. Three smaller repairs went with it: a newline whose byte was
dropped by a full ring no longer counts as a line, a read is bounded by
the length the client asked for rather than by what fits, and a write
is bounded by what the message carried rather than by the length it
claims. And CR LF from a terminal is one line ending rather than two.

**The protocol numbering was reconciled here rather than in stage 6,**
because a console that serves reads and writes cannot wait for it.
`FS_OP_WRITE` exists now and the subset uses section 25.3's numbers,
so it is sparse rather than renumbered: what is missing is missing
rather than moved.
**6. libc's two ends.** *Built.* A program's own input and output,
distinct from the diagnostics of section 22.4.

- `read()` and `write()` against the console connection found in the startup
  namespace under `SYS_NS_CONSOLE`, which already has an identifier and has
  never had a holder.
- `FS_OP_WRITE` and the numbering, which stage 5 had to do first: a
  console that serves reads and writes cannot wait for the protocol to
  agree with section 25.3.
- Enough formatting for a utility to be worth writing, and a line reader
  over `read()`.
- The debug macros keep their own spelling and their own build switch. A
  program's real output is one thing and its diagnostics are another, per
  section 22.4, and the moment `write()` stops going to the backdoor is the
  moment that distinction becomes real rather than stated.

*Check.* `greet` is built with the debug console switched off, so the
macros expand to nothing and there is no call to the backdoor anywhere
in its image. It formats a line, asks for input, and says what it read.
Everything it says arrives through a connection it was handed at spawn.

Spawning it with no console in its namespace is the other half. It
prints nothing at all, not one byte, and exits -1, which is what makes
the first half evidence rather than a demonstration: an image without
the backdoor talks to a person through a handle or it does not talk.

The plan had this check the wrong way round. It asked for a program
whose `write()` produced nothing with the debug console compiled out,
which is the behaviour this stage exists to remove.

Four things this settled. **All three descriptor numbers name the one
connection.** There is one console and nothing else to open, so a
descriptor table would be a table with one entry repeated; it arrives
with the mount table in M5, and until then the POSIX numbers are a
spelling rather than an index.

**The library has a formatter of its own**, which is duplication with a
reason. The kernel's writes a byte at a time to a device it owns; this
one gathers bytes into a message, because every write is a rendezvous
with another process. Sharing them would mean a formatter parameterised
by how it emits, which is more machinery than either side saves.

**The driver was the only writer to that device that did not translate
a newline.** The kernel's console has always written a carriage return
before a line feed, and the driver did not, which is why the first
program to print through a handle had `\r\n` in its format strings.
A program that has to know that is a program written for this console
rather than for a console, so the translation is in the driver, beside
the echo and the erase, which are terminal conventions for the same
reason. Two smaller repairs in the formatter went with it: the sign of
a zero-padded negative counted toward its width, which printed -0007
for -7 in four columns, and a null string faulted on the way to
reporting whatever had gone wrong.

**A connection a parent hands out has to carry more than it hands
over.** The kernel's console connection was minted with the right to
send and nothing else, so the first attempt to give a child a copy was
refused: section 7.4 attenuates on duplication, and a handle that
cannot be duplicated cannot be shared. Also `child_join` now takes how
long to wait, because a child that computes is done in milliseconds and
a child that reads the console is waiting for a person.

**7. The shell, and two utilities.** *Built.* The milestone's stated
goal.

- Read a line, split it, match the first word against the boot archive, spawn,
  join, print the status. That is the whole loop.
- Two built-ins and no more: one to leave, which is what ends a scripted
  session, and one to report the last status.
- A child gets the console by duplicating the shell's connection and
  moving the duplicate into the child, per section 7.4. That was the
  open choice and it is closed: asking the console for a fresh
  connection per child costs a rendezvous per spawn and buys nothing
  while the console has no per-connection state to keep. It buys
  something the moment it does, which is where M7 finds it.
- `echo`, which needs argument passing and nothing else, and `cat`,
  which reads through `fs_server` so that the M1 path stays exercised
  by the thing built on top of it.
- The shell blocks on reading a line and on joining a child, one after the
  other and never both at once. The first shell that wants a keystroke while
  a child runs is the one that needs a second thread, and noticing that is
  part of what this milestone owes.

*Check.* A person types `echo one two three` and sees it, then
`cat motd` and sees the bytes M1 read, then `nosuch` and is told the
shell cannot run it, then `cat nothing` and sees a status of three,
then `last` and sees the three again, then `exit`. The kernel joins the
shell as it joins any child and reports how it ended; a shell that ends
any way other than being told to is a fault rather than a session.

Spawning it without a console in its namespace ends with status -1 and
not one byte printed, which is the same evidence `greet` gave in stage
6 and matters more here: a shell is the program with the most reason to
have an ambient way out, and it has none.

Four things this settled. **Arguments needed the operation, the record
and the runtime.** `echo` exists to need them, and they arrive the way
section 20.4 says: the loader places them in the child's memory, which
at this tier is the process object, and the library copies them out
before `main` runs for the same reason it copies the namespace table.
crt1 puts them in the first two registers by asking the library for
them one at a time, so no assembly knows the shape of a C type and no C
declaration of `main` has to disagree with the definitions in the
programs.

**Two servers answered a read differently, and a client should not have
to know which.** The filesystem wrote into the buffer the client
supplied and the console replied inline, so `cat` written against the
console read nothing from the filesystem and exited zero. Both now do
what the client's request asks for: into the buffer when there is one,
inline when there is not, which is section 25.5 read as a rule about
the request rather than about the server. That is also the first
evidence for the claim that anything speaking these messages is a
filesystem, since the console now is one.

**One read is not one line**, which reviewing it found by typing a long
one. A read carries at most what fits in a message, so a line longer
than forty-eight bytes arrives in pieces, and `readline` returned the
first piece as though it were a line. The shell ran it as a command and
then ran each remaining piece as another: one line became six, five of
them nonsense reported as if a person had typed them. `readline` reads
until it sees the end of a line now, and the shell says so when one
does not fit. The same run found that a ninth word was silently merged
into the eighth, which `echo` hid by joining its arguments with the
spaces that had been lost; a line with more words than a command can
carry is refused.

**A failed spawn must not keep the handles it was given.** The shell
duplicates its console and its directory for every child, and a
duplicate is not moved by a spawn that fails: without closing them on
that path a shell stops working after sixty typing mistakes.

**8. The session that runs itself.** *Built.* A demonstration needing a
person at a keyboard cannot be run twice the same way, and every stage
above was checked by hand until this existed.

- A canned session piped into the machine's console, ending in the command
  that leaves the shell, with the kernel halting afterwards as it does now.
- A `make check` target that runs it on both architectures under a timeout,
  and compares what came back against a recorded transcript.
- The comparison starts at a marker line and not at boot. The boot output
  carries measured values that differ between runs, and a check that compares
  them is a check that fails for the wrong reason.
- Input from a pipe arrives faster than a person types and with no gaps
  between characters, which is a different test of the driver than a person
  is. Both are run.

*Check.* `make check` runs the session twice and takes about thirty
seconds. Paced, at two seconds a line, which is how a person types:
every line arrives while the machine is already waiting for it, and the
transcript from the shell's banner onward is compared against
`share/check/expected.txt`. Then burst, with the whole session arriving
before anything has asked for any of it, where what is checked is that
the machine reaches the end without a panic. The comparison starts at
the banner because everything before it carries numbers that differ
between runs and between boards.

Changing one byte of the expected transcript fails the target. That
took two attempts, which is the first thing this settled: **the
transcript was full of carriage returns**, so the edit meant to break
the check matched nothing and the check passed. A terminal needs a
carriage return before every line feed and the driver writes them, but
a file a person reads and edits should not carry them, so the capture
strips them.

**Each mode catches what the other cannot**, which is what makes
running both worth the time rather than a gesture at thoroughness.
Restoring the console's first behaviour, where a completed line blocks
everything typed behind it, leaves the paced session passing to the end
and hangs the burst one after its first line. A ring too small for a
burst fails both. A person and a pipe are different tests of the same
driver.

*Done when* an interactive shell runs on both architectures, spawns a
program as a separate process, and reports its exit status. And the same
session driven from a pipe rather than a keyboard produces a transcript a
check compares, because a demonstration that needs a person at a keyboard
cannot be run twice the same way.

**Met, on both architectures.** `make check` is the second half and
takes half a minute.

*Would falsify.* Both are now answered.

**Whether one thread per event source is affordable.** It is, at this size,
and the milestone can say what it cost rather than that it felt acceptable.
The decision to have no wait-set cost this system exactly one thread: the
console's, which waits on the interrupt while the other waits on the
channel. Every other thread in the running system has a reason of its own.
That one thread is 4352 bytes of kernel object memory, 4096 of which is the
kernel stack of section 8, and 4096 more of user stack in the driver's own
`.bss`, so eight and a half kilobytes for the whole of what the decision
costs here.

The projection is what matters, and it is linear rather than alarming: a
system with N devices that must stay responsive while serving pays N such
threads. The shell will pay one more the day it wants to notice a keystroke
while a child runs, which it does not yet. Nothing in this milestone came
close to the stacks being unaffordable, so the trigger in `PROPOSALS.md`
stays untripped, and the entry there stands unchanged.

The whole run reports 36 of 64 threads and 21 of 32 processes at their high
water, with 147456 bytes of kernel stack under those threads. That number
is mostly the boot checks rather than the system: nothing is reaped before
M7, so every program any check ever started still holds a slot at the end.
The system that is actually running when the shell exits is the console at
two threads, the filesystem at one, the shell at one, and the kernel's boot
thread.

**Whether an interrupt the handler cannot finish with breaks section 13.2.**
It does not, and the fix needed no device knowledge in the kernel. A handler
that cannot finish with a device masks the source at the controller instead,
and the driver unmasks it when it has drained the device. The kernel knows
which interrupt to mask and nothing else about the UART; `md_irq_mask` is
one register write on each port. Section 13.2 and section 26 now carry the
rule and the obligation that goes with it, which is that a driver drains to
empty before it unmasks.

*What it cost.* Five operations, an interrupt controller per port, a
console driver, a shell, three utilities and a formatter. Kernel object
memory went from 325568 bytes to 365248, and all but 256 of that increase
is the process pool doubling because a boot that spawns seventeen times ran
out of sixteen slots.

*What it left untested.* A second reader of the console, which the driver
would answer by displacing the first. A client that dies inside a
rendezvous, which nothing here can produce. Anything at all about
recovering from a driver that stops answering: the machine hangs, because a
rendezvous carries no deadline, and section 12.1 describes a timeout the
operation has no field for. All three are M7's.

*Watch for.* Every one of these happened, which is why they are kept.

**The check has to be able to fail, and interactive input is the easiest
place yet to write one that cannot.** It was. Breaking the recorded
transcript on purpose did not break the check, because the transcript was
full of carriage returns and the edit matched nothing.

**An interrupt that nothing quiets is a machine that stops responding, not
a machine that panics.** It was, in stage 2, exactly as predicted: the
machine wedged in the handler before printing anything.

**Two writers to one UART.** Settled with a rule in section 22.4 rather
than a lock, and the driver was the one that had to change, since the
kernel had always written a carriage return before a line feed and the
driver had not.

**A shell blocks on two things in sequence and must never block on both.**
It does, and it never wanted to.

**Every process is watched or the machine halts.** The rule got better
rather than being met: holding a handle to a process is what claims its
exit now, rather than a flag the kernel set on its own behalf.

*Size.* Medium to large, and spent. Eight stages, of which the two that
were not userland at all, the interrupt controllers and user mode taking
interrupts, were the ones the plan moved earliest and the ones that cost
least in the end.

**Done.** Section 6 says what it answered, what it cost, and what it left
untested.
### M4. The MMU

**Tier 2: the memory contract enforced rather than trusted.**

This is the thesis. Until Tier 2 exists, "the same interface with or without an
MMU" is a claim about one tier, which is not a claim at all. It comes after M3
so that there is real software to run on both.

*Needs.* Page tables and TLB maintenance in the port layer (section 27). The
move contract enforced by unmapping (6.1). The fault taxonomy and the fatal-by-
default rule (16). Faults as messages the faulting thread sends (15.3). A root
pager, demand-paged program text, and memory locking on the paging path (15).

*Five things it needs that were not listed.* Each is a stage rather than a
detail, and two of them are most of the milestone.

**One of the two ports has no MMU in the mode its kernel runs in.** The
RISC-V kernel runs in machine mode, where `satp` does nothing and the only
protection is the PMP. Tier 2 there means moving the kernel to supervisor
mode: delegating the traps it wants, moving to the supervisor trap
registers, finding a timer that supervisor mode can program, and leaving
the PMP open behind it. That is a second bring-up of a port that already
works, and it is the riskiest thing in the milestone, so it goes early and
alone. The AArch64 side needs no equivalent, because EL1 has had a full
MMU available since the day it booted and simply has not used it.

**There is no page allocator.** Section 8's pools allocate objects of a
fixed type and a fixed count, and page tables are neither: how many a
system needs depends on how its address spaces are shaped. Frames for
tables, for process memory and for whatever a pager hands out all come
from the same place, and that place does not exist. It is the first
allocator in this kernel that is not a pool, and section 8's argument has
to be extended rather than repeated: what makes a frame allocator
acceptable is that its bound is the memory map rather than a guess.

**The kernel reads user memory in a dozen places.** Every operation reads
its argument structure from an address a program chose, every message
carries an inline payload the kernel copies, and spawn reads a namespace
array and a command line the same way. At Tier 0 a pointer is a pointer.
At Tier 2 the kernel needs both permission to read a user page, which is
`SUM` on one port and `PAN` on the other, and a reason to believe the
address belongs to the caller. Section 23.2 says validation happens once
before execution, and this is the milestone where it has something to
validate against.

**The startup record is kernel memory that a program reads.** The record
of section 20.4 and the namespace table live in the process object, and
the child reads them through a pointer the kernel puts in a register. That
works only because there is one address space. At Tier 2 the loader has to
place the record, the table and the argument strings in the child's own
memory, which is what section 20.4 described in the first place and what
Tier 0 was allowed to skip.

**The console driver's registers are a constant compiled into it.** That
was named as a deviation in M3 with this milestone against it, and the
deviation ends here rather than later: an address space the driver did not
ask for does not contain its UART, so the first thing it writes faults.
Section 19.2's grant stops being deferrable at the moment a process has an
address space at all.

*Stages.* Eight. The port work comes first for the reason it did in M3,
which is that it is the only part that can fail for reasons outside this
tree, and the thesis check is as early as the machinery allows rather than
at the end, because a milestone should reach the thing it exists to
answer before it spends time on the thing it merely needs.

**1. A page allocator, and a tier to build for.** *Built.* Frames, and
the switch that says which tier an image is.

- A bitmap allocator over the memory the port hands across, with
  allocation, release and a zeroing allocation, since a page table with
  anything in it is a page table with somebody else's memory in it.
- `SYS_TIER` as a build-time choice. Tier 0 builds exactly what M3 built,
  which is what makes every check that follows a comparison rather than a
  hope.
- The bound is the memory map and not a constant, which is the one place
  this departs from section 8 and the departure is worth stating in the
  section rather than in a commit message.

*Check.* The allocator's invariants: what it hands out is page-aligned,
inside the map, distinct, and zeroed. A frame released is the next one
out. And the last frame the port claims is written and read back, which
is the only part about the machine rather than the code: section 27
lets a port state its memory rather than discover it, and a statement
can be wrong. Both boards report 32768 frames of 128 MiB, and the last
one is memory.

A Tier 0 image still passes `make check` on both architectures, which
is what every later stage is measured against. A Tier 2 image builds,
boots, announces its tier and panics naming the stage that will make
it true, because an image claiming enforcement it does not have would
be worse than one that refuses to run.

Three things this settled. **The check was written against the
implementation rather than the contract**, and the implementation was
not worth defending. A rotating cursor made a released frame not the
next one out, so the only property worth checking was observable only
at exhaustion. The cursor is gone: the allocator takes the lowest free
frame, which section 7.3 refuses for handles and for a reason that
does not apply here, since a frame number is not a name a program
sees.

**The free map contained every running program**, which reviewing it
found and no invariant of the allocator's own would have. A loaded
image sits at the address it was linked for, and nothing told the
allocator: a page table would eventually have landed in a running
program's text, or the next spawn would have written over a page
table, and the first symptom would have been neither. The slots are
held back now, walked from the boot archive so that adding a program
maintains it, and the span held is the slot rather than the image
because a program's zeroed data and its stack are inside the slot and
outside the image. Removing the reservation fails the check by name.

Two smaller repairs went with it. The check that the port's memory
claim is true wrote to a frame it had not allocated, which is the
thing this system exists to prevent, so reserving a frame by address
is now an operation the allocator offers and the check uses it. And a
build variable was expanded before it was defined, which in make is
not an error: the kernel was compiled to hold back slots of zero
bytes.

**Section 8 has an exception now, and says so.** The frame allocator is
the only allocator here that is not a pool. What keeps it inside the
argument is where its bound comes from: a pool's size is a number
somebody chose, and this one's is the memory the machine has.

**2. RISC-V in supervisor mode, with the MMU still off.** *Built.* The
port change, alone, changing nothing above it.

- Machine mode delegates the exceptions and interrupts the kernel wants
  through `medeleg` and `mideleg`, opens the PMP for supervisor mode, and
  drops to S-mode with everything else unchanged.
- The trap path moves to `stvec`, `sepc`, `scause`, `sscratch` and `sret`.
  The mode bit that told a user trap from a kernel one moves with it.
- A timer supervisor mode can program. `stimecmp` is the direct answer and
  QEMU's `virt` provides it; a part without it needs machine mode to take
  the timer and forward it, and which one this port does is a fact worth
  recording rather than an implementation detail.
- Interrupts arrive as supervisor external interrupts through the PLIC's
  supervisor context rather than its machine one, which is a different
  context number and nothing else.

*Check.* Everything M3 checks passes on riscv64 with the kernel in
supervisor mode, and `make check` produces the same transcript byte for
byte as the machine-mode kernel did. That is the whole claim of the
stage: the same system in a weaker mode. The AArch64 image is untouched
and passes too.

Three things this settled. **The mode a kernel runs in is invisible until
something touches a register the mode does not have.** Two instructions
were left behind, both writes to `mstatus`, and each showed up as an
illegal instruction at a program counter a long way from the decision
that caused it: one in the thread trampoline, enabling interrupts for a
thread that had never run, and one in `enter_user`. The boot log says
which mode the kernel is in now, because a build that quietly went back
would otherwise look identical.

**Machine mode had nowhere to put a trap, and neither did the invariant
the trap path depends on.** Nothing expects a machine-mode trap after
the handover, because everything delegatable is delegated and no
machine interrupt is ever enabled, but the boot window before it is a
different matter: `menvcfg` is the newest register this port writes,
and a part without it traps to an `mtvec` nobody set, which is address
zero. The machine stops with nothing printed, which is how this was
found and what the check now shows. There is a machine-mode handler:
it writes a line through the polled UART of section 27 a byte at a
time, because a formatter is more than it is entitled to assume, and
then stops. And `sscratch` is zeroed explicitly rather than trusted to
arrive that way, since the whole kernel-versus-user distinction on this
port is that register being zero in the kernel.

**Sstc is what makes this affordable.** With `stimecmp`, supervisor
mode programs its own timer and machine mode is not involved in a timer
interrupt at all. Without it, machine mode would need a trap handler
whose only job is forwarding, which is the one piece of machine-mode
code this port would otherwise have to keep. That it exists on this
board was checked before the plan was written rather than after.

**3. The kernel's own address space.** *Built.* Translation on, with the
kernel running through it.

- Page tables built for the kernel: its image, the frames it allocates,
  and the device registers it uses for the console and the controllers.
- Enabling translation is the sharpest edge in the milestone, because the
  instruction after the one that enables it is fetched through the tables
  it just installed. Both ports need the sequence written down rather than
  arrived at.
- TLB maintenance in the port layer, per section 27: invalidate a range,
  invalidate an address space, and the barriers each needs around it.

*Check.* All four configurations pass `make check`: two architectures
by two tiers. The Tier 2 transcript is identical to the Tier 0 one,
which is the claim of section 2.1 holding for the first time on an
enforced tier, even though nothing is enforced yet.

Nothing is protected at this stage, deliberately. Memory is mapped at
the address it already had, the kernel's writable half is reachable
from user mode because a program's startup record is still inside it,
and the console's registers are reachable from user mode because its
driver holds them as a compiled-in address. Stage 4 takes the first
away and stage 7 the second. Passing through this state deliberately
separates "the tables are right" from "the permissions are right",
and each is a different way to be wrong.

Three things this settled, all found by the machine stopping with
nothing printed. **A page user mode can write is a page the kernel
will not execute**, on AArch64, whatever the execute-never bits say.
The first version of this stage mapped all of memory as everything to
everyone, and the instruction after the one that enabled translation
took a permission fault. The map is four ranges now, split at the
boundary between what the kernel executes and what it writes, which
the linker scripts had to be taught to mark.

**Supervisor mode may not touch a user page unless it says so.** On
RISC-V the kernel's own stack is inside the range marked for user
mode, so without `SUM` the kernel faulted on its stack, then faulted
saving the frame for that fault, and so on. It is a permission the
kernel gives itself and not a check it skips: what says an address
belongs to the caller is section 23.2's validation, which is stage 4.

**A level of page table arithmetic written the wrong way round looks
exactly like everything else that goes wrong here.** The Sv39 root
index was a level too high, which put every mapping in root entry
zero. All three of these produced the same symptom, a machine that
stops before it can say anything, and all three were found the same
way: by asking QEMU for its exception trace rather than by reasoning
about the tables.

A fourth was found by the session running to the end and faulting on
the instruction that switches the machine off, because the register
that does it is the one device nobody uses until then.

*And one thing this stage was not expected to show.* Reviewing it
asked whether the permissions are real or only written down, by having
a program read the kernel's first instruction. At Tier 0 it reads
`d53800a0` and carries on. At Tier 2, the same program from the same
source takes a permission fault at the address it touched. That is
section 2.1's claim in miniature, two stages before the plan expected
any evidence of it, and the only reason it is not a check yet is that
a fault still stops the machine rather than the thread. Stage 5 is
where it becomes one.

**4. An address space per process, and a loader that fills it.** *Built.*
The stage that makes a process a process.

- A table per process, the kernel mapped in the half a program cannot
  reach, and a switch on dispatch with the maintenance that needs.
- The image mapped rather than copied to an address chosen at build time.
  The load slot of section 15.2 disappears at Tier 2, and with it the rule
  that one image has one live process: two shells become possible, which
  is the first thing this tier buys that Tier 0 cannot have.
- The startup record, the namespace table and the command line placed in
  the child's own memory, per section 20.4.
- The kernel permitted to read user memory, and required to check it: the
  validation of section 23.2, once, before an operation executes.

*Check.* Two live copies of one image at Tier 2, refused at Tier 0, from
one source and one set of programs. A descriptor pointing at an address
the caller does not own is refused rather than read, and a program that
passes one is still running afterwards. And `make check` passes at Tier 2
on both architectures, which is the milestone's first real claim: the M3
system, unchanged, on an enforced tier.

All four configurations pass. `trespass` names a buffer a megabyte away
from its own memory and one that runs off the end of its slot, is
refused both times with `SYS_ERR_FAULT`, and exits zero. Two processes
from `status` are live at once at Tier 2 and the second is refused with
`SYS_ERR_WOULDBLOCK` at Tier 0, from the same request.

**A rendezvous copies between two memories and only one of them is
mapped.** The message a sender parks is in the sender's memory and the
buffer a receiver reads into is in the receiver's, and at Tier 2 the
kernel is running in one address space at a time. Both ports faulted in
the same place, the kernel touching a program's address from the
kernel's own space. The message is staged in the thread object now:
copied out of the sender while the sender is current, copied into the
receiver when the receiver runs, and the same both ways for the reply.
It costs one copy of a small message each way and Tier 0 pays it too,
because a second path that only one tier takes is a path only one tier
checks.

**A buffer is not a message, and it does not fit in one.** The fs
protocol has the server write the bytes into the client's buffer at the
address the client named, which is a dereference of another process's
address. The pages are mapped into the receiver at the address the
sender gave, and taken back when it replies. Keeping the address is what
lets a server dereference what it was handed, and it is affordable
because each process's memory is a range no other process's is.

**The check the weaker tier was said to give up is partly still there.**
Section 23.2 says address validation vanishes at Tier 0 because there is
no address space to ask about. But where a program's memory is is
something the kernel knows without asking the hardware: the loader put
it there. A buffer outside the caller's slot is refused on both tiers,
and removing the check makes `trespass` exit 4 on both. What Tier 0
still cannot check is anything inside the slot, and that is a real
limit rather than a missing check.

**A buffer that is never given back leaks a hole in an address space,
and nothing would have noticed.** The take-back at the reply is asserted
rather than assumed: a client that still has one lent out when it sends
again halts the machine. A client is blocked from its send until its
reply and cannot ask twice, so a record still there is a reply that did
not give the memory back. Deleting the take-back trips it on the file
client's second request, which is the only reason the assertion is worth
its two lines.

*What this stage did not settle.* The sender keeps its own mapping while
the receiver holds the buffer, so the move contract of section 6.1 is
still a convention on both tiers rather than an enforcement on one. That
is stage 6, and it is the milestone's thesis.

*Reviewing it found seven defects, six of them in what this stage
wrote.*

**A reply that failed on the way across arrived empty.** Staging the
reply meant deciding when to copy it out, and the first version copied
it only when the reply reported success. A handle that fails to move is
a reply with an error in it and a payload the server still wrote, and
before this stage the payload was written straight into the client and
arrived regardless. Nothing in the system noticed the change: there was
no check anywhere for a reply that half fails. There is one now, and it
catches the old behaviour.

**Kernel memory cannot be lent, and pretending it could made a program
fault.** The hand-over skipped kernel addresses on the grounds that the
kernel is in every address space already. It is, without the bit that
lets user mode reach it, so the file server took a permission fault on
the first byte it wrote into a buffer the kernel had passed it. The rule
is now that a buffer must be inside the sender's own image, and a
process the kernel built may not send one at all. Both tiers refuse,
because a rule that holds only where the hardware happens to allow it is
the kind of difference section 2.1 exists to keep out of the interface.
Lending a program a page of kernel memory is section 19.2's grant, and a
grant is a handle rather than an address.

**Two copies of one image occupy the same addresses, which is the point
of them.** A buffer sent from one to the other names a page the receiver
already has, and mapping it at the address it came with would have
replaced the receiver's own memory with the sender's and then unmapped
it at the reply. Refused now, with the limit written into section 6.1
next to the claim that made it: keeping the address is affordable
because processes do not share ranges, and the same stage that made
buffers cheap made two processes that do.

**The access flags in a message were ignored.** Section 6.1 has them say
what the receiver may do, and the mapping was read-write whatever they
said. A client asking for a read-only buffer got a server that could
write it. They are the permissions on the mapping now: with the flag
changed to read-only, the file server takes a permission fault on the
store, at the address the client named.

**A reply may come from a thread that did not receive.** The record of
what was lent was on the receiving thread, so a reply from any other
thread of the same process would have found nothing to give back and
left the pages mapped for good. It is on the client now, which is the
thread whose memory it is and the one every reply names.

**A refused send counted as a rendezvous.** The counter section 25.7
compares round trips with was incremented on entry, so a send that never
reached a receiver was measured as one that did.

*And one that was not this stage's.* A `check` that compared the whole
transcript compared the resource report with it, and the report is not
the session: Tier 2 runs two processes where Tier 0 refuses the second.
The session is one file for both tiers, which is the claim; the report
is one file per tier, which keeps an exact count of what each run used.

*Left alone, with reasons.* A spawn that fails after its address space
exists leaks the frames in it, because nothing tears an address space
down at all yet and building teardown for the failure path alone would
be half of M7 done in the wrong place. It is reachable only by running
out of frames, which is the condition it makes worse. A server that dies
holding a lent buffer leaves it mapped, which is section 24.3's. And one
unexplained fault was seen once, in a program executing a kernel address
during the spawn checks, in a build that had a known fault elsewhere; it
did not recur in fourteen boots of the fixed build and nothing more is
claimed about it than that.

**5. Faults, and death by default.** *Built.* What the hardware raises,
and what happens to the thread that raised it.

- Every exception each port can raise mapped to one of section 16.1's
  kinds, with the rows that cannot happen saying so rather than being
  absent.
- A fault terminates the faulting thread, per 16.2, with the kind, the
  address and the program counter in its exit status. That is the path
  exit already built, which is the point: a fault is an exit with a reason.
- `sys_thread_status` reports it, so a supervisor or a shell can say what
  happened rather than that something did.

*Check.* A program that dereferences a null pointer dies with
`SYS_FAULT_ACCESS`, the address it touched and the instruction that
touched it, while the shell that started it carries on and reports it.
The same program at Tier 0 does not fault at all, which is section 6.1's
whole argument visible in two runs of one image.

The address is the kernel's first instruction rather than null, which is
the same claim made better. Null is unmapped at Tier 2 and is also
nothing at Tier 0, where the bus would answer for it and the program
might die anyway; the kernel's first instruction is memory that
definitely exists and definitely is not the program's. So the two runs
differ for the reason the stage is about, and not because one address
happens to be special.

`touch_addr` takes the address on its command line and returns the word
it read. At Tier 0 it reads `0xd53800a0` on one port and `0x20297` on
the other, which is each kernel's first instruction, and exits with it.
At Tier 2 the same image at the same instruction dies of an access fault
at that address, and the check confirms the reported instruction is
inside the program rather than inside the kernel that reported it.
Stage 3's review saw the first half of this and could not check it,
because a fault stopped the machine then.

From a shell, which is where a person sees it:

```
$ touch_addr 40080000                         (Tier 0)
touch_addr: 0xd53800a0 at 0x40080000
[-717750112]

$ touch_addr 40080000                         (Tier 2)
sh: touch_addr: access fault at 0x40080000, pc 0x40f000f4
[-16]
```

**The kernel says nothing when a program faults.** The first version
printed a line, which read well until the shell printed one too. Whoever
holds the process is what reports a death, because reporting needs a
name for the program and somewhere to put it, and a death nobody holds
already halts the machine with the fault named. That leaves exactly one
report per fault and no case where there is none.

**A fault is an exit, and that is all the mechanism there is.** The
completion timeline advances, a joiner is released, donated priority
unwinds, the process finishes when its last thread does. `thread_fault`
records three fields and calls `thread_exit`. Section 16.2 says exit and
fault share a path; sharing it turned out to mean writing nothing.

*What the mapping settled.* Both ports enumerate every exception their
hardware defines rather than the ones that arrive, because a row that is
absent is a fault that lands on whatever the default happens to be.
Three rows are there to say they cannot happen: `SYS_FAULT_PAGE`, which
no port raises because nothing is ever absent from a table until there
is a pager to make one so; `SYS_FAULT_ARITH` on RISC-V, where division
by zero has a defined result and no floating point traps are enabled;
and the ecall-from-supervisor code, which cannot arrive because the
machine-mode window closed at boot and nothing is above the kernel.

*Left for later, deliberately.* The handler of section 16.3 is not here:
a process may not nominate a fault channel, and every fault is fatal.
That is the opt-in half of the section and it needs the pager shape of
15.3, which is stage 8. `SYS_FAULT_EXTERNAL` is mapped on both ports and
cannot be provoked on either board, so the row is written and untested.

*Reviewing it found two defects and one hole the stage exposed rather
than made.*

**A program could stop the machine with a number.** The kernel
dereferences the descriptor array a program passes, the `arg_ptr` inside
each descriptor, the namespace and argument vectors a spawn points at,
and each string in the argument vector. None of them was checked.
Section 23.2 has said since M1 that validation happens once before
execution, and stage 4 built the buffer half of it and nothing else; what
stage 5 added was the ability to see what happened, which is a panic
naming the kernel's own instruction and a program's address.

It is closed now, in the two places the descriptors pass through and in
the three places a spawn follows a pointer. The string case needed more
than a yes: the copier stopped after two hundred and fifty-six bytes,
which bounds the damage rather than the read, so the kernel is told how
far the caller's memory goes and stops there. `trespass` submits a
descriptor array a megabyte away from its own memory and a descriptor of
its own pointing at arguments that are not, is refused both times with
`SYS_ERR_FAULT`, and exits zero. Removing either check reproduces the
panic.

The predicate this needs is not the one stage 4 built. What a program may
*name* is its own image; what it may *lend* is narrower, because lending
needs memory that can be mapped somewhere else, and a process the kernel
built may name anything and lend nothing. Two questions, two functions,
and the version with one of them had the kernel refusing to read its own
memory.

**Two of the eight abort groups were missing from the AArch64 mapping.**
The classes are grouped by masking off the level bits, which is compact
and is exactly how a group goes missing: a synchronous external abort
taken on a translation table walk, and the parity error beside it, fell
through to the default and were reported as access faults. A program
blamed for a bus that failed under the kernel's own page tables.

The mapping is a pure function of a cause register, so it can be checked
without making the hardware raise anything, and now is: each port asks
about every encoding by name and level at boot, and sweeps the whole
cause space to confirm nothing maps to "did not fault". The sweep also
confirms the row that says RISC-V cannot raise `SYS_FAULT_ARITH`. Putting
the missing group back and running it reports exactly the line that was
wrong. This is the check the stage should have had, because "exhaustive"
was a claim about a file nobody could run.

**And a claim in the interface that was not true.** `SYS_EXIT_FAULT` was
described as a value no program returns. A program returns whatever it
likes, and the one this stage added returns a word it read out of memory.
What tells a fault from a choice is the fault kind beside the code, which
is what the field is for.

*Left alone, with reasons.* A fault in a thread that is not the last of
its process is untested: section 16.2 says the process survives, and it
does, but every process here has one thread. The path is the one a normal
thread exit takes and `threads` exercises that. The fault fields on a
thread's own status are written and cannot be read from another process,
because nothing hands out a handle to somebody else's thread; the process
status is what a supervisor reads and it is checked.

**6. The move contract, enforced.** *Built.* The thesis, and the earliest
point the machinery allows it.

- A message buffer unmapped from the sender and mapped into the receiver
  for the duration of the call, and the reverse at the reply.
- The address the receiver sees is its own, which the kernel already
  rewrites at the rendezvous because Tier 0 needed the field to exist.
- Nothing in the interface changes. If anything has to, section 2.1 is
  wrong and this is where that is discovered.

*Check.* The milestone's own clause: a program that touches its buffer
between send and reply faults at Tier 2 and does not at Tier 0, built from
one source, with the fault carrying the address it touched. And every
program written for Tier 0 in M1 through M3 runs unchanged at Tier 2,
which is the other half of the same claim and the half that is easy to
forget to check.

Both halves hold. `grabber` reports 1 at Tier 2 and 0 at Tier 0 from one
source, and the fault it reports carries the buffer's own address; all
four configurations pass `make check`, which is the second half and is
the one that would have failed if the enforcement had broken anything.

**Nothing in the interface changed.** Not a field, not an error, not a
call. The three lines that make the difference are an unmap in the
hand-over and a lookup and a map in the take-back, and every program
that ran before runs now. That is section 2.1's claim discharged, and it
is worth saying plainly that it could have gone the other way: a system
where enforcement needed a new call would be a system with two
interfaces and a paragraph explaining why.

**One thread cannot break the contract.** The thread that sent the
message is blocked until the reply and has no instruction to execute in
between, so the rule binds a process and not a thread, and breaking it
takes a second thread running while the first waits. That is why every
program written before threads existed keeps the contract by
construction, and why demonstrating a violation needed a program written
to do it.

**The window has to be opened deliberately, or the check passes for the
wrong reason.** The first version had the second thread race for the
window and write as soon as it was scheduled; the write landed before
the send about half the time, and a run where it lands early looks
exactly like a run where the contract was kept. The second thread now
waits on a timeline the server opens after taking the message. The
server is the kernel's own boot thread for the same reason: the file
server would have replied when it felt like it, and the window has to
close after the write rather than before.

**A signal readies a thread and does not run it.** Opening the gate made
the second thread runnable and left this one holding the core, so the
reply went out, the buffer came back, and the thread that was supposed
to touch it ran afterwards and found its own memory where it left it.
That version passed on both tiers and proved nothing, which is the exact
failure the paragraph above describes, found twice in one stage by two
different routes. A yield after the signal is enough and is exact: the
server is the lowest priority thread in the system, so yielding runs the
other one to its end.

**Taking a page back does not need to remember where it came from.** The
receiver's own tables say what its addresses name, and those tables are
the thing being undone. Remembering instead would be a list per thread
sized for the largest buffer anyone might ever send, in a system whose
whole memory policy is that nothing is sized for what might happen.

*What is still not enforced.* A grant, which is the other form memory
crosses a boundary in and the one that outlives a call. Section 19.2's
driver still holds its registers as an address compiled into it, mapped
into every space because otherwise it could not run. That is stage 7.

*Reviewing it found two defects in the kernel and one in the program
written for the stage.*

**A buffer that granted no access was a page table.** The flags in a
message say what the receiver may do with the memory, and nothing
refused a message that said nothing. On AArch64 that became a page
mapped read-only, which is more than was granted and no worse. On
RISC-V the encoding for a leaf with no read, write or execute is the
encoding for a pointer to the next level of the page table, so the
hardware walked into a page whose contents the sender had chosen. What
it did with what it found there was to kill the file server, which
nothing was waiting for, which stopped the machine. A program could do
that with four assignments and a send.

Refused now, at both tiers, before anything is mapped. `trespass` sends
one and is refused with `SYS_ERR_INVALID_ARG`; with the refusal removed
the file server dies again, on the port where the encoding means
something else.

**A page already lent said the wrong thing.** Two threads of one process
can each send a buffer, and if the buffers share a page the second send
finds it gone. The error was `SYS_ERR_INVALID_ARG`, which came from the
page table lookup failing and said nothing true: the argument is fine
and the memory is not the sender's at that moment. It is `SYS_ERR_FAULT`
now. This is reasoned rather than checked, because demonstrating it
needs a program with two threads that send, and what it would prove is
the number in an error rather than a behaviour.

**And an invariant is written down where it will be needed.** A sender
runs again only when a reply has given its buffer back, because a reply
is the only thing that releases it: sends carry no deadline and
cancellation does not exist. When cancellation does exist, a client
released by it would run with a hole where its memory was, so the
assertion sits at the point of waking and says so.

*One defect was mine and not the kernel's, and it is worth recording
because the machine found it.* `grabber` passed the top of its thread's
stack where the library wanted the bottom, so the library computed a
region past the end of the array and put the new thread's stack inside
the message buffer. The move then took the buffer away with the stack in
it, and the kernel faulted writing a status to a descriptor on a stack
that was no longer there. The mistake is silent by construction: the
library cannot tell a base from a top, and the array it ran past was
whatever the linker put next. Nothing but enforcement would have found
it, and it went unnoticed until the stack grew large enough to land
somewhere that mattered. The comment on `sys_thread_create` now says
which end it wants and what happens when it is given the other.

*And one thing named rather than fixed.* Enforcement is by the page and
the contract is by the buffer. A buffer sharing a page with anything
else takes that too, so another thread of the sender touching an
unrelated variable on the same page faults. Section 6.1 says this now: a
sender with more than one thread puts its buffers on pages of their own,
which is the one place a program has to know that pages exist.

**7. Grants, and the driver that needs one.** *Built.* Section 19.2, no
longer deferrable.

- The grant object of section 7, covering a region with rights, handed
  over at spawn like any other handle.
- The console driver receives its registers as a grant rather than as a
  constant, and the launcher is what holds the authority to make one.
- Device memory mapped with device attributes rather than normal ones,
  which is a port detail that produces silent corruption when it is wrong
  rather than a fault.

*Check.* The console works at Tier 2, which means the shell session does.
Starting the driver without its grant faults on the first register it
touches, at the address of the register, rather than producing a console
that quietly does nothing.

All four configurations pass, which is the first half: the console driver
holds no address of its own, asks where its grant is, and the shell
session is what comes out. The second half is a program reading the
register page while holding nothing, which dies of an access fault at
that address; and a driver spawned with no grant at all, which stops at
the lookup and says which handle it was missing.

**Nothing can mint a grant.** There is no operation that turns a physical
address into authority, because that operation is ambient authority over
the machine: whoever could call it could reach any device. What
assembles the system makes them, which here is the kernel at boot, and
section 19.2's launcher is what that becomes when there is a root
process.

**A grant is asked for rather than delivered.** The handle arrives at
spawn like every other; the address is the answer to a question the
holder puts. A holder that never maps one pays nothing for holding it,
which is section 18.4's lazy mapping without needing a fault to make it
lazy, and the address belongs in an answer rather than in a table whose
shape a program has to know.

**A block is what a grant cannot refine.** The first attempt mapped the
console's registers into the driver and panicked descending into a
block: the kernel had mapped the same device as two megabytes because
that was the size the identity map liked, and a grant covering one page
of it has nowhere to write. The device regions are the size of the
devices now, which is one page for each console and unchanged for the
two interrupt controllers, and the cost is a table frame per address
space for each region that was a block.

**The deviation three stages carried is gone.** Since stage 3 the
console's registers were mapped into every process with user access,
because the driver held the address as a constant and would have faulted
without it. Both ports said so in a comment naming this stage. They are
kernel-only now, in every space but the driver's, where the grant is
what puts them.

**The rights on the handle are the permissions on the mapping.** A grant
given `SYS_RIGHT_READ` alone is memory that cannot be written, and
`SYS_RIGHT_MAP` is what permits mapping it at all. It needs no
permission field of its own, which is the same arrangement the buffer
flags of section 6.1 have for the shorter-lived form.

*Two things this did not do.* A grant is mapped when it is asked for and
never unmapped, because nothing closes a handle to one yet and nothing
reclaims an address space at all. And the check at Tier 0 does not read
the register: it would succeed, which is the point, but succeeding means
a program reading a live serial port to make an argument about
permission. Which register is safe to read differs by board, one of them
is a byte wide, and reading the wrong one consumes an input byte or
acknowledges an interrupt. The tier with nothing to enforce is the tier
where the demonstration costs something and shows nothing.

*Reviewing it found one defect in the model and one limit that would
have stopped the next stage.*

**A grant's authority outlived its handle.** Section 7 makes the handle
the authority, and mapping a grant left the region reachable for good: a
driver could close the handle to its own device and keep writing to it.
Nothing exercised it, because the only grant was one the driver never
gives up, but a program can call `sys_handle_close` on anything it
holds.

Closing a grant handle now takes the mapping back, and takes it back
rather than removing it: the region reverts to what every other address
space has, which is the kernel's own mapping without the bit that lets
user mode in. Removing it would be worse than leaving it. A grant covers
memory the kernel maps too, so clearing the entry would take the console
away from the kernel in that process's space, which the kernel finds out
about the next time it prints.

Demonstrating it needed a grant that is not a device. A register cannot
be read twice without meaning something and which register is safe
differs by board, which is why the stage's own check would not read one.
A page of ordinary memory is the same object with none of that: the
kernel allocates one, describes it, hands it over, and `granted` writes
a word into memory it did not have, closes the handle, and reads the
same address again. At Tier 0 it reads it. At Tier 2 it dies there, and
the kernel confirms the word it wrote by reading the frame through its
physical address, so the check cannot pass on a program that touched
nothing.

**A mapping made of blocks could not be refined.** The stage found half
of this and fixed the wrong half. The console's registers were mapped as
two megabytes because that was the size the identity map liked, and a
grant over one page of it panicked descending into a block; making the
device regions the size of the devices fixed that case and hid the rule.
The rule is that all of ordinary memory is mapped in blocks too, so a
grant over a page of RAM panicked in exactly the same place, and so
would every page a pager supplies in stage 8.

The port splits a block into the table it stands for now: the same
memory, the same attributes, one page at a time. Turning the split off
does not merely refuse the grant, it faults the kernel on the next
access to the page, which is what the block was describing for everyone
else in that space.

*What remains.* Nothing records which process mapped which grant, so
closing a handle puts the region back whether or not this process ever
mapped it, and two grants over overlapping regions would undo each
other. Both need a per-process record of what a grant did, which is the
same bookkeeping address-space teardown needs and belongs with it. And
the process pool is at thirty of thirty-two at Tier 2, because nothing
is reaped and every check that spawns something spends one for the rest
of the run.

**8. Faults as messages, and text that arrives when it is touched.**
*Built.* Section 15.3 and the pager chain of 15.4.

- A pager as a channel handle, inherited at spawn, and a fault that is a
  message the faulting thread sends. The reply carries a grant and the
  thread retries.
- Priority inheritance across it, which needs nothing added: the faulting
  thread is an ordinary blocked sender and donates to its pager exactly as
  a client donates to a server. That property is the reason for the
  formulation, so the check has to show it rather than assume it.
- A root pager that terminates the chain, memory locking for everything on
  the paging path, and a memory-locked process that faults being fatal.

*Check.* A program whose text is demand-paged runs, with the pager
supplying pages one fault at a time and the count of faults visible. A
high-priority thread's fault is serviced at its priority while a
low-priority thread is runnable, which is the property that would
otherwise be an assertion. And a memory-locked process that faults halts
the machine, named, rather than deadlocking.

`paged` adds up the bytes of its own image and returns the sum. Its text
arrives in five pages, one fault at a time, and the sum matches the one
computed here from the archive, so a pager that answered with blank
pages would fail even though the program ran to the end. The same
program on the same tier with the same pager runs without a single fault
at Tier 0, because a flat address space has nothing absent in it, and
adds up to the same number.

**Priority inheritance needed nothing added, and the check had to show
that rather than say it.** The pager runs at 5 and the program at 20,
with a thread at 10 runnable throughout. The pager records the priority
it is running at while it serves and it is 20, and it records that the
middle thread was queued at the time, so the number is a fact about
scheduling and not a field somebody set. Removing the donation does not
merely change that number: the system stops earlier, in a check three
stages older.

**A fault is the only thing in this kernel that returns from a trap.**
Everything else that reaches the fault path ends a thread and never
comes back, which is why neither port had a way to resume one: the
AArch64 entry built a frame and branched to a handler that could not
return into it. Both ports now return to the instruction that faulted,
with sepc and elr untouched, which is the retry section 15.3 describes
and is three lines of assembly on each side.

**A page arrives at the address that faulted, not at the address the
grant covers.** That is the one place in this system where a grant is
not mapped at its own address, and it is the difference between what a
grant says and what a fault says: the page is the pager's and the hole
is the faulting process's.

*Three things this stage found that were not its own.*

**The two ports disagreed about what a page fault is.** One of them
separates an address nothing describes from one described without the
right permission; the other has a single cause code and says so. Pushing
the difference into the ports would have meant one of them reading page
tables, so the kernel does it instead, once, for both: an address the
tables resolve was present, so the fault was about the access. That is
also what keeps a pager from being asked about memory a program lent out
or was never given.

**The move contract took a page away by removing it.** With nothing
absent in the system that was indistinguishable from taking it away by
any other means. It is not now: a removed page is a question for a
pager, so a lent buffer is taken away the way a revoked grant is, by
leaving the entry and dropping the bit that lets user mode in.

**RISC-V small sections were in no output section at all.** The linker
scripts named `.bss` and `.data` and not `.sbss` and `.sdata`, which
RISC-V uses for anything reachable from the global pointer, so the
linker placed them as orphans wherever it liked. In the kernel that put
four hundred bytes of state outside the range that zeroes `.bss`, which
has never bitten because a cold machine hands over zeroed memory. In a
program it put `.bss` in the middle of the text, which is what the first
demand-paged run faulted on. Both scripts name them now.

*What is not here.* `sys_mem_lock()` is not an operation: a process is
locked by whatever builds it, because the right that would guard the
call belongs to a root task that does not exist. Rule 3 is enforced and
is demonstrated by hand rather than by a check, because what it does is
halt the machine: locking the demand-paged program and letting it start
stops the system with `paged is memory-locked and faulted at
0x41200000`, which is the rule saying what it is for.

*Reviewing it found three defects, one of them much older than the
stage and only visible because of it.*

**A page a pager supplied was executable whether or not it was granted
as such.** The mapping's permissions were assumed rather than read from
the grant, so every page that arrived came back readable and
executable. What a grant carries is what the mapping permits, everywhere
else in this system, and there is no reason for this path to be the
exception; the one subtraction is write, which section 15.1 does not
allow a pager to supply whatever the grant says. Handing the demand-paged
program its text without `SYS_RIGHT_EXEC` now kills it on the first
instruction it fetches, and did not before.

**A write fault was refused after the round trip rather than before it.**
Section 15.1 says a pager supplies read-only pages, so a fault that
wanted to write was never answerable, and asking anyway cost the pager a
frame and a grant and left the faulting process holding a handle to a
page nobody would map. Refused before the message now.

**And a grant carrying the right to map itself is refused on this path.**
A page supplied to a fault is mapped by the kernel at an address the
pager did not choose, so a holder that could map it too would hold a
handle meaning something different from what was done with it: closing
it would take back the region at the grant's own address and leave the
mapping at the faulting one.

*The old one.* Under a loaded host with a dozen machines running at
once, RISC-V Tier 2 stopped in the trap entry, building a frame across
the boundary of the user window, at the same address every time. The
kernel had taken a trap while `sscratch` held something that was not a
parked kernel stack.

`sscratch` is the whole of that port's kernel-versus-user distinction:
zero while the kernel runs, and the thread's kernel stack while a program
does. `enter_user` parks it and then leaves for user mode, and between
those two instructions the kernel is running with it already parked. An
interrupt landing in that window is taken as a kernel trap, and the entry
path clears `sscratch` unconditionally, while the kernel return path put
nothing back. The thread reached user mode with nothing parked, its next
trap built a kernel frame on the user stack, and from there the machine
was confusing one stack for another.

It has been there since programs first ran, which is M3 stage 3, and
nothing had made it likely enough to see. What stage 8 added was a
demand-paged program taking five faults in its first microseconds, a spinner
holding a middle priority, and a pager thread, which is enough more
scheduling that a two-instruction window starts being hit. The fix is
two instructions in the kernel return path, putting back what the entry
took; for an ordinary kernel trap it writes the zero that was already
there. Removing them and running fourteen machines at once reproduces
the original stop, at the original address.

*What this says about the check suite.* Nothing in it runs under load,
so nothing in it would have found this. The four configurations pass on
an idle machine and passed on an idle machine with the bug present. That
is not an argument for adding a load test to the suite, which would be
slow and flaky for reasons unrelated to correctness, but it is an
argument for running one deliberately when a stage changes how often
threads switch, and this stage did.

*Done when* the M3 system runs on Tier 2 from the same source, and a program
that touches a granted buffer between send and reply faults on Tier 2 and does
not on Tier 0.

*Would falsify.* **The central claim of the whole design.** If the interface
has to differ between tiers, section 2.1 is wrong and Part I needs rewriting.
The evidence is the same programs, the same `make check` session, and one
build switch: a difference that requires a source change is the claim
failing, and a difference in what a violating program sees is the claim
holding.

Separately, whether modelling a fault as IPC from the faulting thread really
reuses the reply path, the blocking invariant and donation without extending
any of them, which is the single most valuable borrowed idea in the document.
It is answered by building it and finding out whether anything in sections
10, 12 and 21 needed a case added for it.

*Watch for.* **Enabling translation is a cliff.** The instruction after
the one that turns the MMU on is fetched through the tables it installed,
and a mistake there produces a machine that stops with nothing printed.
Both ports need the enable sequence and the identity mapping that carries
it written down before either is run.

**The kernel is the first thing that faults, not the last.** Every place
the kernel reads user memory becomes a place it can fault, and a fault
inside the kernel has no thread to kill. What is owed is a list of those
places and a decision for each, not a fix per crash.

**A tier that is only ever built one way is a tier nobody checks.** Four
configurations exist the moment this milestone starts: two architectures
by two tiers. `make check` has to run all four, or Tier 0 rots while Tier
2 is being built and the claim that one source serves both stops being
tested exactly when it starts mattering.

*Size.* Large, and larger than M3, and spent. Eight stages, of which the
one that made a process a process and the one that enforced the contract
were most of it. The two that were ports of things that already work,
supervisor mode and the kernel's own address space, cost less than
expected and produced the two hardest bugs in the milestone.

**Done.** Section 7 says what it answered, what it cost, and what it left
untested. The central claim holds: four configurations of one source,
one recorded session, and nothing in the interface that differs between
tiers.

### M5. Namespaces

**The mount table in libc, and namespace composition at spawn.**

*Needs.* Path resolution as ordinary library code walking a local table.
Handle-relative naming throughout (20.3). Named directory handles handed over
at launch and consumed like `envp` (20.4). Union directories.

*Five things it needs that were not listed.* The first is a decision the
tree has already made the other way, and the rest are consequences of
building four milestones without any naming at all.

**A named directory handle needs a name, and the startup record says
names are the wrong trade.** `sys/startup.h` argues against a
string-keyed table: a name buffer per entry and a comparison per lookup,
on a part where the whole record has to fit in a few hundred bytes. That
argument was right for what the table then was, which is a set of roles a
parent and child agreed on by number. It is wrong for what section 20.4
describes, which is the set of directory handles a process was given
*with names attached*, because a parent that cannot say where to mount a
handle cannot compose a namespace at all. The milestone re-decides it and
rewrites the comment, and the cost is bounded: eight entries of a short
name each, beside the two hundred and fifty-six bytes the record already
carries for a command line.

**The kernel gains another array a program points it at.** Section 23.2
was half-built for four milestones and M4 stage 5 found what that costs:
a program could stop the machine with a descriptor array nobody checked.
A mount array is the same shape and gets the same treatment from the
first line, including each name it points at, rather than after something
faults.

**There is no path syntax anywhere in the tree.** The protocol takes a
directory handle and one name, `cat motd` is not a path, and the file
server matches a single component against a flat table. Every part of
`/`, `..`, a prefix and a remainder is new, and half of it is in the
server rather than the library.

**The console is reached by role, not by name.** A program prints through
`SYS_NS_CONSOLE` because the library was handed a connection, and nothing
about that looks like a filesystem. Making `/dev/console` name it is what
turns section 25's claim into something a person can see: two servers
under one namespace, resolved by a library that knows neither of them.

**Two tiers, still.** The startup record is placed in the child's own
memory at Tier 2 and in the process object at Tier 0, and it now has to
carry strings in both. The four configurations are not optional: the
milestone that made them exist is the one before this.

*Stages.* Six. The library work is most of it, and the first stage is the
one that changes an interface the kernel writes.

**1. Named directory handles at spawn.** *Built.* The namespace entry
grows a name, and the library keeps a mount table rather than a lookup by
number.

- A bounded name per namespace entry, placed in the child's memory
  alongside the record and the command line, at both tiers.
- The kernel checking every pointer a spawn now follows, per 23.2, before
  it follows any of them.
- `ns_lookup` by role stays what it is; the mount table is the new thing
  beside it.

*Check.* A program prints the namespace it was launched with, names and
all, and the shell hands its children a different one from its own. A
program given no entries has an empty table, and a name that does not end
inside its slot is refused rather than truncated.

`namespace` prints what it holds and exits with the count of named
entries. Given a root at `/` and a console at `/dev/console` it finds
two; given nothing it finds none and says so. Both at both tiers, from
one image.

The shell half of that clause is checked by hand rather than by the
suite, and the clause is narrower than it was: the shell passes on what
it holds under the names it holds it under, which is `NS_SHARE`, and
*different* is stage 4's word rather than this stage's. Running
`namespace` from the shell prints `/dev/console` and `/`, which came from
the kernel through the shell, on both ports. It is not in the recorded
session because that session is stage 3's to change.

**The name is inside the entry rather than pointed at from it, and that
is the stage's one real decision.** The first version made it a `const
char *`, which is what a caller would write, and it was wrong twice over.
The kernel would have had another pointer to follow out of a caller's
memory, which is section 23.2's obligation again for no gain. And a new
pointer field in a structure fifteen call sites already build is fifteen
wild reads the day it is added, which is precisely what happened: the
machine stopped in the boot checks dereferencing a handle value. Sixteen
bytes inside the entry cannot point anywhere, and the check that survives
is the one that is still possible to fail: a name with no terminator in
its slot is refused rather than truncated, because half a mount point
resolves to something other than what the parent meant.

*Reviewing it found three defects, and one of them was in the record of
the stage rather than in the stage.*

**The roadmap claimed a check the stage had not made.** The clause was
copied from the plan and half of it, that the shell hands its children a
namespace, was not exercised anywhere: the shell passed handles with no
names at all, because the field was new and it had not been taught to
fill it. So the entry was true of the plan and false of the tree. The
shell names what it passes now, which is two lines and is what a parent
is for, and the clause says which half the suite checks and which was run
by hand.

**The library read a fixed block from a pointer the record gave it.** The
names arrive as one slot per table entry, and the copy took the whole
array's worth however many entries the record said there were. A loader
that wrote a shorter block is one this reads past the end of, and the
record says how many entries exist in exactly one place, so the copy is
bounded by that now.

**And a buffer that copied what was already private.** The operation
copies the caller's entry array before validating it, precisely so a
second thread cannot change what was checked, and the name is inside the
entry: it was already safe by the time it was read. A second buffer for
it was a hundred and twenty-eight bytes of kernel stack and a second
thing to keep in step with the first. What is left is the check, which
happens before anything moves like every other one here.

*What this stage did not do.* Nothing resolves anything. A name is a
string attached to a handle, `ns_lookup` still answers by role, and no
program can write a path. That is the next two stages, and keeping them
apart is deliberate: the record that carries a namespace and the resolver
that reads one fail in different ways.

**2. Lexical reduction, and the longest prefix that matches.** *Built.*
The resolver, with no server involved.

- `..` reduced lexically before any entry is selected, which section 20.3
  says is forced rather than convenient.
- Longest-prefix match against the table, and the remainder handed on.
- A path that reduces outside every entry matching nothing at all.

*Check.* `/dev/../motd` resolves to the handle mounted at `/` with the
remainder `motd`, and not to the device server, which could only refuse
it. `/..` and `../x` match nothing. The check is a unit test in a program,
because a resolver is the one thing here that can be checked without a
machine doing anything.

Eighteen cases, against a namespace whoever starts it chose, so the
answers are properties of the table rather than of the library.
Removing the reduction fails case five, `/dev/../motd`, which is section
20.3's own example. Matching by character rather than by component fails
case seven, `/development/x`, which is the off-by-one the entry warned
about. Dropping a `..` at the top instead of refusing it fails case
thirteen, `/..`.

**Resolution costs no system call, and the file is the argument.** Sixty
lines of string handling, no cache, no message and nothing asked of the
kernel. Section 20.2 claims this and the way to check it is to look: the
resolver includes `<ns.h>` and `<string.h>` and nothing else.

**Two entries at one name keep the parent's order.** The first attempt
let a later entry win a tie, which made the answer depend on where in the
table a name happened to sit, and it showed up immediately: a working
directory mounted at `/` beside a root mounted at `/` took every path
that should have gone to the root. Strictly longer replaces, equal keeps
the earlier, which is also what a union directory will mean when stage 5
gives one more than one handle.

**A working directory is a name, not a second kind of thing.** A relative
path is joined to whatever the entry with the working-directory role is
mounted at and then resolved like any other. A process without that entry
cannot write a relative path at all, and there is deliberately no
fallback: falling back to the shortest entry would reintroduce the
ambient root section 20.3 removed, by the back door and under another
name.

*Reviewing it found three defects, all of them one answer standing for
two questions.*

**A path too long to hold answered as a path that names nothing.** The
reduction failed the same way whether it ran out of buffer or climbed
above the top, and those are not the same event: one is the caller asking
for something this library cannot do, and the other is an ordinary result
in a system where a name is not authority. A caller that cannot tell them
apart reports the wrong one to whoever wrote the path. They are
`SYS_ERR_INVALID_ARG` and `SYS_ERR_NO_ENT` now.

**The empty path resolved to whatever was mounted highest.** Nothing in
the reduction objected to a path with no components in it, so `""`
canonicalised to `/` and reached the root. A name that names nothing
should not reach something, and it is refused.

**A remainder that did not fit left the caller holding a handle and an
uninitialised buffer.** The handle is resolved before the remainder is
copied, so a caller that checked one and not the other would send
whatever was on its stack as a path. The buffer is terminated before the
error returns.

None of the three could have been found by the eighteen cases in the
table, because all three are about what happens when a question has no
good answer, and a table of paths and the handles they reach only asks
questions that do. They are checked apart from it now, and each is
reproduced by putting the defect back.

*What this stage did not do.* Nothing opens anything. The resolver hands
back a handle and a remainder, which is what a message to a server
carries, and no program sends one yet.

**3. Open by path.** *Built.* The utilities stop naming a single
component against a single handle.

- An open in the library that resolves, walks and returns a handle.
- `cat` and the shell using it, with the fs protocol unchanged: a walk
  still carries a handle and a relative path, because that is what
  section 20.3 says the protocol always did.

*Check.* `cat /motd` prints the file. The recorded session changes here,
which is the point: the same session on all four configurations, with
paths in it.

It does, and the session now reads `cat /motd` and `cat /nothing` on
every configuration. Sending the whole path where the remainder belongs
fails it: the server has never seen a leading slash and says there is no
such file, which is the recorded session catching a library bug rather
than a kernel one for the first time.

**The protocol did not change and never had to.** A walk carries a
handle and a relative name because a message to a server cannot carry
anything else. What changed is where the name comes from, and the
division is the one section 20 asks for: the resolver knows nothing about
files and `file_open` knows nothing about namespaces.

*The number the falsification clause asked for.* `cat` was a hundred and
twenty-two lines and is seventy-three, of which the protocol is now none.
Its binary went from 6911 bytes to 8070, so the resolver and the file
layer together are about a kilobyte more than the hand-rolled walk they
replaced. `status`, which names nothing, is 3837 bytes before and after:
the library is an archive and a program that opens nothing links none of
this.

So the complexity did move into the library, and the honest summary is
that it moved once rather than per program, and that the programs which
do not want it do not carry it. Whether that is a good trade is a
question about the next three stages, where a mount table starts having
more than one thing in it.

**Two answers a person will meet immediately.** `cat motd` fails, because
the shell hands its children no working directory and section 20.3 has no
root to fall back on. `cat /` fails, because a path that lands exactly on
a mount point names a handle the process already holds. Both are correct
and neither is friendly, and the first is a composition decision the
shell has not made yet rather than a resolver limitation.

*Reviewing it found four defects, and two of them were the stage saying
something that was not so.*

**The entry claimed `cat /dev` fails because it is a mount point.** It is
not one. In the namespace the shell hands out, `/dev` matches the root
and fails because the root has no file of that name, and the sentence
described a namespace the tree does not have. What does land on a mount
point is `cat /`, and that is what the entry says now.

**The shell named the console as if it were a directory.** Section 20.4
calls a namespace an array of named *directory* handles, and the console
is not one: it is a service reached by role, and nothing serves walks for
it. Mounting it at `/dev/console` put a thing in the table that answers
no path below it, so `cat /dev/console` resolved onto it, found nothing
under it, and was told it was a directory. The console keeps its role and
loses its name, and naming it is what stage 5 is for, when something can
answer a walk on its behalf.

**The library refused a mount point for the wrong reason and said so.**
The refusal is right and the reason is ownership rather than type: the
handle is the process's own namespace entry, so handing it back would
give a caller something whose close would close the mount. A program that
opened its own root twice would lose it. `cat` says mount point now,
because that is what happened.

**And a copy into a fixed protocol field trusted its caller.** The name a
walk carries is twenty-four bytes and the bound lived in `file_open`
rather than beside the copy, which is correct exactly as long as every
caller stays correct. It is beside the copy. An open that succeeds and
hands back no handle is also refused now, rather than left to fail on the
first read where it would look like the file going away.

**4. Composition at spawn.** *Built.* What a parent hands a child.

- `NS_SHARE`, `NS_CLONE` and `NS_NEW` as a choice of what array to pass,
  which is what section 20.4 says they are.
- The shell doing it deliberately rather than passing what it holds.

*Check.* The milestone's own clause: two programs see different views of
the same underlying handles, from one shell, with no kernel involvement.
A program launched with `NS_NEW` can name nothing and says so.

The shell is handed one filesystem under two names, `/` and `/files`, so
that different views of the same handles is a fact rather than a
rearrangement of words. Two words in front of a command choose what it is
launched with, and the recorded session now shows all three:

```
$ namespace
namespace: id 3 has no name
namespace: / is id 1
namespace: /files is id 4096
$ only / namespace
    namespace exited with status 1
$ alone namespace
    namespace exited with status 0
$ cat /files/motd
served from outside the kernel
```

`cat /files/motd` and `cat /motd` are the same file down two paths, which
is the clause. Making `NS_CLONE` keep everything, or `NS_NEW` hand over
what the parent holds, fails the session.

**A program given nothing cannot say so, and that is the better answer.**
The clause expected it to report an empty namespace. It cannot: saying
anything needs a console, and a console is a namespace entry like any
other, so a process handed nothing has no way to tell anyone. What it has
is an exit status, which is how it says so and is what the check reads.
The wording was written before the thing existed and the thing is more
honest than the wording.

**Composition is a library function and not a mechanism.** Section 20.4
says these are choices about what array to pass, and the file is thirty
lines of loop: walk this process's own entries, keep the ones the caller
wants, duplicate the handles because spawn moves them. Nothing in the
kernel knows the three names exist. Neither does the shell's parser, in
any deeper sense than deciding which of three arrays to build.

**The rights are the caller's and not the library's.** The first version
duplicated with the right to duplicate, which quietly widened what the
shell hands out: the shell had been stripping that on purpose, so that a
child may use a connection and pass it on but not compose a namespace of
its own from it. A library that chooses attenuation for its callers is a
library that undoes their policy, so the rights are a parameter and the
shell still strips what it stripped.

*Reviewing it found two defects and one thing the shell was doing all
along that nothing had said out loud.*

**A namespace is not all one kind of thing, and the composer assumed it
was.** Section 7.2 refuses a duplication asking for a right the source
lacks rather than clamping it, deliberately, so a composer handing on
connection rights cannot compose an entry that is a grant. The whole
composition failed, which is arguable, and it reported running out of
memory, which is not: those are different events and one of them is a
lie. The error says which now, and the interface says that a namespace
holding two kinds of thing is composed in two calls.

That is a tension worth naming rather than a defect to fix. A generic
"hand on what I hold, attenuated" needs to know what each entry carries,
and nothing in the interface tells a program that. Adding a way to ask
would be a new operation nothing else needs, so what exists instead is a
caller that knows what it holds, which every caller here does.

**A child of the shell cannot compose a namespace at all, and now says
so.** The shell strips the right to duplicate from what it hands on, on
purpose and since long before this stage: a child may use a connection
and pass it on and may not make another. The consequence is that
`ns_compose` fails for such a child on its first entry, which is correct
and was invisible. It is in the recorded session now, which is the
attenuation being demonstrated rather than asserted.

**And the message guessed at a cause it could not know.** Two walls
answer with the same error, an entry of the wrong kind and an entry that
may not be duplicated, and the program named the first. It says what is
true of both.

*What `only` does that is worth knowing.* It keeps the entries whose
names were listed and nothing else, so `only / cat /motd` hands over a
root and no console, and the program runs and cannot print. That is what
the word means and it is not a defect, but it is the shape of every
mistake a person will make with it.

**5. Union directories.** *Built.* One prefix, several handles, in order.

- A table entry that names more than one handle, tried in order until one
  answers.
- A name present in both resolving to the first, which is the whole of
  the semantics.

*Check.* A directory that is two servers: a name only the second has is
found, a name both have comes from the first, and a name neither has
fails once rather than twice.

All three, in the recorded session:

```
$ cat /u/motd
served from outside the kernel
$ cat /u/notes
only the second directory has this
$ cat /u/absent
cat: /u/absent: no such file
```

`/u` is two entries with one name. The first half has `motd`, the second
has `motd` with different bytes in it and `notes` besides, so the first
line is the whole of union order: reversing the two entries changes what
`cat /u/motd` prints, and the session says so. Stopping after the first
candidate loses `notes`.

**What a union joins is directories, not servers.** The check says two
servers and the tree has one, serving two directories through connections
whose cookie says which. That is closer to what a union is and it costs
nothing to be accurate about: the library never learns how many servers
are behind the handles it was given, which is the point of them being
handles.

**Only entries at the same name are in the union.** An entry with a
shorter name is a different mount and not a fallback. Falling through to
one would let a name reach a place the parent deliberately mounted
something else over, which is the same mistake as falling back to the
shortest entry when nothing matches, and section 20.1's answer is the
same: a name is not authority, so a name that matches nothing here
reaches nothing.

**One failure comes back, not one per handle.** Which of the directories
did not have the file is the loop's business. A program told that a name
was missing from three places has learned about the namespace it was
given rather than about the name it asked for.

*What went wrong on the way.* The first version walked the table once,
taking the nth entry that matched at all, which quietly turned the
longest-prefix rule into a first-match rule: with `/` and `/dev` in the
table, `/dev/console` went to `/`. It is two passes now, one to find the
longest name that matches and one to count entries carrying it, and the
resolver's own eighteen cases caught it immediately.

*Reviewing it found one defect and confirmed two properties nobody had
asked about.*

**A union hid a broken half behind a missing file.** Every failure from a
candidate moved on to the next, and when none answered the caller was
told the name was not there. Those are different questions. A name in
none of three directories is missing; a name in none of two because the
third could not be asked is a namespace with something wrong in it, and
telling a person the first when the second happened sends them looking in
the wrong place. What comes back now is the first failure that was not a
directory saying it did not have the file, and `cat` says which of the
two it met.

Verified by hand rather than by the suite: a third half was added to the
union, a console connection, which answers a walk with
`SYS_ERR_NOT_SUPPORTED` because it is not a directory and never claimed
to be. With the distinction in place `cat /u/absent` says it cannot be
reached, and without it says there is no such file. It is not kept,
because keeping it means a deliberately misconfigured namespace in the
recorded session for the rest of the run, and the thing being
demonstrated is what happens when a person makes that mistake rather than
that the system can be made to make it.

*Two things that turned out to hold without being designed for.* A union
survives composition: `only /u` keeps both entries, because `NS_CLONE`
filters on the name and both halves have it, so a child gets the whole
union or none of it. And a working directory named the same as the root
is a union with it, which is what any two entries at one name are; the
handles are the same directory there, so nothing observable follows, but
it is worth knowing that the rule has no special case for the entry a
relative path is measured from.

*And one thing the check's wording could be read as claiming.* "Fails
once rather than twice" is about what the caller is told, not about what
is sent. A union with two halves that do not have the file sends two
walks, because asking is the only way to find out, and the second is not
waste: it is the half that usually has it.

**6. The root of a subtree, and the client that skips the library.**
*Built.*

- A server refusing to resolve above the root its channel represents,
  which section 20.3 calls the backstop.
- The rule stated where a server implementor reads it.

*Check.* A program sends `..` on a directory handle directly, without the
library, and is refused. That is the case the lexical rule cannot cover,
because a program that constructs its own messages is not running the
library's code.

`trespass` sends four walks by hand: `..`, `../motd`, `a/../b`, and
`..quiet`. The first three are refused with `SYS_ERR_PERM_DENIED` and the
fourth is an ordinary missing file, because a name beginning with two
dots is not a `..` component and refusing it would refuse a file somebody
is entitled to call that. Removing the rule fails the first; making it
match any component that starts with two dots fails the fourth.

**The rule is about a channel's own scope, which is why the server can
enforce it alone.** No component of a name may be `..`, whatever the
server's own shape is, and that needs no knowledge of any client's
namespace. It is in `sys/sys/fs.h` beside the operation numbers, because
that is the file a server implementor reads, and the server's own walk
points at it.

**Refused rather than missing, and the difference is the point.** A
client told a name does not exist looks for it somewhere else. A client
told it may not ask has been told the truth. The same distinction the
resolver drew for a path that climbs above the top, made again by the
other party, for the client that did not ask the resolver.

**Two places, two reasons, and neither is redundant.** The library
reduces `..` before choosing a handle so that a path crossing a namespace
boundary reaches the server that can answer it, which is correctness
rather than confinement: matching the boundary first would send `../motd`
to a server that cannot see above itself. The server refuses `..`
whatever the client did, which is confinement rather than correctness.
Section 20.3 says assigning the whole job to either one breaks something,
and building both is what showed which something: with only the library
rule, `trespass` reaches above its connection; with only the server rule,
`/dev/../motd` reaches nothing at all.

*Reviewing it found the stage had written down a stricter rule than the
one section 20.3 states, and called it the rule.*

**Refusing every `..` is one implementation of "must not resolve above
the root", not the rule itself.** A name like `a/../b` never leaves the
subtree and is refused anyway. That is right for a server with one level
and nothing to walk, and it is wrong as a statement of what a server
owes: a server with real directories has to resolve the name and check
where it arrived, because a client is entitled to write a path that goes
down and comes back up. The header says the rule and then says which of
the two ways this server takes and why, and the test case that encodes
the strict reading says that is what it is testing.

**And one refusal was standing in for two.** A `..` sent on a connection
that names a file was answered with the refusal, when what is wrong is
that a file has no subtree to climb out of. The connection not naming a
place things can be found in is checked first, and a walk on a file says
so. Nothing noticed when that ordering was removed, because no test sent
anything to a file connection; one does now, and it fails when the order
goes back.

*A note on breaking things to check them.* The first attempt to break the
ordering edited the wrong line: the same condition appears in the walk
itself and in the handler above it, and disabling the first left the
second doing the work. The check passed, which looked like the ordering
not mattering and was the experiment not having been performed. Both
occurrences look identical and only their next line differs.

*Done when* two processes see different filesystem views assembled from the
same underlying handles, with no kernel involvement and no ambient root
anywhere.

*Would falsify.* The second provisional mechanism. Whether a system with no
absolute paths is usable by ordinary programs, or whether the complexity simply
moves into libc and gets worse there. If this fails, section 20 changes
substantially and section 25 with it.

The evidence is the utilities. `cat` is eighty lines of protocol today
because it names one component against one handle; if it is longer and
worse after paths exist, the complexity moved rather than went. And the
number to watch is how much of the library a program that never opens
anything by name still pays for.

*Watch for.* **Longest-prefix matching is where the off-by-one lives.** A
table holding `/` and `/dev` has to match `/development` against `/` and
not against `/dev`, which means matching on component boundaries rather
than on characters.

**A path that reduces above every entry must match nothing.** The
tempting failure is to fall back to the shortest entry, which manufactures
authority out of a name and is exactly what section 20.1 says names cannot
do.

**The table is fixed and small, and something has to say so.** Eight
entries is a build-time bound like every other bound here, and a program
handed nine gets an error at spawn rather than a truncated namespace it
cannot tell from a complete one.

*Size.* Medium, and spent. Six stages, of which the resolver and the
composer are most of the code and the first is the one that changed an
interface the kernel writes. Mostly library and programs, but not the
"blocks nothing and can move" this entry claimed before it was planned.

**Done.** Section 8 says what it answered, what it cost, and what it left
untested. The question is answered: a system with no absolute paths is
usable, the complexity moved into the library, and it moved once rather
than per program.

### M6. Driver Cohorts

**A shared protection domain, and what it actually saves.**

Cannot come before M4. Without protection domains a cohort is a naming
convention that demonstrates nothing.

*Needs.* Cohort membership fixed at build time. Buffer handoff by address
within a cohort. A two-layer stack to hand a buffer through, and a count of
what each layer costs.

*Five things M4 and M5 taught that change this entry.* The first one changes
what the milestone is about.

**The move contract is already zero-copy, so a cohort does not buy zero
copies.** M4 built the buffer handoff of section 6.1 and it copies nothing at
either tier: the sender's pages are re-mapped into the receiver at the address
they already had, and taken back at the reply. A boundary crossing does not
cost a copy. It costs four page-table entry writes per page and two whole-TLB
flushes, one in the hand-over and one in the take-back. The claim this
milestone can test is therefore narrower and sharper than the one written here
before. A cohort buys zero *mapping*, not zero copying, and the question is
whether a shared protection domain is worth two TLB flushes per layer per
request.

**The fair competitor is a cheaper invalidation, not a copy.** Both ports
flush the whole TLB, which was the right thing to build when the only mapping
change in the system was a spawn. Invalidating the one address that moved is a
far smaller change than a shared address space, and it costs no isolation at
all. A mechanism measured against a deliberately slow alternative proves
nothing, so the alternative is built first and the measurement is taken
against it.

**There is no second layer to cross.** The done-when clause needs a
filesystem and a block driver, and the tree has a filesystem that serves text
out of its own image. Every buffer in the system today crosses exactly one
boundary. The stack has to exist before anything about it can be counted, and
it is worth having whether or not the cohort survives the measurement.

**At Tier 0 a cohort is what the system already is.** There is one address
space, every process is in it, and a member reading a peer's memory is what
Tier 0 permits anybody to do. The mechanism's whole Tier 0 content is
therefore the membership list, which fault reporting reads. The recorded
session cannot show the difference, because one transcript covers all four
configurations. The difference goes where per-tier numbers already go, which
is the resource report.

**Nothing is reaped, and the pool stands at 42 of 48 processes.** M5 raised it
because the boot and check sequences size it and nothing is freed until M7.
This milestone adds a block driver and the programs that exercise a cohort, so
it raises it again. That is the third milestone in a row paying for M7 not
having happened yet, and it is written down here as a cost rather than
adjusted quietly in a header.

*Deferred, and why.* Scatter-gather with lazy mapping (18.4) and cohort
scheduling nodes (11.5) were listed in this entry and are not built.

Lazy mapping pays off on a forwarding path, where a server passes a payload on
without looking at it. No such server exists in the plan before M10, so
building the interface now means guessing its shape with nothing able to say
the guess was wrong. It moves to whichever milestone first has a layer that
forwards.

A scheduling node buys one thing, which is that time yielded inside a group
stays inside it. On one core, with a check suite that runs nothing
concurrently, nothing can observe that. It moves to M8, where there are cores
to lose work to.

Cohort fault recovery (18.2) stays deferred to M7 for the reason already
given: tearing down and restarting every member together cannot be shown
without death.

*The prediction, written before building so that it can be wrong.* A cohort
removes four page-table entry writes and two TLB operations per page per layer
crossed, and nothing else. If invalidating by address closes most of that gap,
then a shared protection domain is being bought for very little, and section
18 should be deleted rather than kept. The retrospective answers this with the
two numbers and not with an argument.

*This is not the measurement section 2 puts late.* That rule is about
performance numbers, which need a system and a real workload before they mean
anything. What this milestone counts is operations rather than time: how many
page-table entries are written and how many TLB operations are issued to read
one file through two layers. That count is exact on an idle machine and does
not become truer under load, so taking it now is safe. What it cannot say is
how much those operations cost, which is the part that waits for M10.

*Stages.* Six. Two of them build the apparatus, one builds the honest
alternative, and only the fourth and fifth are the mechanism itself.

**1. A cohort is an object, and membership is fixed at build time.** *Built.*
A cohort owns one address space, member slots sit side by side in it, and
which images are members is a table in the kernel's source.

- A cohort owns one address space, and a member points at it rather than
  owning one of its own.
- Membership is named where the process is spawned, from a table the kernel is
  built with. No operation joins a cohort, which is section 18.1: a runtime
  join would be a privilege escalation primitive.
- At Tier 0 a cohort is a membership list and nothing else.

*Check.* `peer_b` tells `peer_a` where a word in its own memory is, and
`peer_a` reads that word and writes it. The message carries an address and
nothing else: no buffer descriptor, no grant, and nothing for the kernel to
map or validate. `peer_b` then checks that the reply carried the old value and
that the new one is in its memory, and exits zero. This passes in all four
configurations.

**The clause is narrower than it was written, in two places.** It said each
member reads the other's memory; one member reads and writes the other's,
which is the same claim in one round trip and is what the two programs do. And
it said the peer read goes in the recorded session, which it does not: it is a
boot check, beside the other structural checks, because it needs a channel
wired between two named images and there is nothing for a person at a shell to
type. That is enforcement of the same strength, since a boot check that fails
halts the machine and `make check` fails with it, but it is not the recorded
transcript and the entry should not have claimed it would be.

*Breaking it on purpose.* Taking `peer_b` out of the cohort and leaving
everything else alone kills `peer_a` at Tier 2: it takes a page fault at
0x81601000, which is the address inside `peer_b`'s slot that it was handed,
and the machine halts on a fault nobody was waiting for. The same edit at
Tier 0 passes, and `peer_b` exits zero with the two programs in no shared
domain at all. That pair is the whole of what the stage built. The Tier 2 half
is the mechanism working, and the Tier 0 half is the mechanism being absent
and the check passing anyway, which is what section 18 means when it says a
cohort is what Tier 0 already is.

Two build mistakes stop the boot rather than surfacing later. A cohort naming
an image the archive does not have halts with `no image called peer_c`, and
two members linked for one address halt with `peer_a and peer_b are linked for
0x81500000`. Both were provoked. The second matters more than it looks: in a
shared space the loader would have written the second member on top of the
first, and the first would have found its memory rewritten by the loader
rather than by a peer.

**Membership is decided by the archive lookup and not by a comparison of its
own.** The kernel has no `strcmp` and should not grow one for this. The
question is whether a spawn of this image is a spawn of that member, and the
comparison entitled to answer it is the one the loader uses a moment later.
Two rules for one question is how a member ends up almost in a cohort, which
is the shape of half the defects the last two milestones turned up.

*One thing here cannot be falsified by the suite.* Loading a second member
into a space its peers are already running in flushes the TLB, because the
addresses just filled were absent from that space a moment ago and both
architectures permit an implementation to have cached the absence. Removing
that flush breaks nothing on either port under QEMU, which was tried. The
flush stays because the manuals allow the case, not because anything observed
it, and the comment in `proc_load` says so rather than implying a check stands
behind it.

*What it cost.* Three processes and three threads, which puts Tier 2 at 45 of
48 processes and Tier 0 at 41. The address space is the same one word it
always was, shared rather than copied, so a cohort of two costs one page table
instead of two. The kernel grew one file of about a hundred and seventy lines
and one field in `struct proc`.

*Reviewing it found one defect, and it was the milestone undoing something an
earlier one had earned.*

**A cohort put back the rule that a program may have only one live copy, and
nothing noticed.** At Tier 0 the loader copies an image to the address it was
linked for, so a second copy of a running program writes over the first, and
`proc_spawn_op` has refused that since the tier existed. Tier 2 ended the
restriction rather than improving the check: an address space each makes two
copies of one image two processes that cannot reach each other, and the
comment on that rule says so in as many words. A cohort shares the space
again, so for a member the hazard is back, and this stage shipped without
noticing that the sentence it had just read no longer held.

It is reachable from a shell. `peer_a` is in the boot archive, `sh` spawns any
image in it by name, and the first `peer_a` is sitting blocked in receive with
its slot mapped in the shared space. Instrumenting the loader showed exactly
what the rule exists to prevent: the running `peer_a`'s slot base was frame
0x8307b000 before the second spawn and frame 0x8327d000 after it, with the
first process still alive. Its text, its data and its stack were replaced
underneath it by fresh zeroed frames, and its old frames were lost. Nothing
crashed in that run, which is the worst part. The victim was blocked in the
kernel and never got the chance to notice.

The fix is a record in the cohort of which process is running as each member,
and a refusal when that place is taken. The place a process left by exiting is
free again, because M7 restarts a cohort by spawning its members, and a rule
that said "never twice" would be a different rule and the wrong one.

**The refusal says `SYS_ERR_WOULDBLOCK`, which is what the loader's own rule
has always said.** The first version added an error code for it. That was
wrong for the reason the last two milestones keep turning up in the other
direction: one condition should not have two answers depending on which rule
happened to notice it. The condition is the same condition, and the answer is
honest either way, since the request succeeds once the running copy has
exited.

*Two things the review recorded rather than fixed.*

**A member cannot tell a peer's address from anyone else's.** Passing a buffer
as a bare address is sound between members and unsound from anywhere else, and
nothing in a message says which it is. `peer_a` is safe because the only
connection to its channel was minted for `peer_b`, which is a fact about who
holds handles and not about the cohort. A member serving one channel to
members and outsiders alike would be handing the whole domain to its clients.
Stage 5 meets this directly, because the filesystem serves clients that are
not members and talks to a block driver that is, and it will need two channels
rather than one. Section 18.1 now says so.

**The member table holds process pointers that M7 has to clear.** Nothing is
reaped, so nothing can reuse a process slot and the pointer cannot go stale
today. The day reaping arrives it can, and the teardown of section 18.2 owns
that array as much as spawn does.

*Both halves of the new check were broken on purpose.* Removing the refusal
makes the second spawn return OK and the check fails; making a place stay
taken after its process exits makes the legitimate restart fail. Neither
passes quietly.

**2. A block driver under the filesystem.** *Built.* The bytes moved out of
the file server into a store beneath it, so a read now crosses two boundaries
and the tree has a stack to measure.

- A RAM disk serving fixed-size blocks over the protocol that already exists.
- `fs_server` reads file contents through it rather than out of its own image.
- A buffer a client sent now crosses two boundaries instead of one.

*Check.* `cat /motd` prints the line it has always printed and the recorded
transcript does not move at all. What changed is underneath it. The resource
report shows one more process and one more thread, and the boot report shows
`fs_client` and `fs_batch` each costing one more rendezvous than before the
store existed. Each read gained a crossing, which is the shape a layer has.
The review changed the block size and with it the count; the numbers as they
stand are at the end of this stage.

Breaking the store breaks the reader. A store made to refuse the block
`/u/notes` lives in turns

```
$ cat /u/notes
only the second directory has this
```

into

```
$ cat /u/notes
cat: /u/notes: read failed
```

so the bytes really do come from underneath, and the refusal travels up
through the filesystem to the client rather than being swallowed into a short
read.

**No new protocol, deliberately.** A block read is a read. The request and
reply of section 25.2 already carry an offset, a length and a buffer, and a
block store is what is left when the name is dropped and the offset and length
are required to be whole blocks. `sys/blk.h` holds a block size and a layout
and no messages. The store does refuse three different ways rather than one,
because an unaligned offset, a length that is not whole blocks, and a range
off the end of the disk are three mistakes and a caller told only "no" has to
guess which.

**The move contract does not forward, and that is the stage's real finding.**
`fs_server` cannot pass the client's buffer down to the store. The buffer is
mapped into the file server at the client's address, but section 23.2 checks a
descriptor against the sender's own image, and a page on loan is not the
sender's to lend on. So the file server sends a buffer of its own, and a read
through two layers copies once. That copy is not something a cohort removes.
What a cohort removes is the mapping, and stage five is where the file server
stops sending a descriptor at all and passes the client's address to a peer
that can already see it. The milestone is measuring mapping cost, and this is
the moment it became clear that copies and mappings are separate questions
that a stack of layers pays separately.

**A store the filesystem cannot reach is a filesystem that does not start.**
`fs_server` returns rather than serving errors when `SYS_NS_STORE` names
nothing. There is no content left in it to serve, and a server that started
anyway would answer every read with a failure no client could act on.

*One thing the break test turned up about the check suite.* Refusing the block
behind the second directory's `motd` changes nothing observable, because
`cat /u/motd` resolves to the first half of the union and the second half's
`motd` exists in order to be shadowed. The union is exercised, since
`/u/notes` comes from the second half, but those particular bytes have never
been read by the recorded session and were not read before this stage either.

*What it cost.* One process and one thread, and the process pool went from 48
to 56 and the thread pool from 80 to 96. That is the third milestone running
to raise them, and it is M6 paying for M7 not having happened: forty-six of
forty-eight with three stages still to build was not enough room. The comment
in `config.h` says so rather than the constant changing quietly.

*Reviewing it found three defects, all of one shape, and the block size was
wrong for a fourth reason.*

**A number stated twice, three times over.** Splitting one server into two put
the bytes in one program and their lengths in another, with nothing checking
they agreed. `blk_server` held the strings, `fs_server`'s node table said 31,
32 and 35, and the boot sequence checked a read against `sizeof(motd_text) -
1` taken from the kernel's own unrelated copy of the same text. All three
agreed on the day, and an edit to any string would have made a file server
report a length its store did not have. That is a short read or a read of
whatever followed, and neither announces itself.

The lengths now live in `sys/blk.h` beside the block numbers, `fs_server` uses
them, the boot sequence uses them, and `blk_server` carries static assertions
that each string is the length the header claims and that no file runs into
the next one or off the end of the disk. An edit that breaks the layout stops
the build instead of the check.

**A reply field nobody read.** `store_read` returned the store's status and
ignored its length. The block buffer it reads into is reused from one block to
the next, so a store answering `SYS_OK` without filling it would have had the
file server copy out the block it fetched last time, which is one file's bytes
served as another's. Not reachable through this store, which fills the buffer
whenever it says yes, but the whole point of the layer is that the store is a
separate program. Reading the field that says how much arrived costs a
comparison. It now returns `SYS_ERR_TRUNCATED` when the store says yes and
means less, and a store made to report one byte short fails the check.

**The block loop never ran twice.** Five hundred and twelve byte blocks with
files of thirty-one, thirty-two and thirty-five bytes meant every read was one
block, and the arithmetic that makes a filesystem a filesystem was a loop
nothing ever entered a second time. A block size larger than the data is a
block store that never asks the layer above it to do anything.

Sixteen bytes now, which is smaller than every file and chosen to be. Each
file spans two or three blocks, so a read fetches each in turn and copies part
of the last, and the partial-final-block path runs on every read in the
recorded session. It is not a plausible size for a disk, and the header says
that plainly: it is the size that makes the layering real in a system whose
entire contents are three short strings.

**And that exposed something true about layers that a bigger block had
hidden.** With several store reads per file read, the file server now queues
behind the store. The store replies, the reply wakes the file server at the
priority its client donated, the file server preempts the store before it gets
back to receive, and its next send finds nobody waiting. So a layer costs a
scheduling round trip per block on top of the message itself, which is a cost
stage three should count and this stage did not know about.

It also broke a check M4 built, and quietly. `run_client` asserted that
`fs_batch` never queues behind a busy server, which is section 25.7's claim
about one message costing less than three. The trace was system-wide, so the
file server's own queueing answered yes and the claim silently became
unfalsifiable. `trace_saw_thread()` asks about one thread by name, and the
claim is back: `fs_batch` itself still never send-blocks while the system
around it does. Flipping the expectation fails the check, so it is falsifiable
again rather than merely true.

*The count changed with the block size.* `fs_client` costs five rendezvous
where it cost three before the store existed, and `fs_batch` three where it
cost one. Two of each are the store, one per block of a thirty-one byte file.

**3. What a boundary costs.** *Built.* Four page-table entries and two
whole-TLB flushes to move one page across a boundary and back, at Tier 2.
Nothing at Tier 0. The review below corrects the shape of that: the entries
are per page and the flushes are per half of the move.

- A count of page-table entry writes and TLB operations on the buffer path,
  kept in the kernel and printed in the resource report.
- Per tier, because the answer at Tier 0 is zero.

*Check.* The resource report carries a line the check compares:

```
  buffer remapping: 42 pages in 42 halves, 84 entries, 4 tables, 42 flushes
  buffer remapping: 0 pages in 0 halves, 0 entries, 0 tables, 0 flushes
```

The same numbers on both ports, which is worth saying: the ports describe
memory differently and spend the same amount doing this.

**The rate is checked as arithmetic, not recorded as a total.** The hand-over
writes the entry into the receiver and rewrites the sender's to take the page
away; the take-back unmaps one and restores the other; each half throws the
whole translation cache away when it is done. Two entries per page in each
half, one flush per half, and the kernel panics if the ports spent anything
else. `fs_client` costs twenty entries and ten flushes over ten page-moves in
ten halves, `fs_batch` twelve and six over six and six, and neither number is
written down anywhere: they are derived and then compared against what
happened. The review below is where the denominators were separated; the first
version multiplied by the rendezvous count.

**Three numbers rather than one, because they are three costs.** An entry is a
store. A table is an intermediate level built, which is a store and a frame
that is never given back. A flush is the whole translation cache discarded,
which is neither and is the expensive one. The tables are the interesting
small number: four for the whole run, because a table is built the first time
a program lends a page into a particular peer's space and is reused by every
crossing after it. The mapping cost of a boundary is per message; the table
cost is per pair of programs that have ever spoken.

*Counted where it is spent.* The counters are in the ports rather than in the
rendezvous, because what matters is what the hardware description had written
to it and not what the caller asked for. A port that maps a block where the
other maps a page does less work, and a count taken above them could not tell.
Charging is bracketed around the two halves of the move, which is honest
because neither half can block: nothing else runs between the begin and the
end, so nothing else can be charged to it. Loading a program writes an entry
per page of its slot, two hundred and fifty-six per process, and a total that
included those would be a number about the loader with the buffer path lost in
it.

*Breaking it on purpose.* Taking the block store back out of the read path,
and changing nothing else, drops the run from 84 entries and 42 flushes to 24
and 12, and from four tables to three. The store is 60 of the 84 and 30 of the
42, and one of the tables. So the count follows the path rather than measuring
something fixed about the boot sequence.

**The entry predicted the wrong multiple, and the real relationship is
better.** It said two layers cost twice one layer. They cost one crossing for
the client plus one per block of the file, and with sixteen byte blocks a
thirty-one byte file is two blocks, so `fs_batch`'s single client message costs
three crossings and not two. The general statement is the one worth having:
a read costs four entries and two flushes per page per crossing, and the
number of crossings is one plus the number of blocks the file occupies. A
block store with realistic blocks would make that two; this one makes it
three; neither is a property of the boundary, which is the thing being
measured.

*One cost is deliberately not counted.* Reviewing stage two found that a layer
also costs a scheduling round trip per block, because the store's reply wakes
the file server at its client's donated priority and the file server preempts
the store before it returns to receive. That is real and it is not a mapping,
so it does not belong in this line. It is already visible as the rendezvous
count and in the send-blocking trace, and a cohort will not remove it: members
still send messages to each other. Only the mapping goes.

*Reviewing it found two defects, and both were in what the stage claimed
rather than in what it built.*

**The rate has two denominators and the stage stated one.** An entry is written
per page. The translation cache is thrown away once per half of the move,
however many pages that half carried. The stage said "four entries and two
flushes per page per crossing", which is right only for a one page buffer, and
every buffer in this tree is one page. A two page buffer costs eight entries
and still two flushes.

The check had the same fault and was worse, because it multiplied by the
rendezvous count: it asserted four entries and two flushes per message, which
held only because every message here happens to carry exactly one page. It was
checking the workload and calling it the mechanism.

The rendezvous now says how many pages each half moved and how many halves
there were, so the numerator comes from the ports and the denominator from the
message. The rule checked is two entries per page and one flush per half, and
it holds for any workload rather than for this one. Spending one extra entry
per page fails with `10 pages moved cost 25 entries, not 20`; spending one
extra flush per half fails with `10 halves cost 15 flushes, not 10`. The two
are independently falsifiable, which the earlier form could not be.

It is also checked over the whole run and not only over the two client runs
that print it, so a path that spent something other than the rule says would
be caught even though nothing measures it directly.

**The bracket can be preempted, and the comment said it could not.** The first
version kept the charging depth in a global and justified it by saying neither
half of the move can block, so nothing else runs between the begin and the
end. Blocking is not the only way something else runs. The timer interrupt
calls `sched_undefer()` on its way out, which dispatches if something better
became runnable, so a thread can be preempted in the middle of the move and
everything the next thread mapped would have been charged to the buffer path.

The depth is on the thread now, so a preempting thread is not charging and its
work is its own. Instrumenting the old form to panic if a second thread wrote
tables mid-move shows it never happens in these runs, so this was latent
rather than live, and it is recorded here as latent. That is the same shape as
the flush in stage one: kept because the argument for it is sound, not because
a check caught it.

**And the report line said the wrong thing at Tier 0.** It read `moving
buffers: 0 pages`, which says no buffers were exchanged. Buffers are exchanged
at both tiers and the rendezvous counts show it. What is zero is the
*remapping*, so the line says `buffer remapping` and the zero now means what
it should: the same messages carrying the same buffers cost nothing to place,
because there is one address space and a buffer that arrives is already where
it needs to be.

```
  buffer remapping: 42 pages in 42 halves, 84 entries, 4 tables, 42 flushes
  buffer remapping: 0 pages in 0 halves, 0 entries, 0 tables, 0 flushes
```

**4. Invalidating by address.** *Built.* The buffer path throws nothing away
that has not moved. It also turns out never to have needed to throw anything
away at all, and that is the stage's real result.

- Both ports invalidate the addresses that moved instead of flushing the whole
  TLB.
- The buffer path is measured again.

*Check.* The report counts the two separately, and the buffer path's flush
count is now zero:

```
  buffer remapping: 42 pages in 42 halves, 84 entries, 4 tables, 42 invalidations, 0 flushes
```

and, on the line above the compared report, the scale to read it against:

```
address space switches: 395, each a whole-TLB flush, against 42 buffer halves
```

One invalidation per page per half, not per crossing. The page enters the
receiver at the address it had in the sender and leaves the sender at that
same address, and neither port tags a translation with the space it came from,
so one call removes both entries. The zero is asserted rather than merely
observed, because a flush put back here would vanish into a system that
already flushes on every address space switch.

**The second line is the finding.** Three hundred and ninety-five address
space switches, each of them a whole-TLB flush, against forty-two on the
buffer path before this stage. The thing this milestone set out to measure was
under ten percent of the whole-TLB flushes in the system, and the other ninety
percent is context switching, because neither port assigns address space
identifiers and so every switch discards everything.

*And the invalidation cannot be shown to matter.* The entry said a port made
to invalidate a neighbouring address would fail an existing check. It does
not: the check passes. Removing the maintenance entirely, both halves, also
passes, on both ports, with the whole recorded session intact. There is an
address space switch between the hand-over and the receiver touching the
buffer, and another between the take-back and the client touching it again,
and each of those already discards the translation that would have been stale.

So the forty-two flushes stage three measured were work that did not need
doing, and the forty-two invalidations replacing them are not needed either,
*as the system is currently arranged*. They stay, for a reason that is about
what comes next rather than about what a check can show: inside a cohort there
is no switch between members, so this is the only thing that would clear a
stale entry, and a system that assigned address space identifiers would be in
the same position. The maintenance is correct and it is insurance. It is
recorded here as insurance rather than as something the suite proves, which is
the same standing as the flush in stage one.

**The plan's fair competitor was the wrong one.** This stage exists because
measuring a shared address space against a system that flushes the whole TLB
on every crossing would flatter it, and the honest alternative was said to be
invalidating by address. That was aimed at the wrong cost. Invalidation on the
buffer path saves flushes that were redundant. What a cohort actually removes
is the address space switch between its members, and the alternative that
removes the same cost without a shared domain is address space identifiers,
which neither port has. Stage five now has a sharper question than it was
given, and the milestone's falsification clause has to be answered against
ASIDs rather than against this.

*What it cost.* Two functions, one per port, and a counter. Nothing in the
recorded session moved.

*Reviewing it found two defects. One was a number that moves on its own, put
into a file compared exactly.*

**The switch count is not deterministic and was recorded as though it were.**
The same session gives three hundred and ninety-five switches paced and three
hundred and eighty-eight burst, because how many times a thread is dispatched
into a different space depends on when console input arrives. Three runs of
each are stable, so it would have passed today and failed later for a reason
no change caused. Everything else in that file is fixed by program logic: the
buffer remapping line is forty-two in both modes, because how many buffers
cross is a property of the programs and not of the host.

The count moved out of the compared report into the line above it, which is
printed and read and not diffed, and what is checked instead is the relation
that does not move: there are more switches than there are buffer halves. Set
the threshold to a hundred times and the check fails with `switching stopped
dominating: 395 switches, 42 halves`, so the claim is still falsifiable
without pinning a number the host can move. If a later stage makes the
relation false, that is a result and should be read as one rather than
repaired.

**And a claim about what comes next was backwards.** The comment said that a
port carrying address space identifiers would turn this into an invalidation
of one address in one space "and the callers do not change". The callers
change. One call covers both spaces here precisely *because* nothing is
tagged: an entry cannot say which space it came from, so removing an address
removes it everywhere, and a page moving between two spaces needs one call.
Give the ports identifiers and each entry carries its space, one call stops
covering both, and the buffer path needs two calls per page instead of one.

That matters beyond the comment, because ASIDs are now the alternative this
milestone has to be measured against. They would remove the flush from every
one of those switches and add a second invalidation to every page move. The
number this stage records for the buffer path is therefore a number for a
system that is not paying that, and the retrospective has to say so rather
than comparing the two as if they were like for like.

**5. The two layers in one cohort.** *Built.* One line in a build-time table
removes seventy-one percent of the buffer remapping in the system and a copy
per block, and the recorded session does not move.

- `fs_server` and the block driver share a cohort, so the buffer crosses the
  second boundary as an address.
- The hand-over already does nothing when both ends share an address space.
  The work here is making the two ends share one, and establishing that the
  validation rules of sections 6.1 and 23.2 still hold when they do.

*Check.* The same programs, the same transcript, and one entry in
`kern_cohort.c` the only difference:

| | out of the cohort | in it |
|---|---|---|
| pages remapped | 42 | 12 |
| page-table entries | 84 | 24 |
| tables built | 4 | 3 |
| invalidations | 42 | 12 |
| address space switches | 395 | 358 |

Thirty of the forty-two page-moves in the system were the filesystem talking
to its store, and they are gone: the hand-over sees one address space at both
ends and returns having done nothing. Thirty-seven of the switches are gone
too, because a dispatch from one member to another does not change the root
register and the port is not asked to discard anything.

**A member may describe the domain's memory and not only its own.** That is
the rule this stage adds and it is the whole of what a cohort buys on this
path. Section 23.2 says a buffer descriptor names the sender's own memory,
because a number naming somebody else's would be acted on for the sender if
nothing checked. Two members are one protection domain: the receiver can
already reach everything the sender can, the kernel maps nothing and
dereferences nothing, and no authority is created by allowing it because none
is being moved.

**The test is the cohort and not the address space, and that is why a cohort
is a named thing at Tier 0.** At Tier 0 every process shares an address space,
so a rule asking about the space would let any program describe any other
program's memory, and would undo what M4 stage 5 built. Asking about the
cohort gives the same answer at both tiers. The membership list that looked
like bookkeeping in stage one is what makes the rule statable at all.

**The rule is asked twice because a channel does not know who receives on
it.** It was created by whoever assembled the system and the receive right was
moved to a server afterwards, so at send time the only honest answer is to
refuse a non-member at once, as always, and let a member through to be decided
at the rendezvous. That is one rule asked where the answer is knowable, not
two rules: a sender in no cohort gets the same refusal at the same moment it
always did.

**What a member may not do is lend the domain's memory outside the domain.**
`peer_b` tries it, with the address of `peer_a`'s slot and a connection to a
server in no cohort, and is refused with `SYS_ERR_FAULT`. Removing the second
asking does not merely let it through, it stops the machine in the assertion
that follows, which is the assertion doing its job.

**The copy goes too, and it is not in the table above.** Stage two found that
the move contract cannot forward: a server holding a page on loan may not lend
it on, so a read through two layers copied once. Inside a cohort the
filesystem hands the store the client's own address and the store writes
straight into it, so a whole block wanted at a block boundary never passes
through the filesystem at all. A thirty-one byte file in sixteen byte blocks
takes one block direct and one staged, because the ragged end of a read has to
be cut out of somewhere.

*Breaking it on purpose, three ways.* Taking the store out of the cohort puts
every number back exactly and leaves the transcript identical, which is the
comparison the milestone wanted: the same server running both ways, so the
difference is the cohort and not the program. Sending the direct block one
byte off breaks the transcript, which says the direct path is the one being
taken. Removing the second asking halts the machine, which says the rule is
load-bearing.

**The filesystem does not require its cohort, and that was a change made
during the stage.** The first version always handed the client's address to
the store, so taking the two out of the cohort stopped reads working
altogether rather than making them cost more. A server that breaks because a
build-time table changed is a coupling nothing declares, and it also destroys
the measurement, which needs one program that runs both ways. It now tries the
direct path once, before anything has succeeded that way, and a refusal
answers the question for good. The refusal costs no rendezvous, because a
hand-over that fails puts the receiver back in receive before anything is
counted.

*Reviewing it found nothing that let memory escape, and three things that were
one edit away from letting it.*

**The check at Tier 0 is real, which is worth having established rather than
assumed.** The stage rests on a rule that has to hold at a tier with no
hardware behind it, and the temptation with such rules is that they compile
and are never exercised. Removing the second asking and running Tier 0 makes
`peer_b` return seven, which is the send it expected to be refused succeeding.
At Tier 2 the same edit halts the machine in the assertion behind it. Both
catch it; they catch it differently, and the Tier 0 one is the one that
matters, because at Tier 2 the hardware would have had something to say
eventually.

**One argument stood for two questions.** `buf_may_send(from, to, ...)` took a
receiver that was NULL at send time, meaning "not known yet", and a real
process at the rendezvous. Nothing reaches it with a threadless receiver
today, so the two meanings never collided, and a NULL that means "allow" in
one caller and "no process" in another is the shape this file has been bitten
by more than once. It is two functions now. `buf_may_deliver` is the rule.
`buf_may_send` is the most that can be decided before there is a receiver, and
the invariant is written down: it must never be stricter than the rule, or a
legitimate send would be refused before anyone could say it was legitimate.

**A check program was teaching a wrong use of a name.** `peer_b` was handed
its outsider connection as `SYS_NS_CWD`, which means the directory a process's
paths start at. `peer_b` resolves no paths. Section 20.4 has private
identifiers for exactly this, a number a parent and a child agree on and
nothing else reads, so it uses one. A borrowed role in a check program is a
worked example of misusing a role.

**And a downgrade could have been permanent for the wrong reason.** The
filesystem learns whether the store is in its cohort by sending it an address
and reading the refusal, and `SYS_ERR_FAULT` was taken to mean "not my memory"
forever after. No store answers `SYS_ERR_FAULT`, and this one cannot: it says
`INVALID_ARG`, `NO_ENT`, `TRUNCATED` or `NOT_SUPPORTED`. But a store that did
would have turned one bad read into a permanent loss of the direct path, quiet
and unattributable, and the whole point of the layer is that the store is a
separate program that may change. The question is asked once now, before
anything has succeeded that way; after that a `FAULT` is a read that failed.

*The measurement is unchanged by all three.* Taking the store out of the
cohort still restores every number exactly with the transcript identical.

**6. A fault names the cohort, and the recovery path is stubbed.** *Built.*
A member's fault halts the machine whether or not anything was waiting for it,
and the halt says which cohort died, who was in it, and what M7 has to build.

- The fault reporting of section 16.2 says which cohort a faulting member
  belongs to.
- The system halts, and the teardown of 18.2 is named in the message as M7's
  work rather than attempted.

*Check.* `peer_c` is a member of the peers cohort whose only act is to trap.
It is in the archive and the recorded session never runs it, because running
it stops the machine. Typed at the shell, in all four configurations:

```
$ peer_c
cohort peers: member peer_c faulted, instruction at 0x0, pc 0x81800042
  the whole cohort is the recovery unit, per section 18.2: peer_a peer_b peer_c
  tearing them down together and restarting them is M7, and is not built

panic: cohort peers cannot be recovered and must not be left running
```

And the same program with one line of the membership table changed, so that it
is in no cohort:

```
$ peer_c
sh: peer_c: instruction fault at 0x0, pc 0x81800042
[-16]
$ last
-16
```

One program, one fault, two outcomes: contained and reported to its parent, or
the end of the system. That pair is what section 18.2's trade costs, shown
rather than argued.

**It halts even though something was waiting for it.** The shell is `peer_c`'s
parent and holds its completion timeline, so before this stage the fault would
have been an ordinary child death. Section 18.2 is why it cannot be: the
members share a protection domain, so one that faults may have written
anything into any peer before it did, and nothing in the system can establish
that it did not. A supervisor holding one member's handle can be told that
member died. What it cannot do is anything useful, because putting the domain
right means tearing every member down together and there is nothing here that
can. Continuing would mean running peers that may already be wrong, which is
the one outcome section 18.2 rules out.

The check therefore sits before the question of whether anyone was watching,
not after it. Whether somebody was waiting changes what can be reported about
one process and changes nothing about a domain whose survivors may already be
wrong.

**It traps rather than touching a bad address, and that is not a detail.** An
address that faults is a Tier 2 idea. At Tier 0 nothing is protected and
reading the kernel succeeds, which is what M4's fault checks record. An
illegal instruction is a fault at both tiers and needs no MMU to mean
anything, so the same program dies the same way in all four configurations and
the halt can be shown at the tier where a cohort is otherwise invisible.

**What the message says is what is missing.** Section 18.2 and 12.5 between
them name the work: revoke every handle every member holds, force-signal their
timelines in error state, release clients blocked sending to them with
`SYS_ERR_OWNER_DEAD`, and deliver `SYS_PULSE_DEAD_OWNER` to every channel with
an outstanding rendezvous. Then a supervisor relaunches the cohort, per
section 18.3. None of that exists, and the running system says so rather than
leaving it to this document.

*What it cost.* One image in the archive, never spawned by the recorded
session, so no process and no thread. The resource report does not move.

*Reviewing it found one defect, and it was the rule attached to the wrong
event.*

**A fault in a member with more than one thread ended nothing.** The check sat
in `proc_thread_exited`, which runs when a process's *last* thread leaves, and
behind the early return for a process that still has threads. So a member
whose faulting thread was not the last one out went straight past it. Worse,
the fault record is taken from whichever thread does leave last, so the fault
was not merely unhandled, it was erased.

Built as a temporary `peer_c` with two threads, one trapping while the other
waited for it and then returned normally, and run from the shell:

```
$ peer_c
peer_c: still here, and so is the cohort
    peer_c exited with status 0
$ last
0
```

A thread of a cohort member wrote something no one can vouch for, the cohort
carried on, and the program reported success. Section 18.2 was not applied and
nothing said so.

The rule is on `thread_fault` now, where the fault happens. A process ends when
its last thread does; a domain ends when any thread of any member faults, and
those are different events that were being treated as one. The same two-thread
program now halts, and so does the single-threaded one, in all four
configurations. The check in `proc_thread_exited` is gone rather than kept
beside it, because the fault record it read is only ever written by
`thread_fault`, so two places asking would be two places to keep agreeing.

*What this says about the stage as it was written.* The entry asked for a
member's fault to name its cohort and halt, and a single-threaded member is
what every member in this tree is, so the stage passed its own check and every
by-hand run of it. The hole needed a member shaped like nothing the system
contains, which is the kind a check suite is worst at finding and the kind
worth building on purpose.

*Done when* a filesystem and a block driver in one cohort exchange a buffer as
an address rather than a grant, with no mapping cost for that crossing, a
fault in either halts the system with the recovery path stubbed and named, and
the resource report carries the number that says what the cohort saved against
an alternative that keeps the isolation.

**Done.** Section 9 says what it measured, what the measurement turned out to
be about, and why the question it was written to ask is not yet answered.

*Would falsify.* Whether zero mapping within a shared domain is worth a shared
domain at all. The measurement is taken against a system that invalidates by
address, not against one that flushes everything, because the second
comparison flatters the mechanism. If the difference is small, section 18 is
deleted in the retrospective and the plan loses a mechanism rather than
gaining one. Whether the fault-domain cost is tolerable is still answered in
M7, when a fault recovers rather than halting.

*Size.* Medium.

### M7. Death

**Cancellation, killing, and restart.**

The design leans on death everywhere: cohort recovery (18.2), the
supervision tree (24.3), and reclaiming server state (25). None of it has
been exercised.

**This was scheduled second and has been moved.** The original argument was
that teardown is easier to reason about in a system that already works, and
that argument is still right. What it got wrong is how much system had to
work first. Nothing before this point has anything worth reclaiming: two
processes that run to completion (M2), a shell whose utilities exit
normally (M3), and a page fault path whose faults are fatal (M4) all reach
their goals with an exit that halts or a fault that panics. Building
cancellation, pulses, reaping and restart ahead of them would have meant
building the recovery machinery for a system with nothing yet to recover,
and then maintaining it across four milestones that never called it.

**What forces it here is cohort recovery.** M6 tears down and restarts every
member of a cohort together, and that is the first mechanism in the plan
that cannot be demonstrated without death. M6 was therefore built for its
zero-mapping half only, and its recovery half is part of this milestone.

*Six things it needs that were not listed.* Three were left out by
milestone 1 and are each load-bearing; three are new, and are what M6
built and could not finish.

**Pulses do not exist.** Section 12.5 has a server learn of a dead client
through `SYS_PULSE_DEAD_OWNER`, because a server is at a receive point and
can collect one, where a blocked client is not and must be released with an
error instead. Both halves are needed for death to be covered in both
directions, so section 13.1 has to be built here: a fixed-size queued
notification, with the reserved region that keeps a flood of user pulses
from displacing a death notice. A dropped death notice leaks a dead
client's resources permanently, which is why that reservation is
correctness rather than politeness.

**Nothing is ever reaped.** A thread that exits stops being scheduled and
keeps its pool slot for ever, so nothing has yet reused a slot. Restart
needs reuse, and reuse is the first real test of the generation counters of
section 7.1: milestone 1 asserted that a stale identifier is refused, but
only ever against slots it had freed by hand.

**Donation has to unwind through death.** A server killed while running at
a client's donated priority, or a client killed while its donation is
held, both leave a priority that no longer has a reason to exist. Nothing
in section 12.3 says what happens, and nothing in M1 could reach it.

**A lent buffer has to come home when either end dies.** This is M4's move
contract meeting death for the first time, and M6 made it matter twice
over by putting a second layer under the filesystem. A client killed while
reply-blocked has pages mapped in the server and taken away from itself; a
server killed while holding them has a window into memory whose owner is
about to be told the server is gone. Neither direction exists. What makes
it correctness rather than tidiness is that the pages are unmapped from
the client: a client released with `SYS_ERR_OWNER_DEAD` and no memory
where its buffer was is a client that faults on its own stack.

**A member's slot has to leave a space something is running in.** Every
member of a cohort has its slot mapped in one address space and the peers
keep executing there. Tearing a member out removes mappings from a live
space, which nothing but the buffer path has ever done, and M6 recorded
the TLB maintenance on that path as insurance because removing it broke no
check. This is where the insurance is claimed. It is also the first thing
in the system whose correctness depends on it, so it is the first thing
that can falsify it.

**And a cohort's member table holds process pointers.** Nothing can reuse
a process slot today, so the pointers in `c_running` cannot go stale.
Reaping is exactly what makes them able to, and M6's stage one recorded
that the teardown owns that array as much as spawn does.

*What M6 leaves behind is a specification rather than a note.* A member
that faults halts the machine and prints what is missing:

```
cohort peers: member peer_c faulted, instruction at 0x0, pc 0x81800042
  the whole cohort is the recovery unit, per section 18.2: peer_a peer_b peer_c
  tearing them down together and restarting them is M7, and is not built
```

Stage six replaces that halt. What it has to do is in sections 18.2 and
12.5: revoke every handle every member holds, force-signal their timelines
in error state, release clients blocked sending to them with
`SYS_ERR_OWNER_DEAD`, deliver `SYS_PULSE_DEAD_OWNER` to every channel with
an outstanding rendezvous, and then let a supervisor relaunch the cohort.

*The measurable outcome, stated before building so it can be wrong.* Three
milestones running have raised the process and thread pools because
nothing is reaped, and they now stand at 56 and 96, which is 606976 bytes
of kernel object memory with 433152 of it thread stacks. Those numbers
size the boot check sequence and not the system. After reaping they should
size what is alive at once, which nobody has measured and which this
milestone is what measures. The prediction is that the high-water falls
below fifteen processes and twenty-five threads. If it does not, something
holds handles it has no reason to hold, and finding out what is worth more
than the memory.

*Exit and join already exist*, from M3. What is left is everything that
happens when the exit was not the program's own idea.

*Stages.* Six, and the order is forced: nothing can be restarted until
slots come back, nothing can be killed until it can be released from a
wait, and nothing can be told about a death until there is a way to tell
it.

**1. Reaping.** *Built.* Thirteen processes and fifteen threads where the
boot sequence used forty-six and sixty-five, and kernel object memory down
from 606976 bytes to 226496.

- A process and a thread stop being permanent. The event is not the exit:
  section 10.4 keeps a status readable through a handle, so the event is
  the exit and the last handle both having happened, in either order.
- The generation counter of section 7.1 gets its first real test, which
  is an identifier held across a reuse naming a slot somebody else now
  occupies.

*Check.* The high-water is 13 of 24 processes and 15 of 32 threads, the
same in all four configurations and the same paced as burst, so it
depends on what runs and not on when input arrives. The prediction was
under fifteen and twenty-five, and it holds.

```
    a thread slot, taken again             13, generation 1 then 2
    a reply addressed to the thread that was there STALE
    processes in use, back to              9
    its handle, used after the join closed it STALE
```

The second line is the milestone's own falsification question, which said
nothing had yet tried whether the generation counters close the recycling
hazard under real reuse. A thread exits, its slot goes back, the next
thread takes the same index, and a reply addressed by the old name is
refused rather than delivered to whoever inherited the slot. Nothing
before this stage could reach that line, because no slot was ever handed
back.

*The pools are lowered rather than left.* They stood at 56 processes and
96 threads because three milestones running raised them, and they were
raised because they tracked how long the boot sequence was. They are 24
and 32 now, which is about twice what the sequence needs at once. That is
still not a property of the system in general; what it is now is a
property of how much runs at the same time, which is a far better thing
for it to be.

**Reaping a thread cannot happen where a thread ends.** Its kernel stack
is inside its pool slot, so returning the slot frees the memory the code
returning it is standing on. A dead thread with nothing left to ask about
it is parked, and collected at the next scheduling decision, which runs
on a stack that is not it.

The collection point is `dispatch()` and not the context switch, which
was the first version and was wrong. `dispatch()` returns without
switching when the best thread is already running, and a yield with
nothing else runnable is exactly that, so a thread parked by the last
switch sat there until something unrelated caused a real one.

**A process outlives its join by a bounded number of decisions**, because
its last thread is a reason it still exists until that thread's slot is
collected. The check waits for that rather than asserting a particular
number of yields, which would be a check about the scheduler dressed as a
check about reaping.

*Breaking it on purpose.* A thread that is never parked exhausts the pool
partway through the boot sequence, which is the shape of the system
before this stage and the reason the pools were where they were. A handle
that takes no reference moves the recorded high-water, because a process
is then reaped while something still names it. The generation bump and
the generation check on lookup are each caught by an invariant that M1
already had, which is worth saying: those checks were written against
slots freed by hand and they hold under real reuse without amendment.

*What does not come back is memory.* A slot returns to its pool; the
megabyte the process was loaded into does not, nor do its page tables.
The boot report says so, in the numbers beside the compared ones:

```
frames: 17925 of 32768 used at most, 17925 still held
```

Used and held are the same number because nothing has ever been freed.
That line is printed outside the compared file deliberately, because the
two ports do not spend the same and that file is one per tier. Closing it
needs the loader to record which pages it allocated, since a demand-paged
process runs on frames that belong to its pager's grant and freeing those
would take memory from whoever granted it. That is a stage of its own and
it is not this one.

*Reviewing it found two pointers that reaping can leave naming the wrong
object, one of them specified in the design and unbuilt, and confirmed a
hazard M6 predicted.*

**A channel keeps a pointer to its owner and takes no reference for it.**
Every other way of naming a process is either a handle, which now holds
the process alive, or an identifier, which the generation counters
protect. `c_owner` is neither. A reaped owner leaves it naming a freed
slot, and a reused one leaves it naming somebody else entirely, which no
generation helps with because a generation protects identifiers and this
is a pointer.

It is not reachable as things stand, because there is no operation for
making a channel and every one belongs to a process the kernel built.
But that is a fact about who calls `channel_create()` today rather than a
rule, and the check sequence already builds one for a process it has
finished with: the paging checks make a channel owned by a program they
have already joined, in order to have a pager that is not one. That
process stays alive only because the handle the spawn returned is never
closed.

`proc_reap` now refuses to reap a process any channel still names.
Closing that one handle, so the process really is reaped, makes it fire,
and what it says is the whole point:

```
panic: proc status: reaped while a channel still says it owns one
```

`status` is not the process the channel was made for. It is the program
that took the slot afterwards, and the guard caught the pointer naming
it. That is the aliasing hazard happening, on the second occupant,
exactly as a pointer with no generation behind it fails.

The real answer is teardown. A channel whose owner dies should go,
releasing its senders and telling its receivers, which is stage four and
stage six. Until then this makes the hazard stop the machine rather than
corrupt it quietly, which is what a stub is for.

**And the thing M6 said this milestone would create, it created.** M6's
stage one recorded that a cohort's member table holds process pointers
which nothing could make stale, because nothing could reuse a process
slot, and that the teardown owns that array as much as spawn does.
Reaping is what makes them able to go stale, and `proc_reap` gives the
place back before it frees the slot. Removing that one call fails the
cohort check with `a second copy of one that has exited: expected OK,
got WOULDBLOCK`: the place stays occupied by a pointer to a process that
is not there any more, and a legitimate restart is refused. The check
that catches it was written in M6's own review, against a hazard that
could not yet happen.


**And a completion handle pointed into the object it reported on.** This
is the one the review should have found first, because section 10.4
already specifies it: "when the last handle to a thread or process
closes, the object is reaped and its resources released, but any
completion-timeline handle still held remains valid and still reports the
exit."

A completion timeline is embedded in its thread or process per section 8,
so the handle is a pointer into a pool slot, and it took no reference for
it. Drop every other handle, keep the completion, and the object is
reaped underneath: the next spawn takes the slot and the handle now names
a timeline that has been zeroed and reused. Nothing before this stage
could reach it, because nothing was reaped, and the comment beside the
status operation said as much in as many words.

A timeline now says what it is embedded in, and a handle naming an
embedded one holds that object. The check holds a completion handle
across the closing of every other handle to a program that has already
finished, and the wait afterwards is still satisfied by the exit that
already happened. Making the completion hold nothing fails at once, with
`proc exiter: a thread outlived every reference to it`, which is the
count going negative rather than the corruption it was heading for.

**This is short of what section 10.4 asks, and the difference is
recorded rather than papered over.** The section wants the object reaped
and the completion still reporting from a value frozen into each
holder's slot. That needs the reap to find its holders, and nothing here
can: a handle table belongs to a process and there is no list the other
way. What is built keeps the object until the last handle of either kind
goes. The same holders are told the same exit; what it costs is the
object's memory held for longer, and what it gives up is the claim that
a reaped object is really gone.

*What the review did not find.* Duplication and transfer keep the count:
both go through insert and then close, and the insert comes first, so
the count never dips through zero in the middle of a move. A dead thread
can be parked only once between two scheduling decisions, because a
switch is only ever reached through a dispatch and a dispatch collects
before it switches. And the process's own handle table can be taken
apart while closing one of its handles wakes something, because by then
nothing names the process and its threads are all gone.

**2. Cancellation.** *Built.* One call, four waits, the same answer from
all of them, and the buffer comes home.

- The `t_unwait` hook already exists, forced into being by deadlines in
  M1 stage 7, and already serves channels and timelines.
- A cancelled sender that has already lent a buffer gets it back before
  it runs again. A thread released with its pages still in another
  process faults on its own memory, which is worse than the hang it was
  cancelled out of.

*Check.* `wedged` blocks one of four ways, chosen by its argument, and
the boot sequence breaks it out of each:

```
    released, and it said so               recv
    released, and it said so               fence
    released, and it said so               send
    a reply to a client that was cancelled STALE
    released, and it said so               reply
```

The program exits zero only if its blocking call returned
`SYS_ERR_INTERRUPTED`, and in the reply case only if it could write to
its own buffer immediately afterwards. The recorded buffer count went
from twelve halves to fourteen, which is one hand-over and one take-back,
so the pages moved rather than being abandoned.

**M1 stage 7 built the hook and M4 wrote the check, four milestones and
two milestones early.** Section 10.6 closes the list of ways to block and
requires each to leave behind the one function that takes a thread off
again. Deadlines were the first caller; this is the second, and it needed
no new hook, which is what that section is for. And M4 left a panic on
the far side of the send saying "cancellation does not exist yet. When it
does, this is where it will be found." Removing the buffer return from
the cancel path prints exactly that: `ipc: wedged ran again with its
buffer still lent out`.

**A cancelled client stops being somewhere a reply can go.** Section 10.5
step 3 says a server that replies anyway is told `SYS_ERR_STALE`, and
what makes it true is clearing the client's record of where a reply would
be written. The check makes that reply while the cancelled thread is
still alive and merely runnable, not after it has gone, because a reply
refused because the thread is dead would be the same answer to a
different question. Leaving the record in place fails with `thread
wedged: made runnable while already runnable`, which is the reply
releasing a thread the cancel had already released.

*What is not built here is step 2.* Section 10.5 has the server also
receive a `SYS_PULSE_DEAD_OWNER` so that it can let go of what it
allocated without waiting to find out at reply time. There are no pulses
yet; they are stage three, and the server in this check simply never
learns. That is the leak the section describes, and it is why the stages
are in this order rather than a different one.

**Cancelling a thread that is not blocked is refused, and that matters
more than it looks.** `SYS_ERR_WOULDBLOCK` and not `SYS_OK`: a caller
told `SYS_OK` may assume the thread is on its way out of a call, and a
caller told this has to look again. The check leans on the distinction,
waiting for the program to reach its wait by cancelling until the answer
changes. One value for both would have made that loop a race with no way
to see it.

**And a process can reach its threads now.** It has had a count of them
since M2 and never a way to reach one, because nothing needed to. The
list is here because the check has to cancel a thread of a program it
spawned, and stage five needs it more: killing a process unblocks every
thread of the target, and there is no doing that to a set nothing can
enumerate.

*Reviewing it found the thing the milestone entry had already listed, and
a check that could not have seen it.*

**A cancelled client leaves its donation behind.** The entry says
donation has to unwind through death, that a client killed while its
donation is held leaves a priority with no reason to exist, and that
nothing in section 12.3 says what happens because nothing in M1 could
reach it. Cancellation is what reaches it, and the stage shipped without
doing anything about it.

A server takes its client's priority at the rendezvous and gives it back
in the reply, and the reply is the only place it was ever given back. A
cancelled client is not going to be replied to, so the server keeps
running at that client's priority for ever. Worse with reaping in the
stage before: the server holds a pointer to the thread that donated, and
once slots come back that pointer can name whoever took the slot, so the
comparison that decides whether to give the priority back can match the
wrong thread.

The client records who it raised now, which is the half nothing had,
because the only way a donation ever ended was the reply and the reply
runs in the server and already had both ends. Cancellation ends one from
the other side, where the client is the only thread with anything to go
on. Removing the fix fails with `cancel: the server is still at 20, base
5`.

**The check could not have caught it, and would have passed for ever.**
Every server in this system is at priority fifteen and every program is
spawned below that, so `donate()` never raised anybody on a path a
cancel could reach: the reply-blocked case ran with no donation to undo,
and undoing nothing looks exactly like undoing it correctly. The check
now asserts the donation happened before asserting it was undone, and
that assertion fired the moment it was written, which is what said the
test had been passing for the wrong reason.

Making it real needed the server put below the client rather than above
it, and the client spawned through the loader rather than the operation,
because a child may not outrank its parent and the boot thread is the
lowest thing running.

**And the reply-blocked case had been testing something else for part of
its life.** Putting the server at priority zero, so that the client would
certainly outrank it, meant the server never ran at all: nothing yielded
below the boot thread, so it never reached its receive, and the client
queued as a sender instead of rendezvousing. The case would have been
the queued-send case twice over. Five was the answer, which is below
every program and above nothing that matters, and the waiting loop now
waits for the thread to be blocked rather than cancelling until the
answer changes.

**3. Pulses.** *Built.* Sixteen user pulses into a queue of eight leave
six, and the death notice sent after them arrives anyway.

- The reservation is correctness. A flood of user pulses that displaced a
  death notice would leak a dead client's resources for good.
- The overflow flag a receiver observes, so that a receiver knows it
  missed something rather than inferring it.

*Check.*

```
    a flood, and a death notice            6 of 16 kept, the notice too
    a receive after the loss was reported  OK
    a program asking for a system pulse    INVALID_ARG
    and a user one                         OK
```

Both halves of the first line matter. A queue that held everything would
say nothing about the reservation, and a notice that was dropped would be
the leak the reservation exists to prevent. Removing the reservation, so
that user pulses may fill the whole queue, stops the machine with
`channel pulses: the reserved pulse slots are full`, which is the notice
having nowhere to go.

**The stage entry had the reporting backwards and the design was right.**
It said the sender of the pulse that did not fit is told. Section 13.1
says the oldest is dropped and an overflow flag is set on the channel,
which the receiver observes on its next receive, and that is what is
built. The sender is not told because a sender that never blocks has
nowhere to be told: the point of a pulse is that sending it cannot fail
or wait.

**The flag is reported once and cleared.** The question it answers is
whether anything was missed, not how much. A count would be a number a
receiver could do nothing with, and leaving it set would have every later
receive claim a loss that had already been reported. Not clearing it
fails with `the loss was reported twice`.

**The oldest user pulse goes, and it need not be at the head.** A system
pulse can be in front of it, and taking the head instead would be user
pulses displacing a death notice by a longer route. One array with a
reservation rather than two queues, so that a server told a client died
learns it in the order it happened rather than ahead of or behind
everything else.

**A pulse goes through a connection, the way a message does.** The first
version took a channel handle, which was wrong twice: a channel handle
is the right to receive, and a program that could name a channel to pulse
it could pulse any channel it could name. A program may also only send
user codes; a code below `SYS_PULSE_USER_BASE` is refused rather than
renumbered, because the reserved slots are kept from exactly the program
that would like to fill them.

**A pulse is taken before a message.** Section 13.1 gives a server one
receive point for both and does not say which comes first. A
notification is about something that has already happened, so it should
not wait behind work, and the queue is bounded and drains: there are at
most eight of them and nothing refills it faster than a receiver empties
it, so this cannot starve messages.

*What is not wired here is the sender of the notice.* Nothing yet emits
`SYS_PULSE_DEAD_OWNER` on its own; the check sends one from the kernel to
exercise the reservation. Section 10.5 step 2 has a server learn that its
client was cancelled, and that is stage four, which is why the queue is
built before the thing that fills it.

*Reviewing it found the half of the queue the check never touched.*

**A pulse to a receiver that was already blocked handed it a stale
message.** Everything the stage checked enqueued first and received
afterwards, so no receiver was ever waiting when a pulse arrived. Waking
one went through the path a sender's wake goes through, which copies out
the thread's staging message and returns without `SYS_MSG_IS_PULSE`: the
receiver got whatever the last message on that channel happened to be,
dressed as a new one, and the pulse stayed in the queue.

The receive is a loop now, and not because anything retries. A receiver
woken for a pulse can find it gone, because the wake and the taking are
two steps and another receiver can run between them, so the question is
asked again rather than assumed. What tells a pulse wake from a sender's
is the client identifier: `channel_recv` clears it before it blocks and a
sender fills it in before readying anybody, so a wake with it still clear
is a wake no sender caused.

**The check that found it did not work the first two times, and both
failures were the check and not the code.** A fresh thread blocked in
receive has nothing stale to be handed, so the first version passed with
the fix removed. Making it serve a message first was not enough either,
because replying clears the same field. What catches it is asking the
receiver whether what it got was a pulse: with every wake treated as a
message the check stops with `a waiting receiver was handed a message`.

That also showed a second place keeping the same invariant. The pulse
wake was clearing the client identifier itself, which `channel_recv`
already guarantees, so removing it changed nothing and could not be
falsified. One place keeps it now.

**And the overflow flag was reported on the next pulse rather than the
next receive.** Section 13.1 says the receiver observes it on its next
receive, and a server that took a message in between would have learned
late or not at all. It is on both paths now. Not reachable as things
stand, because a loss means pulses are queued and the first one taken
carries the flag, but the rule is about receives and the code was about
pulses.

**4. Death notification.** *Built.* The party that can be told is told,
the party that cannot is failed, and neither waits on a deadline.

- A client blocked on a dead server is released with
  `SYS_ERR_OWNER_DEAD`, because it is not at a receive point and cannot
  collect anything.
- A server holding an unanswered request from a dead client gets a
  `SYS_PULSE_DEAD_OWNER`, because it is at one, and its later reply is
  refused as stale rather than delivered to whoever holds the id now.

*Check.*

```
    the server was told, and by name       0x40000e
    its reply afterwards                   STALE
    its client was failed, not told        OWNER_DEAD
```

The first two are one run: a client cancelled while a server holds its
request, the server discovering it by receiving rather than by replying,
and the reply it makes anyway refused. The notice carries the client's
identifier truncated, per section 7.1, and the check compares it against
the identifier the server already held, which is what a real server
would do. Not telling the server fails with `the server was never told
its client had gone`.

The third is the other direction, and it needs a channel owned by a
program rather than by the kernel, because the kernel never exits.
Section 12.5's trigger is the process owning the channel terminating.

**The stage entry named the filesystem server and a kill, and neither
was available.** Killing a process is stage five. What is available is
cancellation, so the client is cancelled and the channel's owner is
cancelled out of a wait it then leaves. The mechanism under test is the
same one either way: what releases the clients is the owner's
termination, not what caused it.

**The client and the server it loses have to be two images.** Both roles
were `wedged` at first, which works at Tier 2 and cannot work at Tier 0,
where every image is loaded at the address it was linked for: two live
copies of one image are one program written over another. `orphan` is
the client now. That is the same fact M6 stage one found for cohorts,
arriving from the other side.

**Telling the server has to be the last thing the cancel does.** The
first version sent the notice in the middle of dismantling the client,
and a server that outranks whoever is cancelling runs the moment it is
made runnable: it found a client that still looked like one waiting for
a reply, replied to it, and released a thread that was halfway through
being released. Cancellation is deferred now, so a run of readies is one
step, which is what the deferral was built for.

**And the notice needs the channel, which the unwait forgets.** Taking a
thread off what it was blocked on is what clears the record of which
object that was, so the channel is told before the unwait rather than
after. That ordering is the whole of why the first attempt delivered
nothing at all.

**A channel stops naming an owner that has gone.** M6 left `c_owner`
taking no reference and stage one guarded a reap against it. This is
what that guard was waiting for: the pointer is interesting only while
the owner is alive, and termination is when it stops being. The guard
now cannot fire, and removing the release makes it fire again, which is
how the two are checked against each other. The channel itself stays,
because tearing one down with its owner is section 18.2's teardown and
that is stage six.

*Not built: a server acting on the notice.* The check's server collects
it and confirms it names the right client; nothing yet uses it to let go
of per-connection state. That is the observation `PROPOSALS.md` gates
connection death notification on and it wants a real server to do it, so
it waits for one that has state worth reclaiming.

*Not checked: a client reply-blocked on a program that owns its own
channel.* Section 12.5 covers both blocked states and this covers the
queued one against a dying owner, plus the reply-blocked one against a
cancelled client. The remaining corner needs a program to own a channel
it also receives on, and nothing can arrange that: a channel is created
by the kernel and a program learns its handles at spawn, so the process
has to exist before the channel and the handle has to exist before the
process. Stage six needs it and will have to break that circle.

*Reviewing it found a pointer the stage's own fix had just made
dangerous, and two things it did to a server that was already gone.*

**The pager is a connection pointer and took no reference.** A process
keeps the connection object rather than the handle, because the kernel is
what sends a fault message and it does not send one from a table. That
was safe for as long as nothing freed a connection, which was until this
stage. A program that closed the handle it was given at spawn would now
have its pager naming a freed slot, and the next page fault would send
through whatever took it.

This is the third pointer of exactly this shape: the completion timeline
in stage one, the channel owner in stages one and four, and now this.
Every one was made dangerous by something being reclaimed, and none of
them is protected by a generation, because a generation protects an
identifier and these are pointers. The process holds the connection now
and gives it back when it is reaped.

Not falsifiable by the suite, and recorded as such: no program closes its
pager handle, so nothing reaches it. What is checked is the other half,
that a connection goes back at all. The four cancellation runs are each
given three connections, and the pool returns to what it was afterwards;
never giving one back fails with `24 connections in use, started with
16`.

**A client released because its server died was told to tell that
server.** The teardown a cancel does ends with a death notice, and
releasing a client whose server has terminated went through the same
path, so it left a `SYS_PULSE_DEAD_OWNER` on a channel whose owner was
the thing that had gone. Nobody would ever collect it, and enough of them
would fill the slots reserved for the notices that do have a reader,
which is the one thing the reservation exists to prevent. Cancelling
tells; releasing does not.

**And releasing was not one step.** Making a client runnable can run it,
and one that outranks the thread doing the releasing runs at once, in the
middle of a walk over every channel and every thread. That is the same
defect this stage found in cancellation and fixed there, in code written
the same afternoon, and it was reintroduced twenty lines away. Both are
deferred now.

**5. Kill and restart.** *Built.* A server ended, its client released
rather than hung, another put on the same channel, and a supervisor that
stops trying.

- The kernel holds no restart policy, per section 18.3. What is built
  here is the mechanism a supervisor uses and one supervisor that uses
  it.
- Escalation is what a bound is for: a child that dies immediately every
  time is a configuration mistake, and a supervisor that relaunched it
  for ever would hide one.

*Check.*

```
    its client was released                OWNER_DEAD
    and the next client was answered       OK
    a child that always dies, given up on  3
```

`echoes` is a server with nothing to serve, so that ending it is the
only interesting thing about it, and `orphan` is the client, sent twice
with different expectations. The replacement receives on the same
channel, which is what makes the second client unable to tell there was
a first: a channel outlives the process serving it, and the connection
the client holds names the channel.

**Killing is not cancelling every thread, though it starts by doing
that.** Section 10.5 asks for the threads of the target to be unblocked
exactly as if each had been cancelled. Unblocking is prompt; leaving is
not the same act. A thread released with an error would otherwise come
back into its program and carry on, so the process is marked, and each
thread leaves at its next return from the kernel. Every blocked thread
is released straight into that point, because being blocked is being
inside a system call. A thread computing in user mode reaches it at its
next call, and one that never calls in is not stopped at all, which is
worth saying plainly rather than implying: what section 10.5 asks to be
prompt is the unblocking.

Removing the leaving makes the killed server exit with the code it chose
rather than the one it was given, which is the check noticing that
nothing ended it.

**A spawn handle now carries the right to end the child.** It carried
wait and stat, so a parent could watch a child wedge and do nothing
about it, and section 7.2 would not let it add the right afterwards
because duplication only attenuates. Section 10.5 says a supervisor
holds a handle to the process and none at all to the threads inside it,
which makes the process handle the only thing it could use.

**The supervisor is a program, because the policy is not the kernel's.**
Section 18.3 says restart policy varies per deployment and does not
belong in a kernel trying to stay small enough to analyse. What the
kernel provides is section 10.4's completion timeline; `supervise` is
one policy over it and not the policy. Its bound is the part worth
having: given a child that exits at once every time, it makes three
starts and reports three, and taking the bound out makes fifty-six
before something else runs out. A supervisor that relaunched for ever
would turn a configuration mistake into a system that looks busy and
never works.

**A channel belongs to whoever may receive on it, which is a correction
to what stage four built.** Section 12.5 releases clients when the
process owning the channel terminates, and ownership was whoever created
the object. That cannot be a server: a program learns its handles at
spawn, so a channel it receives on has to exist before it does. Stage
four called that a circle stage six would have to break, and stage five
needed it broken first.

Ownership follows the receive right now, both ways: putting a handle
that carries it into a table makes that process the owner, and dropping
the last one leaves the channel owned by nobody. Section 24.5 already
described the arrangement, which is root creating a channel and handing
the receive end to a server, and this is only that arrangement being
what the kernel believes as well.

Two checks were relying on the old meaning and now say what they meant.
The pager chain check builds a channel whose pager is an ordinary
program, and had to put a receive handle in that program's own table
rather than in the kernel's; without it the pager was the kernel, which
is a valid chain terminator, and the check passed for the wrong reason.
Stage four's abandoned client needed the same.

*Reviewing it found the check killing a server that had never received.*

**The case the stage was about did not happen.** A server ending while it
holds a client's buffer has to give the pages back as well as release
the thread, because the pages are gone from the client's own memory and
a client released into a hole is worse than the wait it was in. The
server ran at the same priority as the thread spawning it, so readying
it did not run it, and by the time the kill arrived it had never reached
its receive: the client was queued with its buffer still its own, and
the unwinding the stage was written for never took place.

The server holds the request now and never answers it, the client sends
a buffer and writes to it the moment it is released, and the check waits
for the client to reach reply-blocked rather than assuming it. The
buffer count says the pages moved: sixteen halves became twenty, which
is the crossing that was missing and the one the second client makes.

**Waited for rather than assumed** is the shape of every one of these.
Three checks in this milestone have now been written against a state the
system reached later than the check looked, and each passed while
testing something else. What they have in common is a program spawned at
the checking thread's own priority, which does not run when it is
readied, only when the checker blocks or yields.

*And one thing tidied rather than fixed.* The supervisor asked for a
thread status on a process handle and fell back to a process status when
that failed with the wrong type. It knew which it had; trying one and
then the other is the shape of code that does not.

**6. Cohort recovery.** *Built.* One member faults, all three end, and a
supervisor starts the pair again on the same channel.

- A fault in any member tears down every member together and a
  supervisor restarts them, per 18.2 and 18.3.
- The member table's pointers are cleared as the members go, so a
  restart can take the places back.
- The slots leave a live address space, which is where M6's TLB
  insurance is claimed.

*Check.*

```
cohort peers: member peer_c faulted, instruction at 0x0, pc 0x81800042
    peer_a goes with it
    peer_c goes with it
    the peer that did not fault, ended     with its cohort
    no member of it is running any more
    and the pair works again, through new processes
```

The middle line is the whole content of section 18.2. `peer_a` was
blocked in receive and had no reason of its own to stop; it is ended
because a peer it shares memory with faulted and nothing in the system
can establish that the peer did not write into it first. Tearing down
only the member that faulted makes the check hang waiting for `peer_a`
to finish, which is the survivor the section forbids, left running.

**A member that goes with its cohort says so.** `SYS_EXIT_COHORT` and not
`SYS_EXIT_FAULT`, because that process did not fault and its fault kind
says so: a supervisor reading the statuses of a torn-down cohort can tell
which member was the reason from which one reports a fault. One exit code
for both would have made the reason unfindable, which is the shape this
milestone keeps producing in the other direction.

**The restart hands out fresh ends, because a handle moves.** The ones
the first pair were given went with them. What the supervisor holds is
the channel, and the channel outlives the processes serving it, which is
the same fact stage five needed and is what lets a restart be invisible
to anything holding a connection.

**M6's TLB insurance is claimed here.** The cohort's address space
survives the teardown, so the restarted member's slot is mapped over the
one the dead member left, in a space its peer is being started into.
That is a mapping replacing a live one, which is the only case the flush
in `proc_load` exists for and the first thing whose correctness depends
on it.

*The check asserted more than the rule promises, and had to be
corrected.* It required every pointer in the member table to be NULL
before a restart. The rule is that a *live* process holds a place: a
member that has exited does not, and the pointer to it goes when it is
reaped, which happens when the last thing that could ask about it lets
go. Asserting the pointer were already NULL was asserting when somebody
else closed a handle, and it failed for that reason rather than for a
reason about cohorts.

*Found while building, and latent.* Reaping a thread did not take it off
its process's thread list, so the list named a freed slot. Nothing
reaches it today: a process whose thread is reaped while the process
lives is one nobody kills afterwards, and killing is what walks that
list. Removing the fix again breaks nothing, so it is recorded as
reasoning rather than as something a check caught.

*What is not done.* The storage cohort is not torn down and restarted.
`fs_server` is in the recorded session, so making it fault would change
the transcript rather than test a mechanism, and what it would exercise
beyond this is a client blocked on the dying filesystem, which stage
five already checks against a server that is killed rather than one that
faults with a peer.

And the memory does not come back. The restarted member is loaded over
the slot the dead one left, so the old frames leak, exactly as they leak
for every process since stage one. Freeing them is the piece stage one
named and deferred, and a cohort makes it no harder: the difference is
only that the space is shared, so the slot goes and the space stays.

*Done when* killing the filesystem server mid-read releases the blocked
client with an owner-dead error rather than hanging it, a supervisor
restarts the server, the client's next walk and read succeed against the
replacement, a fault in one cohort member restarts the whole cohort, and
the resource report's high-water reflects what is alive rather than what
has ever run.

**Done.** All six stages are built. Section 10 says what it answered,
what it cost, and what it left untested.

*Would falsify.* Whether "the disappearance of the only party who could
satisfy a wait is itself an event" is sufficient in practice, which is the
claim the whole error state of section 9.3 rests on. Whether a restarted
server leaks per-connection records, which is the observation
`PROPOSALS.md` gates connection death notification on. And whether the
generation counters actually close the recycling hazard under real reuse,
which nothing has yet tried.

*Watch for.* Teardown wakes things. Closing a dying process's handles drops
signal rights, which puts timelines into the error state, which releases
their waiters, all from inside the teardown. That is the same re-entrancy
that produced three separate faults in M1, and it will be worse here
because the structure being walked is the one being destroyed. Finish the
structure before waking anything, as `handle_close` already has to.

And watch for the rule attached to the wrong event, which M6 got wrong
twice and caught late both times. Reaping is not "it exited". Cohort
teardown is not "the process ended", it is "a thread faulted", which is
where M6 finally put it.

*Size.* Large. The original estimate of medium predated knowing that
pulses, reaping and donation unwinding were all inside it, and cohort
recovery, the buffer unwind and the live-space teardown have since
joined them.

### M8. Multicore

**Partitioned scheduling with explicit migration.**

**Done.** All six stages are built. Section 11 says what it answered,
what it cost, what the building kept finding, and what it left untested.

*Needs.* Per-core ready queues with no global scheduler lock (11.4). Explicit
migration rather than automatic balancing. Cross-core priority donation via
inter-processor interrupt.

**Tier 2 only, which section 13.3 already decided and this entry did not
say.** Tier 0 disables interrupts inside the kernel and bounds latency by
where the preemption points are, on the argument that such cores are
simple and often uniprocessor. Tier 2 enables them and locks instead,
because multiprocessing forces internal locking regardless. So a second
core belongs to one tier, and the four configurations stay four: two of
them keep exactly the system M7 finished.

*What M7 leaves for this milestone, and it is more than a warning.* Its
retrospective says every ordering argument so far is about one core with
interrupts disabled and a deferral counter. Here is the list, because a
list can be worked through and a warning cannot.

**`md_intr_disable()` is being used as mutual exclusion.** It appears in
the rendezvous, the scheduler, the thread paths and the timer, and on one
core it is exclusion because nothing else can be running. On two it
excludes an interrupt handler on this core and nothing at all on the
other. Every one of those regions needs a lock, and interrupt disabling
keeps only the job its name describes.

**`sched_defer` is one counter for the system, and says so.** Its comment
explains that it is not per thread because it exists to make a short run
of readies atomic and never spans a switch. Per system is per core the
moment there are two, and the runs it makes atomic are exactly the ones
M7 found being interleaved.

**`reap_parked` is one slot**, justified by a switch not being able to
begin before the last one finished. Two cores switch at once.

**Reference counts are plain increments.** `p_refs`, `t_refs` and
`cn_refs` decide when an object is freed, and M7's whole lesson was that
a freed object turns every pointer to it into a way of naming its
replacement. A racing decrement makes that a use-after-free rather than a
stale read, which is the same defect with the timing removed.

**`current`, `current_as_root` and the counters are globals.** The first
two are per core by definition. The counters are measurements, and a
measurement that loses increments is worse than one that is absent,
because it looks like an answer.

*What the plan gets wrong until this is built.* The recorded session is
compared exactly, and three milestones have now had to move a number out
of it for being timing-dependent. Partitioned scheduling is what keeps
this tractable: nothing runs on the second core until something is put
there, so a check that does not migrate anything is a check that cannot
see one. The compared file stays exact for as long as placement is
explicit, and the first stage that migrates something has to say what
became of that.

*And what M7's checks will do here.* Several of them wait for a program
to reach a state by yielding until it does, because a program spawned at
the checking thread's own priority does not run when it is readied. On
two cores it may already be running. Those loops become genuine waits
rather than no-ops, which is fine; what is not fine is any check that
still assumes the yield is what let the other program run.

*Stages.* Six. The first two are about being able to run at all on two
cores, the middle two are the milestone's actual subject, and the last
two are the measurement and the question.

**1. A second core that runs nothing.** *Built.* Two cores at Tier 2, one
of them idle with an empty queue, and the recorded session identical in
all four configurations.

- Bring-up on both ports at Tier 2: the RISC-V harts arrive together at
  the reset vector because this kernel has no SBI beneath it, and the
  AArch64 secondaries are started with the PSCI call the port already
  uses to power the machine off.
- Per-core state, reached through a register rather than a global: the
  running thread, the address space it is in, the deferral counter and
  the slot a dead thread is parked in.
- The second core runs an idle loop over an empty ready queue and takes
  no lock, because there is nothing yet for it to take one for.

*Check.*

```
cores: 2 of 2
    core 0: up, nothing running
    core 1: up, nothing running
```

and the transcript does not move, which is the whole point of a
partitioned system with nothing migrated. Giving the second core the
first core's state rather than its own fails at once with `cpu 1:
claimed somebody else's state`, which is the two of them agreeing about
which thread is running and is the one thing they must never do.

**The two ports start a core in opposite directions.** There is no
firmware under the RISC-V kernel, so every hart arrives at the reset
vector together and the boot hart's problem is holding the others back;
starting one is releasing it from a wait. The AArch64 secondaries are
held powered off by the platform and are started by asking for one by
name, through the same PSCI interface the port already used to power the
machine down. Both end in the same place, which is a core with a stack,
a vector table and a translation of its own.

**The handshake cannot live in `.bss`.** A parked hart writes its
presence at the same moment the boot hart is zeroing that section, so a
word there is a word the boot hart erases just after it is written, and
the kernel counted one core. It is in `.data` now, which the image
carries already zeroed and nothing rewrites. This is the only piece of
memory in the system that two cores touch before there is a kernel.

**Four globals became four fields.** The running thread, the address
space root, the deferral counter and the parked-thread slot were each
justified in a comment by there being one core, and each is now found
through `tp` on one port and `TPIDR_EL1` on the other. Reading them
costs one instruction and no agreement with anybody, which is what
section 11.4 needs: a scheduling decision that had to be agreed is not
a decision that decomposes per core.

*The idle count is one, and that is the right number.* The stage entry
predicted a rising count. Nothing is routed to a core with an empty
queue, so it goes round once and waits for an interrupt that does not
come. A large number would mean an idle core spinning and taking memory
bandwidth from the one doing the work, so the check asserts it moved at
all and prints it outside the compared file, where numbers about the
host belong.

**A core that cannot be found is not used, and that had to stop being
quiet.** Removing the line where a hart says it is there left the kernel
counting one core and passing every check it has, which is the failure
M7 ended on: a check passing while the thing it tests did not happen.
The emulator and the kernel are configured together here, so fewer cores
than the build asked for is a build mistake, and it stops the boot with
`cores: 1 answered, and this build is for 2`.

*Reviewed.* The review looked for the thing this stage is most likely to
get wrong, which is not the mechanism but what the mechanism assumes
about numbers the hardware chooses.

**Every index came from the hardware and none of them was checked.** A
core arriving says which one it is, and that number indexed three arrays
in two languages: the presence and release words in assembly, the stack
array, and the per-core structures in C. All four indexings trusted it.
A build for two cores on a machine with four is not an exotic case, it
is one flag, and the layout makes it specific: the presence array is
followed immediately by the release array, so hart three saying it is
here is hart one being released, before there is an address to release
it to. That machine now prints nothing at all. With the bound in place
it boots and reports two cores, and the extra ones park having touched
nothing. The C side got the same treatment: claiming a core's state
checks the index rather than trusting that assembly already did.

**The word that says a core is up was published without ordering.** Both
of these architectures order memory weakly. A core filled in its own
state and then set the flag another core spins on, with nothing between
them, so the reader was entitled to see the flag before the state it
describes. Nothing failed, because what the reader goes on to read
happens to be zero either way, which is exactly the kind of correctness
that stops holding in the stage that adds a field. There is a fence on
each side now. The two fields one core writes and another reads are
read every time rather than once into a register: the spin worked only
because the relax hint happens to tell the compiler memory changed.

**The count of cores was a race with a comfortable margin.** Presence
was sampled once, and a hart that had not stored its word yet was
counted as absent. It always had, because the boot hart spends the
interval zeroing memory, so the answer depended on how much memory this
kernel has. That is a number with nothing to do with how many cores
there are, and the stage had just made a wrong count fatal. It waits
for each hart now.

**And an argument one port honoured and the other discarded.** Starting
a core took the entry point to start it at. The RISC-V port passed it
through; the AArch64 port cast it to void and went somewhere of its
own, because a core arriving from firmware has no stack and no vector
table and cannot enter C. Both were right about their own machine and
the interface was wrong to ask. It is gone, and where a core arrives is
each port's own business up to the point where it enters the kernel.

**2. One lock, and interrupt disabling stops being exclusion.** *Built.*

- A spinlock, and one of them held across every entry into the kernel.
  Sound, small, and a bottleneck: what it buys is that the rest of this
  milestone is about scheduling rather than about auditing seven
  thousand lines for shared state.
- Section 11.4 forbids a global *scheduler* lock and says nothing about
  the rest, which is what makes this a legitimate resting place rather
  than a shortcut. Whether anything beyond the scheduler needs breaking
  out is a measurement, and measurement is late by section 2.

*Check.* Two cores each running a thread that hammers the same kernel
path complete without corrupting the object they share, and the object's
own invariant check passes afterwards. Taking the lock recursively is
refused rather than deadlocking silently, and releasing one that is not
held stops the machine.

*Built.* There is one lock, taken at every entry into the kernel from the
port's exception vectors, and there are four of those: a system call, a
fault taken by a program, a fault taken by the kernel, and an interrupt.
Each is a pair of calls in C rather than anything in assembly, which is
worth saying because the alternative was editing four exception vectors
in two ports.

**Two cores through one region, 4096 rounds each.** Nothing is scheduled
on the second core until the next stage, so the two cores that hammer the
lock are the boot core and the one on its way to the idle loop. The
region carries a flag saying a core is inside it and a count with a
shadow written from a value read at the top, so a lost update and an
overlap are separately visible. Taking the lock out of that region fails
on the first round with `two cores inside the region at once`, which is
the answer arriving immediately rather than after a rare interleaving:
the region widens its own window on purpose, because a race that needs
an exact interleaving to show is a race this check would report as
absent.

**The lock is put down at a context switch and around the idle wait
rather than carried across either.** The first shape tried was the other
one, where a thread that blocks hands the lock to whatever runs next and
that thread releases it through an exit pairing with an entry it never
made. It is a real design and this kernel cannot have it, because a core
holding the lock has interrupts disabled and the idle path exists to wait
for an interrupt. Putting it down is also simply better: a thread that
blocks has already put its state in order, so the region does not need to
survive the block.

**And the kernel's own threads hold nothing, which is a hole and is
recorded as one.** A kernel thread never entered the kernel, it started
inside it. The boot thread runs several thousand lines of kernel work
with interrupts enabled, and the lock cannot be held with interrupts
enabled, so it cannot hold the lock. What this bought is exact rather
than complete: every entry from user mode is exclusive, and the kernel's
own thread is not. That is sound while nothing is scheduled on the second
core and stops being sound in the stage that schedules something there,
which is the next one.

**What interrupt disabling still does.** The seven subsystems that
disable interrupts around their critical sections still do, and the
reason changed underneath them without a line of theirs changing. A
handler runs on the core it interrupts, so disabling still keeps a
handler out of a region a thread is inside. It never kept the other core
out of anything, and the comment in `sys/intr.h` that said it did is
corrected rather than deleted: the sentence was true for two milestones
and is now half true, and which half it lost is the point.

**Both misuses stop the machine, and the first thing the lock ever said
was the wrong thing.** Taking it recursively gives `lock kernel: taken
again by the core already holding it`, and releasing one not held gives
`lock kernel: released and not held`. Both said `lock (null)` at first,
because the lock was a static that nothing had initialised, and a lock
that cannot say which lock it is answers half of the one question it
exists to answer. It is named where it is declared now, because an
initialiser that has to be called is one that can be forgotten.

*Reviewed.* The stage wrote a claim into a comment and the review tested
it, which is the only reason it was found.

**Kernel entries nest, and the comment said they could not.** The
interrupt state a core entered the kernel with was kept in one slot per
core, justified by there being no nesting: interrupts are off while the
lock is held, so a core inside the kernel cannot reach a second way in.
Counting outstanding entries says otherwise, two deep, within a second of
booting. The path is the one this stage built: a thread blocks, reaches
the idle wait, and the idle wait puts the lock down and enables
interrupts on purpose, so the interrupt that arrives there enters the
kernel inside an entry that has not left. The inner entry overwrote the
outer one's saved state.

Nothing had gone wrong, and the reason is worth stating because it is not
a reason to keep the arrangement. Every entry to this kernel is from a
vector that has already masked interrupts, so every state saved was the
same state, and an overwrite with an identical value is invisible. The
state is returned to the caller and handed back now, which is one slot on
a stack that already exists per entry by construction, and the comment
that was wrong is replaced by the account of how it was wrong.

**Three functions had no callers.** The lock's initialiser, because the
stage had just moved to naming the lock where it is declared; the
assertion that a core holds the lock, because the design it belonged to
was the one that carried the lock across a context switch and was
replaced; and the two predicates, which are used only by the lock's own
code. The first two are gone and the last two are internal. A constructor
with no caller is a thing the next stage will call by habit rather than
because it needs to.

**And the whole spinlock is Tier 2 only now.** It compiled at Tier 0,
where nothing calls it, because the file was written before the tier
question was settled. Section 13.3 buys that tier a kernel with no
internal locking at all, and a lock that exists there is a lock somebody
can use there.

**What the review confirmed rather than changed.** Asserting that the
lock is held at the top of the system call body and the interrupt body,
and running the whole recorded session, gives no failure in any of the
four configurations: the lock is genuinely held on every entry, not only
in the check that hammers it. The one window left is known and belongs to
the next stage. A switch sets the current thread of this core before
dropping the lock, so there is an instant where another core could read a
thread as current on a core it is not yet running on. Nothing reads
another core's current thread yet, and the stage that puts work on the
second core is the stage that makes it readable.

**3. A ready queue per core, and threads that stay where they are put.** *Built.*

- Each core dispatches from its own queue and takes no lock the other
  core could be holding. That is section 11.4's requirement and the only
  part of the big lock this milestone is obliged to undo.
- `sys_thread_migrate()` moves a thread to a named core. Nothing moves on
  its own, so where a thread runs is a fact about the program rather than
  about load.

*Check.* A thread migrated to the second core runs there and says which
core it ran on, and the first core's queue never held it. A thread that
is not migrated never runs anywhere but where it started, over a run long
enough for a balancer to have moved it if there were one. And the
dispatch path is checked for what it does not do: a counter of lock
acquisitions on that path, asserted at zero for locks another core could
hold.

*Built.* The queues and the bitmap moved from statics in the scheduler
into the per-core structure, which is the whole of the mechanism. A
thread carries the core it belongs to, fixed when it is created and
changed only by `sched_migrate`, and the enqueue path routes by it: onto
this core's queue directly, or onto the owning core's arriving list with
one compare and exchange.

**The arriving list is what makes the decision lock-free, and it exists
because a lock here would be the wrong lock.** Putting a thread on
another core's queue under a lock means that core takes the same lock to
dispatch, which is a core waiting on a decision another core is making.
That is exactly what section 11.4 says a per-core queue is for avoiding.
One exchange takes the whole list, so a core sending while the owner
drains simply starts a new one and nothing waits for anything. The order
of arrival is not preserved and is not a scheduling decision: each
thread goes on the queue its priority names, and section 11.2 is where
order comes from.

**The claim that dispatch takes no such lock is a counter and not a
reading of the source.** Every acquisition of a lock another core could
hold is counted, and the count is sampled either side of the decision
and asserted unchanged. Making the decision take the kernel lock fails
with `the decision took 1 locks another core could hold`.

That counter was wrong the first time in a way worth recording, because
it was wrong in the direction that hides nothing and cries wolf: one
counter for the system counts the other core's locks as well as this
one's, so a decision taken here failed because a system call was made
there. It fired within a second of the check session starting. A claim
about what this core's path does needs a number about this core.

**A second core needs something to be before it can dispatch.** Section
11.6 has idle be a state of the core rather than a thread, and
dispatching still means switching away from something. So each core
bootstraps a context of its own in the kernel's process, at the lowest
priority, which offers the core back at every turn. Anything migrated
there outranks it and runs. That is one more thread at Tier 2 and the
resource file says so; the recorded session is unchanged, which is the
point of a partitioned system.

**It spins, and that is the shape of the hole the next stage fills.** A
core waiting on `md_cpu_wait` is woken by an interrupt, and the interrupt
that would say something arrived for you is the inter-processor one that
stage four builds. Until then a core with nothing to do looks at its
arriving list instead of sleeping. Stage one recorded that an idle count
of one was the right answer; it is millions now, and it will be one
again when there is something to wake it.

**Two checks, and the failing case had to be made to fail properly.** A
thread migrated before it was ever runnable runs on the second core and
says so, and the bitmap of cores it was ever queued on holds only the
second: the claim is that the first core never had it, which is a
question about history rather than about now. A thread nothing migrates
takes two thousand turns and every one is on the core it started on.

The first version of the migration check waited by spinning, and when
migration was disabled to see it fail, it hung instead of reporting.
A thread that was not migrated sits on this core's queue behind a thread
that never yields. Waiting by yielding, at the priority the migrated
thread has, turns that silence into `it ran on core 0` followed by the
panic that says so. A check whose failure mode is a hang is a check that
has not been read.

*Reviewed.* Four findings, and the one worth the most is that the same
mistake was made twice in one stage.

**A counter that measures one core must live on that core, and this was
got wrong again.** The stage had already found it once: a system-wide
count of locks taken reported the other core's system calls as this
core's dispatch, and was made per core. Then the review added a count of
scheduling decisions to check that readying a thread elsewhere takes
none here, made it a global, and it reported thirty-one decisions across
a call that takes none. The other core is dispatching the whole time.
Two counters, the same error, minutes apart. On a machine with two
cores, a counter is per core until there is a reason it is not.

**A core that goes idle never looks at what arrived for it.** The idle
loop waits and then tests the ready bitmap, and a thread pushed here by
another core is on the arriving list and on no queue, so the bitmap
knows nothing about it. The core goes back to sleep with work waiting.
The drain belongs in that loop and is there now.

This one is not fully closed and the reason is the next stage. Draining
after waking is correct; being woken is what is missing, because the
thing that would say something arrived for you is the inter-processor
interrupt. Until then such a core sleeps until some other interrupt
happens to arrive. So this is half a fix, and the half it has is the
half that stops being wrong once the other half exists.

**Readying a thread on another core preempted this one.** The
preemption rule compared priorities and nothing else, so a thread made
runnable on the second core stopped a thread running on the first for a
thread that was never going to run there. Section 11.4 makes preemption
a per-core question because the queue being competed for is per core.

Showing this took three attempts and the first two proved nothing. The
wasted decision chooses the running thread again, so removing the guard
changed no output at all: what it costs is invisible in what ran. It is
visible as a count of decisions taken, and with the guard removed
readying that thread takes one decision on a core it has nothing to do
with. The cost is not only the decision. A thread at the same priority
as the running one would take the core from it, so an unrelated core's
activity would reorder this core's queue.

**And the stage named an operation it did not build.** The plan said
`sys_thread_migrate()` and what was built was the kernel call under it,
which the check reached directly. It is an operation now, opcode and
argument structure and a wrapper, reached with a handle carrying WRITE
because where a thread runs is authority over it rather than a question
about it. The check goes through the operation, which found that it had
been leaving a handle in the table: a later check asserts that table is
empty, and a check that leaves something behind fails a check that has
nothing to do with it.

**4. Donation across a core boundary.** *Built.*

- A donating core raises the target's inherited priority and sends an
  inter-processor interrupt; the target's core reschedules on receiving
  it. Section 11.4 is explicit that this is the mechanism.
- The chain bound of section 12.3 still applies, and a chain that crosses
  a boundary twice is still one chain.

*Check.* A client on one core sends to a server pinned to the other, and
the server runs at the client's priority rather than its own, which the
server reports. Dropping the inter-processor interrupt leaves the server
at its base priority until something else happens to reschedule that
core, and the check sees the priority rather than the eventual delivery,
because a donation that arrives late is a donation that did not work.

*Built.* A server based at priority 4 and pinned to the second core
serves a client at 9 running on the first, at 9, and drops back to 4
when the reply undoes it.

**The donation does not ride the interrupt, and the stage predicted it
would.** The plan said dropping the inter-processor interrupt would
leave the server at its base priority until something else rescheduled
that core. It does not. A server is blocked at a receive when its client
sends, which means it is on no core's run queue, and a thread on no
queue can have its priority written by whoever is holding the
rendezvous. The priority is set before the thread is handed over. What
the interrupt carries is the wake.

So the check that was described cannot be written, and what replaces it
is stronger. Dropping the interrupt does not degrade the server, it
strands it: the second core is in the idle wait and nothing else is
going to arrive, so the thread never runs at all and the earlier check
fails first with `the migrated thread never ran`. Breaking the donation
instead of the interrupt gives `served at 4 and the client was at 9`.
Two failures, two mechanisms, and they are separable, which they would
not be if the priority travelled in the message.

**The port that has no firmware had to be its own.** A hart is poked by
writing its word in the core-local interruptor, and that raises a machine
software interrupt, which is one of the two kinds that cannot be
delegated: the machine-mode bits of the delegation register are read-only
zero however hard the boot code writes ones into it. So the poke lands in
a mode the kernel has left and does not return to. Machine mode now
catches it, takes it back down, raises it again for supervisor mode and
returns, which is exactly what an SBI implementation does with the same
interrupt. This kernel is its own, and the three registers it borrows to
do it come from an area per hart, because two harts can be in there at
once.

**The other port needed the opposite thing split apart.** Its
distributor is one device for the machine and its CPU interface is one
per core, and both were configured in one function called once. A core
that never ran that half receives nothing however well the machine is
set up, so the per-core half is its own function and every core calls
it on the way in.

**The second core stopped spinning.** Stage three had it offering the
core back in a loop, because there was no way to wake it. It blocks now
and never becomes runnable again, so the core falls into the idle wait
and stays there: section 11.6 taken literally, with the context existing
only to be the thing the core dispatches away from. Stage one predicted
an idle count of one and got one, stage three made it millions, and it
is five and six now on the two ports.

**What is still guarded rather than solved.** Raising the priority of a
thread that is queued on another core still stops the machine, because
moving it between that core's queues is that core's business. Donation
never reaches it: a server at a rendezvous is blocked, not queued. The
stage that needs it is the one that donates to a thread that is already
runnable somewhere else, and nothing does yet.

*Reviewed.* Three defects, and each one is a thing that worked because
the machine was being kind about it.

**The thread was read after it had been given away.** Publishing a
thread to another core's list and then naming the core from that
thread's own field is reading a slot that is no longer this core's. The
exchange is the moment it changes hands, and the other core may already
have taken it, run it, ended it and given the slot to something else. It
is named from the pointer taken before the exchange now. Nothing had
gone wrong, because the field being read holds the value that would
have to change for it to matter and the window is a few instructions
wide, which is the description of a race that shows up on hardware
faster than this and never here.

**Acknowledging a software interrupt with the wrong sender.** On the
port with a distributor, the same interrupt number from two different
cores is two different interrupts, and finishing with one means saying
which. The claim discarded that field and the acknowledgement wrote the
bare number, so every poke from the second core to the first was
acknowledged as though the first had sent it to itself. The check ran
anyway because the emulator is lenient. It is not theoretical: counting
the pokes shows five from the first core to the second and three the
other way, so it happened on every reply that woke a client.

The whole value the controller gave is kept now, per core, because two
cores are in that path independently. That is the third time this
milestone that the answer to a bug has been to stop keeping one of
something for the machine.

**And the port that stopped being able to say what went wrong.** The
machine-mode handler borrows three registers through a scratch register
holding a per-hart area, and the boot code pointed the trap vector at
that handler before setting the scratch register up. A machine trap in
that window writes through whatever reset left, traps again, and loops
with nothing printed. That is word for word the failure the comment
above those lines says the handler exists to prevent, and the stage put
it back. The area is set first now, then the vector, then the interrupt
enabled, in that order and for that reason.

**What was checked and left alone.** The interrupt controller on the
other port still addresses its external sources to the first hart's
context, which is wrong for any core but the first. Nothing reaches it:
a second core unmasks the poke and nothing else, so it never takes an
external interrupt. It is a limit of what has been built rather than a
defect in it, and the stage that gives a second core a device is the
stage that has to answer it.

*Since fixed, after M10 closed.* Every per-context register is
addressed from the core asking now: a context is a hart and a privilege
level together, so hart H uses 2H+1 for its supervisor mode. The
threshold moved from the one-off setup into the per-core half, because
a core whose threshold nobody lowered is enabled for a source and never
delivered one. A second core writes a value nothing else uses into its
own threshold and reads the first core's back to check they are not the
same register, which the old arithmetic fails with `core 1 shares the
first core's context`.

*And what that fix found.* The panic proving the check works arrived
unreadable: `panic: pl4096 on ic: core co1 shre 0ares t anhed first
core's context`. Two cores writing the console at once produce a line
neither of them wrote, because nothing kept one core's message together.
A break test made both cores print the same fixed line three hundred
times each: without exclusion, 277 of 304 lines came out woven, and the
count itself is wrong because weaving destroys the line boundaries the
count is made of.

The console now holds one core at a time for the length of a message
rather than the length of a character, which is the unit because it is
what a reader reads. Messages nest, since a panic prints a prefix, a
body and a newline and the formatter prints each piece of what it
formats, so the outermost call takes the lock and a depth in the core
knows which one that was. The same test with it in place: 600 lines, all
of them one of the two.

The panic path is the one that cannot simply wait. A core that stops the
machine must say why, so waiting forever would lose the message
altogether if the core holding the lock is the broken one, and that is
the case a panic exists for. It waits a bounded time and then prints
regardless. Printing immediately was tried first and reproduced the
original symptom exactly, because the case that actually occurs is not a
wedged core but an ordinary core partway through an ordinary message.

*What reviewing it found.* Holding a message across a whole line means
printing with interrupts off, and that made the console the longest
interrupts-disabled region in the system: on riscv64 at Tier 2 the
worst went from 714 microseconds at `channel_reply` to about 1100
inside the console, and the mean of every region on the core went from
49 ticks to 117. At Tier 0 the mean went from 16 to 111 microseconds.
Nothing in the kernel had changed. Section 13.3's headline figure had
quietly become a measurement of the UART, which is the one thing M9
said a figure must not be allowed to do.

Disabling interrupts is not the part to undo. A core preempted while
holding the console lock is a core whose next thread spins for a lock
its own core will not release until it runs again, and at Tier 0, where
there is no lock at all, preemption mid-message weaves two threads
together on one core just as two cores did. What was wrong was the
reporting, not the exclusion. The report now gives the longest region
and the longest that is not a console message, and the second one is
back at `channel_reply` in all four configurations at 469 to 602
microseconds, which is where M9 left it.

*And what reviewing the panic path found.* Two defects, both in the
part of the kernel that exists to explain the others.

An unhandled kernel trap reported the wrong thing entirely. `trap_fatal`
opened by taking the kernel lock, and it is reached because kernel code
faulted, and kernel code faults while it is inside the kernel holding
that lock. Taking it again is the one thing `spin_lock` refuses, so a
kernel fault raised inside a system call printed `lock kernel: taken
again by the core already holding it` and nothing about the fault: no
cause, no address, no thread name. Forcing a store to address 1 inside a
system call showed it on both ports. The lock was never needed there.
It reads per-core state and stops the machine, and the message is held
together against the other cores by the console. With it gone the same
break test prints `unhandled trap in spawner: page, cause=0xf pc=...
addr=0x1`. At Tier 0 the lock was a no-op that did not even disable
interrupts, so the report was also open to being preempted.

A panic that faults inside its own message never stopped. The thread
name in that report is read out of a structure the fault may be the
first evidence of having corrupted, and printing it can fault, which
re-enters `panic`, which prints, which faults. Handing the report a
pointer to address 1 produced twenty-five kilobytes of `panic:
unhandled trap in ` and no halt until the emulator was killed. A core
now records that it is panicking, and the second entry prints `panic
while panicking` and stops. Per core, because a second core with its own
reason to stop has something worth saying.

*And the intermittent aarch64 failure, which was this check.* `make
check` timed out about one run in twenty on aarch64 at Tier 2, always
after `user mode: a program that can be interrupted` and never with a
message. Catching one under a debugger while it was still alive
answered it: the machine was at full CPU with the `spin` program on a
core at priority 20, and the millisecond timer this check arms had
fired 68,713 times, then 72,819 four seconds later. It was servicing
1026 interrupts a second and the program it was interrupting was
advancing by nothing.

A period is a rate the host has to keep up with. When the guest cannot
service an activation before the next comes due, it never returns to
the interrupted thread at all, and since this check prints nothing
until it finishes, a machine working as hard as it can looked exactly
like a machine that had stopped. Two things made that hard to see: the
output was block-buffered, so where it stopped was not where it stopped,
and the first CPU figure taken was of the `timeout` wrapper rather than
of QEMU, which read 0% and argued for a sleep rather than a spin.

The check needs one interrupt to arrive while a program is running, not
a thousand a second. It arms a single activation now, ten milliseconds
out, inside the deferral it already held around the spawn, so it is
armed before the child has had the core and comes due while it has it.
One activation cannot outrun the machine however slow it is. Both
figures stretch together when the host is busy, which is what makes the
ten milliseconds hold: the program is eighty milliseconds of work.

The rate at which this reproduces is not under my control, so the fix
is not proved by putting the period back: 392 sessions with the old
code and the same load produced nothing, having produced two hangs in
under 250 a few hours earlier. What is settled is the mechanism, which
was observed directly, and that a single activation cannot exhibit it.

*Reviewing that fix.* Two things it got wrong and one it got right by
luck. The right one first: with the one-shot removed entirely the check
sees no interrupt at all, and with it back it sees exactly one per try,
so the timer really is what interrupts the program and the deadline
really does land inside its run.

The margin was asserted from a number nobody had measured. Instrumenting
the loop puts the child's run at seventy-two milliseconds on one port
and seventy-eight on the other, against a shot armed ten milliseconds
out, so it lands with about seven times the room at the far end and far
more at the near one. That is what the comment now says, because it is
what was measured.

The defect is that the timeline outlives the timers bound to it.
Destroying a timer drops the kernel's signal right, and dropping the
last one puts a timeline into the error state for good, so from the
second try on the timer was bound to a timeline nothing could advance.
It never showed, because this check counts interrupts and not values,
which is the kind of quiet wrong worth not leaving in. One signal right
is now held for the whole check. Its value still stops at one, and that
is inherent rather than a fault: a one-shot signals activation one, so
the next try offers a value the timeline already has and is refused for
going backwards.

*Reviewing the console lock a second time.* The exclusion itself holds
under a harder test than the first one: two cores printing through
different entry points, `cnprintf` with conversions against `cnputs`,
at different lengths. 600 lines, 300 from each core, none malformed.
The same test with the lock taken out gives 129 malformed lines of 446,
so it is sensitive to what it is checking.

What the first review left half done was the measurement. It taught the
run-wide figure to report the console separately and left the windowed
figure, the one a check clears and reads around a stretch of code,
counting console regions like any other. Nothing measured prints inside
its window today, so it had not bitten, but that figure is the one that
is asserted on rather than reported, and a console region inside a
window is thousands of times every kernel region in it. It would fail a
check that was right or pass one that was wrong. It is excluded there
too now.

*And something else that was already there.* Verifying that change
turned up an intermittent failure in the batch latency check that has
nothing to do with the console. It asserts at Tier 0 that the sixteen
entry figure is no more than four times the one entry figure, which is
a duration compared against a threshold, and its comment claims the
margin is an order of magnitude wider than the effect. It is not. Under
eight machines at once the one entry figure ranges from 48 to 86 ticks
and the sixteen entry figure from 45 to 204, and a run was seen at 231
against 54, which fires. The distributions are identical with and
without the change above, so this is not new. It is the same rule this
plan keeps rediscovering: a count is safe to assert and a duration is
not, and this check asserted a duration. Fixed next, by counting the
regions in the window instead: the count is what the claim was really
about, and it does not move.

**5. What two cores did to the measurements.** *Built.*

- Every counter this system prints is a plain increment on a shared word.
  Some are per core by nature and some are not, and the ones that are not
  need saying so.
- The compared file has to stay exact or stop containing the number, and
  M6 stage four settled which of those to do.

*Check.* The paced and burst runs agree, in all four configurations, on
everything the check compares. Anything that stops agreeing moves out of
the compared file and into the report above it, with the relation that
does hold checked in its place.

*Built.* The audit sorted every printed number into two kinds, and the
kinds turned out to matter more than the numbers.

**The ones that are per core by nature are per core now.** A page table
is written by the core walking it, a translation is thrown away by the
core holding it, and an address space switch is a core switching. All of
those were one set of words for the machine, which was the same thing as
per core while there was one core. Two cores incrementing one word lose
an increment whenever they land together, and six of these are in the
part of the report the check compares exactly. A number that is usually
right is worse than no number, because it makes a check fail sometimes,
and a check that fails sometimes gets deleted.

They are added up when the report is read, and the parts are printed
beside the whole: 418 switches, 417 on the first core and 1 on the
second, with the sum checked against the total and each part asserted
nonzero. A sum whose parts nobody looks at is a number that would go on
looking reasonable if one core stopped contributing.

**The one that is not per core by nature needed a lock, and needed it
for something other than the number.** A high-water mark is a
largest-ever over everything live at once, which does not decompose per
core the way a count of events does. So the pools keep one of each, and
they are kept under a lock.

What the lock is actually for is the free list. Two callers taking the
head both get the same slot, which is a failure that has nothing to do
with counting. Two cores at one pool, 4096 rounds each, gives 8192 with
every slot returned; without the lock it fails in the first few with `a
slot that was taken would not go back`, on both cores at once. The
system's own pools are reached from both cores already, but twice in a
whole session, which is a lock needed by construction and tested by
nothing, so the check hammers a pool of its own. It is not a scheduler
lock: nothing under it decides what runs, and the dispatch path's
existing assertion that it takes no such lock still holds, because a
slot is given back before that assertion begins rather than inside it.

**And the audit found something that is not a counter at all.** The
check began failing about one run in ten, on one port, with `nothing
interrupted the program while it ran`. There is one list of pending
events and the timer hardware is per core, and arming was done by
whichever core happened to change the list. A timer armed on the wrong
core still fires. What it interrupts is a core that has nothing to do
with the deadline, so the event the check was counting arrived on the
idle core, where it is an interrupt taken in the kernel rather than one
taken from a program.

Arming names a core now, and a core that changes the list and is not
that core asks it rather than arming itself, through the poke this
milestone already has. Eighteen consecutive runs at Tier 2 across both
ports after the fix, against one failure in ten before it.

That is the milestone's own lesson arriving in a fourth place. Three
times the answer has been to stop keeping one of something for the
machine; this time the thing being kept for the machine was right and
what was wrong was assuming the hardware under it was too.

*Reviewed.* Four findings, and the first one is the worst thing this
milestone has produced.

**A program could steer the kernel's per-core pointer, and one
instruction was enough.** Stage one put that pointer in `tp` on the port
where `tp` is an ordinary register, and nothing on the way into the
kernel put the kernel's own value back. A program that writes to `tp`
and then takes any trap has the kernel read its per-core state through
an address the program chose: the running thread, the deferral counter,
the counters this stage was auditing. A program with `li tp, 0x41` in it
stops the machine dead at its first interrupt, with no panic, because
the panic path reads the same pointer.

The other port never had this. `TPIDR_EL1` cannot be written from user
mode at all, which is why it was the obvious register there and why the
same choice on the other port needed something the obvious one does not.

The frame carries both pointers now. The program's value is saved on the
way in and given back on the way out, so a program that uses that
register keeps it, and the kernel's is parked in a word below the
thread's kernel stack on the way out and loaded back on the way in.
Below the stack rather than anywhere else because it has to be reachable
without already holding the register it restores. A program that starts
gets a cleared register rather than a kernel address, which is the same
hole in its smaller form.

**The check that was supposed to catch two cores holding one slot could
not fail.** It wrote a mark into the slot and compared it against what it
had just written, through an ordinary member, which the compiler is
entitled to fold away and does. It is a volatile read with a gap before
it now, and the gap is on purpose for the same reason the lock check has
one. In practice the free list gives way first, so what actually reports
the failure is the slot that would not go back, and the mark is the
second witness rather than the first.

**The lists the timer keeps are the pools' sibling and were left
unlocked.** This stage locked a free list reached from both cores and
left, in the file it was editing, two ordered lists reached from both
cores in exactly the same way: any core can arm a timer, destroy one or
sleep, and the core that owns the hardware walks both when it fires.
They are held now, across the whole pass and put down before anything
can switch away, which the deferral already in that pass is what makes
possible.

**And a read that answered nothing said nothing.** Asking for the
counters of a core that does not exist left the caller's structure
holding whatever it did before. It is zeroed.

**6. Whether partitioned scheduling is usable.** *Built.*

- The milestone's own question, and the only stage whose outcome could
  delete a section.
- What would answer it is a workload placed by hand that a balancer would
  have placed better, and the difference stated.

*Check.* The storage stack is pinned across two cores, with the
filesystem on one and the block store on the other, and a read still
works. What it costs is a number: the inter-processor interrupts and the
donations that a single core did not need. If placing them by hand is
obviously wrong and no rule of thumb helps, that is the answer section
11.4 asked for.

*Done when* threads run pinned across cores, a donation chain crosses a
core boundary and is honoured, no scheduling decision takes a shared
lock, and the recorded session is identical in all four configurations
with two cores running.

*Would falsify.* Whether partitioned scheduling with no load balancing is
usable, or whether the absence of automatic placement is intolerable in
practice. The design chose it for analysability, which is a real-time
argument; if real time is one subsystem rather than the thesis, the
argument is weaker than it was when it was made.

*Watch for.* The lesson M7 ended on, which is that a check written
against a state the system reached later than the check looked passes
while testing something else. On one core that needed a program spawned
at the checker's own priority. On two, anything the checker does not
explicitly wait for is a race, and the ones that pass are the ones that
will pass again on a faster host and fail on a slower one.

*Built.* The storage stack runs split across the two cores and a read
still works. The block store is moved to the second core while it is
blocked at its receive point, which is the only reason one core is
allowed to move it: a thread on no queue belongs to nobody, and a thread
running on another core cannot be moved by this one at all.

**The cost is four pokes and two donations, for a read that cost neither
on one core.** The same client reads the same file twice, once with the
whole stack on the first core and once with the store on the second, and
the difference is the whole of what partitioning charged. Both numbers
are measured rather than reasoned about: zero on one core is not an
assumption, it is what the counters said.

Four is a small number and it is per read. It is also the number for one
boundary crossing one core: the chain is a client, a filesystem and a
store, and only the last hop crosses. A stack with more hops placed more
finely pays it more times, and this measurement is the unit rather than
the total.

**The same read send-blocks now, where it did not before.** On one core
the client always found the filesystem already waiting, and the check
asserted exactly that. With the store elsewhere the client blocks in
send, because the two cores make progress independently and whether the
server has got back to its receive point is a question about which core
got there first.

It blocked on every run of it, and the check still does not assert that
it will. Asserting either answer is asserting a race, which is the thing
this milestone's own note said to watch for: a check that passes on this
host because of an ordering nothing enforces is a check that fails on a
slower one. So the check reports which happened and asserts only what is
guaranteed, which is that the read returned the right bytes.

**What this does not answer.** The milestone asked whether partitioned
scheduling with no balancer is usable, and whether the absence of
automatic placement is intolerable in practice. This evidence answers
the first and cannot answer the second.

It is usable: the mechanism works, the cost is small, bounded, and
countable, and nothing above the placement noticed. But there was
nothing here for a balancer to have done better. The stack has one
obvious placement, it was placed that way by hand in one call, and it
stayed there. Answering the second question needs a workload whose right
placement changes while it runs, and this system has no way to produce
one: every program in it is started by the checker and does a fixed
amount of work.

So section 11.4 is neither deleted nor endorsed by this stage. What is
established is narrower and worth having: the mechanism it specifies
exists, costs four inter-processor interrupts per boundary crossed, and
a hand-placed stack across two cores behaves exactly as the same stack
on one. The argument that partitioning buys analysability is untouched,
because nothing here measured latency; that is M9's question and it is
the one that will decide whether the trade was worth making.

*Reviewed.* Three findings, one of which is about how the checking was
done rather than what it checked.

**The guard that proved the crossing happened could pass without one.**
It asserted that the pinned read cost more than zero pokes, and a poke
is sent whenever one core has anything to tell another, including a
timer being rearmed. A machine where the read never left the first core
could still have a nonzero count and pass. What proves the chain crossed
is the donation: a priority raised on a thread this core does not own
happens for one reason. The check asserts that now, and disabling the
migration fails with `the store moved and no donation crossed` rather
than passing quietly.

That is the fourth time this milestone that a check has been found
asserting something adjacent to what it claimed.

**A warning existed in two of the four configurations and was not seen,
because warnings were only ever read from one.** The variable holding
the block store's process is used only at Tier 2, and at Tier 0 it is
set and never read, which the compiler says plainly to anyone who
builds that configuration and looks. Every stage of this milestone has
run the check in all four and read the compiler output from one. The
build is clean in all four now, and the habit was the defect rather
than the variable.

**And the strongest thing this stage did was not being claimed.** The
store is moved to the second core partway through the boot checks and
nothing moves it back, because nothing in this system moves on its own.
Everything after that point runs with the storage stack split across
two cores, including the shell reading files through it, and that is the
part of the run the check compares exactly. The whole run costs 35 pokes
and 16 donations against the 4 and 2 the single measured read cost, and
those numbers are what say the rest of the session kept crossing rather
than quietly ending up back on one core.

So the compared transcript is not merely identical with two cores
running. It is identical with the storage stack split across them.

*Size.* Large. The original estimate of medium to large predated knowing
that interrupt disabling was carrying the exclusion for the whole kernel.

### M9. Real Time

**Deadlines, the tickless timer, and the first measurements.**

**Done.** All six stages are built. Section 12 says what it answered,
what the figures are, what the building kept finding, and what it left
untested.

The priority mechanisms are present from M1. What is missing is treating
the result as a real-time system, which means measuring it. This is where
"no figures are claimed" stops being true.

*Needs.* Absolute deadlines everywhere a timeout appears (14.3, 14.4). A
one-shot timer reprogrammed to the earliest pending event, with no tick
(14.1). A bound on the donation chain. Introspection sufficient to read
pool high-water marks (22).

**Most of that list is already built, and saying which changes what this
milestone is.** Absolute deadlines arrived in M1 stage seven and every
wait has carried one since. The tickless one-shot arrived with them, and
M8 gave the arming of it to one named core. Pool high-water marks have
been in the compared report since M3. What is left of the list is one
item: the bound on the donation chain, which `donate()` says in its own
comment is unenforced, because donation happens at a rendezvous where
the target has just stopped being blocked and a walk is only needed to
raise a thread that is already blocked onward. A cycle of servers would
deadlock undetected. That is a real gap between section 12.3 and this
tree, and it is the only new mechanism this milestone owes.

So M9 is not mostly mechanism. It is measurement, and the hard part of
measurement here is not taking the numbers.

**The hard part is that this system has never run on hardware.** A
latency figure taken under emulation is a figure about the emulator and
the host it is on. M8 proved that rather than assuming it: a check that
asserted an interrupt arrived inside one particular run failed about one
run in three with the machine busy and never with it quiet. Publishing a
nanosecond figure from this tree and calling it interrupt latency would
be the same error with a decimal point on it.

The way out is not to avoid measuring. It is to separate two kinds of
number and treat them differently.

**What is a property of the kernel is countable and deterministic.** How
many instructions the longest interrupts-disabled region is. How many
preemption points a path passes. How many switches, rendezvous and
whole-TLB flushes a round trip costs. These are the same on every host,
belong in the compared file, and are what section 13.3's first term
actually asks for: it defines worst-case latency as the longest
interrupts-disabled region plus the dispatch, and says both terms are
measurable on a given port.

**What is a property of the host is a duration**, and it goes in the
report above the compared line, labelled with the machine that produced
it, and is never asserted against a threshold. M6 stage four settled
this rule for switch counts and it applies with more force here.

**And a rate is a property of the host too, which is what M10 found out
afterwards.** The interrupt check armed a repeating timer at a
millisecond and ran a program that computes for eighty, so that
something would certainly interrupt it. A period is a rate against the
host clock, and the guest has to service one activation before the next
comes due. When it cannot, it never gets back to the program between
them: the timer was caught firing 1026 times a second while the program
made no progress at all, the machine at full CPU and the check printing
nothing, for as long as anyone was willing to wait. It looked exactly
like a hung kernel and was not one.

A count of activations is safe to ask for and a duration is safe to
report. A rate is neither, because it is the one figure the host can
make impossible to meet. The check arms a single activation now.

**And M8 left a deviation this milestone has to confront.** At Tier 2
the kernel takes one lock at every entry and holds it with interrupts
disabled, which was recorded as a stated deviation from section 13.3
rather than hidden. Section 13.3's whole per-tier argument is that Tier 0
disables interrupts and accepts a latency floor, while Tier 2 enables
them and locks finely to remove that floor. As built, Tier 2's longest
interrupts-disabled region is its longest kernel entry, which is not
better than Tier 0's, it is worse. The argument in 13.3 is currently
inverted by the implementation, and this is the milestone that has to
measure that and either fix it or say so in the section.

*What M8 leaves for this milestone.* Three habits, each of which cost a
stage there.

**A counter is per core until there is a reason it is not.** M8 found one
of something for the machine six times in six stages. Every number here
is taken on a core, so every one of them decomposes, and a longest-region
figure summed across cores would be meaningless rather than merely wrong.

**A check must assert the thing it names.** M8 found four that did not,
all of them passing. A latency check is unusually easy to get wrong this
way, because a number arriving at all looks like the number being right.

**A check that depends on how fast the host is will fail on a slower
one.** This is the same lesson as the first paragraph about emulation and
it is worth stating twice, because the entire subject of this milestone
is durations.

*Stages.* Six. The first three build the measurement and take the two
terms of section 13.3, the fourth turns them on the paths the design
cares about, the fifth is the one piece of mechanism still owed, and the
sixth asks what the numbers mean for the section that motivated all of
it.

**1. A measurement that is not a measurement of the host.** *Built.*

- A cheap monotonic read on both ports, and the honest statement of what
  it counts. Its resolution is reported rather than assumed.
- Alongside it, the counting instruments: a way to say how long a region
  is in a unit that does not move when the host does.
- Per core, because a duration is measured on a core.

*Check.* The counter never goes backwards across two reads on one core,
its resolution is printed with the configuration that produced it, and
the counted unit of a deliberately doubled region doubles while the same
region's duration on a loaded host does not have to. Anything reported
in nanoseconds is above the compared line; anything compared is a count.

*Built.* There is a clock, a statement of what it is a clock of, and a
counted unit beside it. The two are reported in different places on
purpose, and that separation is the whole of what this stage settles.

**The unit that does not move when the host does was supposed to be
retired instructions, and neither port can supply one.** The RISC-V
counter is architecturally an instruction count and can be read from
supervisor mode here, because the boot code already permits it. Asked
for the length of a thousand-iteration loop it answered 130000, and
asked for a loop of twice the length it answered about the same again.
The emulator is answering an instruction counter with something
time-shaped. That is recorded rather than worked around, because it is
the reason every duration in this milestone comes from a clock nobody
should trust and every compared figure is an integer instead.

So the counted unit is the kernel's own arithmetic: how many times a
region was entered. A region entered twice as often counts twice as
often, on every host and in all four configurations, and one of those
counts is in the compared file to prove it. Changing the work changes
the compared line, which is what makes it a check rather than a
decoration.

**The clock is one clock and both cores read it.** Both ports take it
from a counter the machine shares rather than from anything per core,
and the check is an ordering rather than a duration: the first core
stamps, the second stamps, the first stamps again, and the middle
reading has to lie between the outer two. Giving the second core a
counter with its own origin fails with the reading sitting outside the
interval by a wide margin. It cannot flake, because it asserts an order
and never a threshold.

The first attempt at that check proved nothing, and how it failed is
worth keeping. Breaking the clock by giving every core its own counter
starting from zero left both cores reading small numbers that still
came out in the right order, so the check passed. Two counters with the
same origin and rate are one clock for this purpose; what actually goes
wrong is two counters with different origins, which is what a per-core
cycle counter would give. The break test was rewritten to be the failure
that can happen rather than the one that was easy to write.

**The resolutions differ between the ports and both are printed.** Ten
million ticks a second on one and 62500000 on the other, 100 nanoseconds
and 16. A figure quoted without that number beside it would be a figure
nobody could compare with anything.

And the durations are already visibly useless, which is the most useful
thing they do here. The region entered 1024 times reported a longest
visit of 264 ticks and the region entered 2048 times reported 117. The
smaller job took longer, because what was being measured was the host.
The counts beside them are 1024 and 2048 in every run.

*Reviewed.* Two findings, and the first is the one this milestone was
warned about before it started.

**The check for a per-core counter could not tell one from a shared
one.** The stage's claim is that a measurement is kept per core because
a region runs on a core. Its check entered two regions a known number of
times and compared the counts, and every one of those entries was made
by the same core. Forcing every increment into a single slot for the
machine leaves that check reading 1024 and 2048 and passing.

That is the failure M8 found four times and wrote into this milestone's
entry as the thing to watch for: a check asserting something adjacent to
what it claims. It took one experiment to find, and the experiment was
the obvious one, which is the part worth remembering. The claim was
about two cores and nothing in the check involved two cores.

There is a third region now that both cores enter at once, four thousand
and ninety-six times each, and the check asserts each core's own share
rather than only the total. With the counter forced into one slot it
fails with `shares of 8190 and 0, not 4096 each`. The 8190 is the second
half of the point: two increments were lost outright to the race, so the
total was wrong as well as the split, and the total alone would have
been a weaker signal than the split.

**And the file that says never to assert a threshold on a duration was
asserting one.** The clock check read the counter ten thousand times and
insisted it had moved, which is a statement about how fast this host
gets through ten thousand reads. The margin was enormous and it would
never have fired, and that is not the point: the rule this milestone is
built on was broken in the file that states it. It reads until the value
changes now, with room enough that only a stopped clock reaches the end,
and what it asserts is that the counter runs and never goes backwards.
Both are properties of the counter.

**What was checked and left.** The compared count is deterministic partly
because these regions are entered by this check and by nothing else. A
region the rest of the kernel entered would put a number in the compared
file that moves with what the system happened to do, which is the trap
M6 stage four fell into. That is a property of how this facility is used
rather than of the facility, and the next stage measures a region the
kernel really does enter, so it is the next stage's problem to solve
rather than a defect here.

**2. The longest interrupts-disabled region, per tier.** *Built.*

- Section 13.3's first term, instrumented where interrupts are disabled
  and restored, recording the longest region and where it began.
- Naming the site matters more than the number. A figure with no site
  attached cannot be acted on.

*Check.* A deliberately long region introduced anywhere becomes the
reported maximum and is named as its own site. Removing it restores the
previous maximum. The figure is reported for both tiers.

*Built.* The region is measured at the transition rather than at every
call, because section 13.3's term is a contiguous region and this kernel
nests these freely. A call that finds interrupts already off is inside a
region somebody else began; the one that turns them off starts the
clock, and the restore that turns them back on stops it. The site is the
address of whoever asked, resolved against the link map.

**The first figure it produced was 480 milliseconds, and it was wrong in
an instructive way.** A timer fires every millisecond in this check, so
a region that long is impossible. The region was the idle wait. M8 moved
that wait behind masked interrupts to close a lost wakeup, so the core
really does sit there with interrupts off, and the instrument was
counting it. But a core asleep on an empty queue is not withholding an
interrupt from anybody: the instruction wakes on a masked interrupt and
the next two lines take it. Counting the sleep makes the longest region
the length of the quietest moment in the run. The measurement stops at
the wait and starts again on the other side.

**The second figure was 248 milliseconds, and it was wrong differently.**
Returning to user mode turns interrupts back on without going through
the restore, so a region opened in the kernel stayed open across
everything the program then did. The figure included time when
interrupts were on. Every path that re-enables without a restore closes
the region now: the explicit enable, the return from a system call or a
trap, and the drop into a new program.

What is left is a figure that can be true. On this host, at Tier 2, the
longest region is about 3800 microseconds' worth of ticks and it begins
in `sched_block`; at Tier 0 it is the same site. That is the useful part
of the result and it is what the site was for. Blocking disables
interrupts and then dispatches, so the region runs from the moment a
thread decides to block until whatever runs next re-enables. It spans a
context switch, and it is genuinely a span in which that core cannot
take an interrupt.

**The count is not in the compared file and must not be.** How many
regions a run has depends on how many interrupts arrived, which is a
property of the host. The figure and the count are both above the line;
what stage one put below it stays the only compared thing here.

A deliberate region introduced before the report becomes the reported
maximum and is named as its own site, and removing it puts the site back
to `sched_block`. Worth noting that the deliberate one, half a million
iterations of a busy loop, came out about the same size as the largest
region the kernel produces on its own. That is a statement about how
long blocking holds interrupts off, and it is the first number this
project has that says anything about latency at all.

*Reviewed.* Two findings and one corroboration.

**The measurement put a read of this core's own state inside the
function every critical section in this kernel calls, and that register
was never initialised.** `cpu_self()` finds the per-core structure
through `tp` on one port and `TPIDR_EL1` on the other, and neither was
cleared at boot: before `cpu_init` claims a core, both hold whatever
reset left. That was harmless for as long as nothing read them early,
which M8 stage one noticed and left alone. Putting the read inside
`md_intr_disable` moved it from harmless to one early call away from a
wild write, and the early calls in a boot path are exactly the ones
somebody adds later without thinking about it.

Both boot paths clear the register now, and the measurement declines
when it finds nothing there. The read before `cpu_init` returns zero,
which was checked rather than assumed. One residue is stated rather than
fixed: the RISC-V secondary carries its hart number in that register for
the few instructions between the handover and claiming its own state,
because supervisor mode cannot read the hart number any other way.
Nothing in that window disables interrupts.

**And the idle path opened a region it immediately closed.** Waking from
the wait began a region attributed to the idle loop, and the very next
line enabled interrupts, which ends one. It recorded a region of almost
no length per wakeup and added it to the count. Removed: the wait closes
the region before it and the disable after it opens the next, with
nothing in between to attribute.

**The corroboration is worth as much as either.** Both ports name the
same site. The other port had not been looked at when the stage was
written, which was an omission: its clock runs at 62500000 ticks a
second against 10000000, its longest region is a different number, and
it resolves to the same line of `sched_block`. Two ports, two clocks,
two figures, one answer about where the time goes. That is the first
result in this project that two independent measurements agree on.

*Also.* The break test that produced the stage's figures was reverted
with a scratch copy rather than with the version control, and one of
those reverts never ran because the command it was in hit a timeout.
Three subsequent bisections were run against the leftover before a
`git diff` showed it. The lesson is not about the code: it is that
"is the tree what I think it is" should be one command and never an
assumption, and this milestone's whole subject is measurements taken
against a state nobody verified.

**3. Dispatching an interrupt to a user thread.** *Built.*

- Section 13.3's second term: from the interrupt being taken to the user
  thread that waits on it running.
- The path already exists and is checked for correctness; what is missing
  is its length.

*Check.* The measured dispatch counts the preemption points the path
actually passes, and inserting one more into that path raises the count
by exactly one. A program that waits on an interrupt reports the same
number on both ports.

*Built.* Sixteen interrupts reach a program in the paced run, the longest
taking 1601 ticks on one port and 10400 on the other, and every one of
them passes no preemption point at all.

**The preemption points did not exist.** Section 13.3 bounds Tier 0
latency by where they are, section 17.6 says they sit at batch entry
boundaries, and the tree had none: two comments referred to them and no
code marked one. So the argument that tier rests on was resting on a
placement that existed only in the sections. They are placed now where
those sections say, at the batch entry boundary in the operation entry
path, at the message copy boundary, and at each page table entry a walk
writes. They count rather than yield, because what this stage needs is
the number and a point that yields is a scheduling change wearing a
measurement's clothes.

**The dispatch passes none of them, and that is the result.** The path
from an interrupt being taken to the program waiting on it running goes
through a handler, a timeline, a scheduling decision and a context
switch, and none of those is a boundary anybody named. So the term is a
straight line: it cannot be broken into, and its length is its whole
bound. Adding one point to that path makes the reported figure exactly
one, which is the check.

**The counts agree across all four configurations and the durations do
not.** Sixteen dispatches and zero points in every one; 1601 ticks
against 10400 for the same work on two clocks running at 10000000 and
62500000 a second. That is the milestone's rule producing exactly what
it predicted: the number that is about this kernel is identical
everywhere, and the number that is about the machine is not.

**And the term is unmeasurable in one of the two input regimes, which is
a fact about the input.** The paced session dispatches sixteen because
the console driver waits for a person between lines. The burst session
dispatches none, because the whole input is already there when the
driver looks and it never blocks. The first version of the check
panicked when the count was zero, which failed the burst run and would
have been asserting how the input arrived rather than anything about the
kernel. It says so instead, and what it asserts is the part that is
about the kernel: a dispatch that happened passed no point.

*Reviewed.* One defect, one piece of dead code, and one thing that
turned out to be right for a reason worth writing down.

**Whether this core is inside an interrupt handler was kept on the core,
and it belongs to what is running.** The handler sets the flag, runs,
and clears it, and in between it can hand the core away: releasing a
waiter can dispatch, and the thread that runs next would have every
thread it makes runnable recorded as woken by an interrupt. The flag is
put down at a context switch and picked up on the way back now, exactly
as the kernel lock has been since M8.

This is the milestone's recurring shape one more time. Not a counter
this time but a flag, and the same question answered the same way: it
described the thing running, and it was stored on the thing running it.

The hazard is real and it is rare. A count of how often the handler
hands the core away mid-handler reports one in some runs and none in
others, so the mis-attribution was one thread's timing away from
happening and had not happened yet. That count is printed even when it
is zero, because a line that appears only when something went wrong
cannot say that nothing did.

**The dead code was an accessor for the points figure**, declared,
defined and never called. It went in because the reporting function was
written before it was known what the report would need.

**And the point at the page table walk was placed correctly by
accident.** It sits before the test that asks whether this walk is being
charged to a buffer move, so it counts every entry a walk writes rather
than only the entries the buffer accounting cares about. That is what
section 13.3 asks for and what the stage claimed, but the placement was
one line away from being the narrower thing, and the two are not
distinguishable by any check here: the dispatch path passes no page
table walk at all, so both placements report zero for the term being
measured.

**4. IPC round trip and donation latency.** *Built.*

- The two figures the design's own arguments rest on, taken on the paths
  that already exist rather than on a benchmark written to be fast.
- Both tiers, and at Tier 2 both with the server on this core and on the
  other, because M8 measured that boundary at four pokes and this is the
  same crossing seen from the other side.

*Check.* The round trip is reported as a count that is identical in all
four configurations and as a duration that is not compared. The donation
figure is taken with the chain crossing a core boundary and without, and
the difference is stated.


*Built.* Both figures are taken on the paths the system already has: the
round trip is what a client spends blocked waiting for its reply, and
the donation is from a client raising a server's priority at the
rendezvous to that server running.

**The numbers, with the configuration attached.**

| | Tier 0 riscv64 | Tier 0 aarch64 | Tier 2 riscv64 | Tier 2 aarch64 |
|---|---|---|---|---|
| round trip, best | 39 us | 39 us | 50 us | 35 us |
| donation on one core, best | 15 us | 12 us | 24 us | 16 us |
| donation across cores, best | none | none | 57 us | 56 us |

These move by a few microseconds between runs, because they are
durations and the host is not the same twice. What does not move is
their order and their ratios.

**Crossing a core costs about two and a half times a donation that does
not, and both ports say so independently.** Sixty-two microseconds
against twenty-five on one port, fifty-eight against twenty-two on the
other. M8 priced that boundary at four inter-processor interrupts per
read; this is the same crossing in time, seen from the other side, and
the two ports agreeing to within seven per cent is what makes it worth
believing.

**The ticks had to become microseconds before any of that was
visible.** The two clocks run at 10000000 and 62500000 a second, so the
same work gives 621 ticks on one port and 3684 on the other, and a
reader comparing those two numbers learns nothing. Converted, they are
62 and 58. The tick counts are what was measured and the microseconds
are what can be compared, and printing only the first would have hidden
the strongest result in the stage.

**The stage expected a count identical in all four configurations and
there is not one.** At Tier 0 both ports agree exactly: 145 round trips,
46 donations, and the same range of preemption points. At Tier 2 one
port sees 158 round trips and the other 159. Two cores make the number
of round trips a run performs depend on how the two of them interleave,
so it is not a property of this kernel and it does not go in the
compared file. The expectation was written before there were two cores
in the check and it was wrong.

**And the first version of the points figure printed a negative
number.** A donation that crosses a core is stamped on one core and
arrives on another, and the count of preemption points is per core:
subtracting one core's from another's gave 18446744073709501435, which
is what a small negative number looks like when it is unsigned. That is
this milestone's recurring lesson arriving in a third form. The points
are counted only when both ends were on one core, and the report says
how many crossings could not be counted rather than printing a figure
for them.

**The worst round trip is a person typing.** Twenty million ticks is
the two seconds the paced session leaves between lines, which a client
waits through when the server on the other end is the console driver.
That is a number about the session, so the figure that matters for the
round trip is the best case: the one where nothing was waiting for
anything, which is the kernel's own cost.

The donation figures were the same shape and for a different reason,
which the review of this stage found and which is recorded there.

*Reviewed.* One defect in what was being measured, one in how, and a
published number that was wrong by four orders of magnitude.

**A third of the donations were not donations.** `donate()` raises a
thread to at least a priority, and it is called twice: once by a client
raising the server it is about to rendezvous with, and once by a server
raising itself when it picks up a sender that was already queued. The
second is not a donation anybody waits for. The server is already
running, so the measurement, which closes when the donated-to thread
next comes out of a block, closed at the next message that thread
received. In the paced session that is two seconds later.

Excluding it changes the count from 46 to 33 at Tier 0 and from 43 to
30 at Tier 2, and changes the worst case from 20145394 ticks to 2830.
Two seconds becomes 283 microseconds. The best case barely moves,
because a self-donation is never the fastest thing in the set, which is
why the figure that was published survived and the explanation beside it
did not.

**And the explanation was the part that was wrong.** The stage said the
worst case in every one of these was a person typing. That is true of
the round trip, where a client really does wait for a server that waits
for input, and it was false of the donation, where the two seconds came
from measuring an interval that had no business being measured. A
plausible explanation for a number is not a check on it, and this one
was plausible enough to stop the question being asked.

**One visit, two readings of the clock.** The duration used for the
shortest and the duration used for the longest came from separate reads
taken a few instructions apart, so a single visit was measured to two
different ends. Nothing observable followed from it, and it is the kind
of thing that makes two figures in the same line quietly incomparable.
One read now.

**And a parameter nothing used.** The round trip's closing call took the
thread it was closing for and ignored it.

**5. The bound on the donation chain.** *Built.*

- The one mechanism the *Needs* list still owes. `donate()` explains why
  the current arrangement cannot enforce it: nothing links a blocked
  sender to whoever is serving it.
- Either that link is built and `SYS_ERR_DEADLOCK` becomes reachable, or
  section 12.3 concedes that per-send donation is the whole mechanism.
  The stage decides which by trying the first.

*Check.* A cycle of servers is refused with `SYS_ERR_DEADLOCK` rather
than deadlocking undetected, and a legitimate chain of the maximum
length is not refused. If the link proves to cost more than the bound is
worth, the check is that section 12.3 says so and the code stops
claiming otherwise.

*Built.* The link exists, the bound is enforced, and `SYS_ERR_DEADLOCK`
is reachable for the first time. The stage said it would decide between
building the walk and conceding the section by trying the first, and
trying it produced a more interesting answer than either.

**The link is a list on the channel of threads that owe it a reply.**
That is what section 12.3 said was missing: from a queued sender there
was no way to reach whoever was serving the channel it was queued on.
It is set when a rendezvous completes, cleared at the reply, and cleared
again when a server dies or is cancelled mid-request, because a dead
thread left on that list is one the walk would follow into a slot that
now belongs to something else.

**A cycle is refused by the bound rather than recognised.** A server
that takes a request and then sends into the channel it serves is a
cycle of one, and the walk follows it round until the count runs out.
That is deliberate: to a walk that counts, a cycle and a chain that is
too long are the same thing, and the section already specifies the same
error for both. One server and one channel is the whole test, which is
two threads cheaper than two servers pointing at each other and proves
exactly as much.

Removing the walk makes that send block forever and the machine wedges
with nothing printed, which is what section 12.3 said would happen and
had never been demonstrated.

**And the inheritance half of it raises nobody.** Every run walks chains
to a depth of seven out of eight and raises zero threads. No client in
this system ever queues on a server that is itself blocked onward at a
lower priority, so the case the walk was built for does not occur here.
The mechanism exists for the workload that has it; on the evidence
available, the section's original claim that per-send donation is the
whole of what is needed has not been contradicted.

That is worth stating plainly rather than counting the stage a success
for the mechanism it added. What the walk demonstrably buys is the
deadlock error. What it was also supposed to buy cannot be shown to be
worth anything by any workload this tree contains, and a counter saying
zero is a better answer than a mechanism nobody measured.

Section 12.3 is rewritten to describe what is there: the walk, what it
is for, what enforces the bound, and the fact that it has raised nothing
so far.

*Reviewed.* The stage reported that the walk raises nobody and treated
that as the answer. It was the answer to a question nobody had asked
properly: the raising half of the mechanism had never been executed, so
"it raises nobody" and "it cannot raise anybody" were indistinguishable,
and the stage had no way to tell which it had built.

**So the review built the workload the walk exists for.** Two servers,
one behind the other, serving a client at priority 6, with the deeper
one blocked without replying so the chain stays where it is. Then a
client at 20 sends to the first server, which cannot take it, and queues.

The walk raises both of them to 20. `chains: 2 threads raised by a walk`
where every previous run said zero. The raising half works, and it is
now exercised in every run rather than argued about.

Removing only the raise, and leaving the bound, fails with `the chain
stayed at 6 and 6`. The two halves of the mechanism are independently
falsifiable now, which they were not: removing the walk wholesale hangs
at the cycle check before the inversion is ever reached, so the earlier
break test could only ever have demonstrated one of them.

**What this changes about the stage's conclusion.** It said the case the
walk was built for does not occur in this tree and that section 12.3's
original claim had not been contradicted. The first half stands: nothing
in the system's own work produces the inversion, and the count was zero
for every run before this check existed. The second half was too
generous to the section. The claim was that there is no thread whose
promotion would help, and there are two, and they are now raised. What
the section can still say is that the workloads this system runs do not
need it, which is a much narrower statement than the one it made.

The zero was a real measurement and reporting it was right. Stopping
there was not: a mechanism that has never run is not a mechanism that
has been shown to be unnecessary, and the difference is one check.

**6. What the figures say about section 13.3.** *Built.*

- The milestone's own question, and the only stage whose outcome could
  rewrite a section rather than delete one.
- The per-tier argument is that Tier 2 removes a floor that Tier 0
  accepts. As built it does the opposite.

*Check.* The longest interrupts-disabled region is reported for both
tiers with the site that produced it. If Tier 2's is longer than Tier
0's, section 13.3 is wrong about this implementation and the section
changes or the implementation does, and the stage says which and why.

*Built.* Both terms of section 13.3 are measured on both ports and both
tiers, and the section is rewritten around what they say. The stage
expected to find Tier 2 worse than Tier 0 and the section inverted by
the implementation. It found something else.

| | Tier 0 riscv64 | Tier 0 aarch64 | Tier 2 riscv64 | Tier 2 aarch64 |
|---|---|---|---|---|
| longest region | 596 to 1009 us | about 500 us | 571 to 733 us | about 800 us |
| mean region | 16 us | 16 us | 5 us | 7 us |
| regions entered | 580 | 580 | 20500 | 20400 |
| interrupt to a program | 126 us | 87 us | 151 us | 222 us |
| points on that path | 0 | 0 | 0 | 0 |

Those are the figures after the review of this stage corrected them.
What the stage first published was four times larger and named the
wrong site; see below.

**The tiers cannot be told apart in the worst case and are sharply
different in the typical one.** M8 recorded a deviation, that Tier 2
takes one lock at every entry and holds it with interrupts disabled, and
predicted this milestone would find the latency floor raised by it. Four
runs of each on one port give 596, 1009, 619 and 621 microseconds at
Tier 0 against 733, 601, 580 and 571 at Tier 2, which is two overlapping
ranges and no difference at all. The mean is a different matter: 159
ticks against 51, stable to a few per cent, so a typical region at Tier
2 is a third the length. The lock makes twenty thousand short regions
where the other tier has six hundred longer ones.

**What the deviation costs is the number of regions, not their length.**
585 in a run at Tier 0 against about 22400 at Tier 2, which is the big
lock making every entry into the kernel a disabled region, and the
longest of them unaffected by any of it.

**The longest region spans a context switch, and no preemption point can
bound it.** Four measurements, two tiers, two ports, two clocks, agree
on both ends: it begins in `channel_reply` and ends in `channel_send`. A
server replying disables interrupts, hands the reply over and dispatches
to its client, and the region does not close until that client is back
in the kernel sending again. That is the strongest agreement anything in
this project has produced.

So section 13.3's argument is aimed at the wrong region. It says Tier 0
bounds latency by preemption point placement; the binding region
contains no preemption point and cannot, because it is the switch
itself. Stage three placed points at batch entries, message copies and
page table walks, and every one of them lowers regions that were never
the longest. Enabling interrupts inside the kernel and locking finely,
which is what the section promises Tier 2, would leave the binding term
exactly where it is.

**What would lower it** is not disabling interrupts across a switch, or
making the switch interruptible. Both are design changes rather than
measurements, and neither is attempted here. Naming them is what this
stage can do, and it could not have named them before the figures
existed: the answer was that the floor is scheduling and everyone had
been looking at critical sections.

**And the section keeps its per-tier shape rather than losing it.** The
tension it describes between analysability and absolute latency is real
and the resolution it chooses is still defensible. What was wrong was
the account of what sets the floor, which is now measured and written
down, along with the caution that these figures come from an emulator
and are the right shape and the wrong magnitude.

*Reviewed.* The stage published a figure four times too large, drawn
from one sample, and named the wrong site. All three faults were the
same fault, and it is the one this milestone has now made four times.

**The longest region was the boot.** Nothing was measured about when it
happened, so nothing said that it happened 0.6 per cent of the way into
the run and ended at the single `md_intr_enable` that turns interrupts
on for the first time. Before that call the machine is running with them
off because it has not started yet, and the interval across it is not a
path this kernel ever takes again. Measurement begins at that call now.

The real longest region is 473 to 628 microseconds rather than 1741 to
3180, and it begins in `channel_reply` and ends in `channel_send`, not
in `sched_block`. So a server replying to its client, which dispatches
into that client, which comes back into the kernel to send again. The
stage's conclusion survives in shape and not in detail: the binding
region does span a context switch and does contain no preemption point,
which is what the argument rested on, and everything specific about it
was wrong.

**And the site named where the region opened, which the stage read as
where the time went.** Those are different questions whenever a region
spans a switch, because the second thread's work is inside the first
thread's region. Recording both ends is what found the boot artefact:
one address said `sched_block` and looked plausible, and the pair said
`sched_block` to `md_intr_enable`, which is not a path, it is a boot.

**One sample is not a distribution.** The stage reported a maximum and
concluded from it. The mean is 5 to 16 microseconds against a maximum
of 473 to 628, so the figure the section is built on is one event in
twenty thousand. That does not make it the wrong figure, because a
real-time bound is made of worst cases, but a bound whose relationship
to the typical case is unknown is a bound nobody can act on. Both are
reported now.

**What changes in the conclusion.** The stage said M8's prediction was
wrong, the review of the stage said it was right by about a fifth, and
four runs of each configuration say neither: the worst case is the same
within a spread that is larger than the effect being looked for. Reading
a difference out of one run of each was the same error twice in a row,
in opposite directions. What the stage got right and keeps is the shape:
the region that binds spans a switch, and no placement of preemption
points touches it.

**And the pattern.** The idle wait, the return to user mode, a server
donating to itself, and now the boot. Four times in one milestone a
measurement has included an interval that was not what was being
measured, and each time the number was plausible enough to publish. The
instrument that found all four in the end was the same one: ask where
the interval started and where it ended, and check that both are
somewhere the question makes sense.

*Done when* interrupt latency, IPC round trip and donation latency are
measured on both tiers and published together with the configuration that
produced them, the Nth activation of a periodic timer lands at `first + N
* period` despite a deliberately stalled task, and every published figure
says whether it is a property of this kernel or of the machine it was
taken on.

*Would falsify.* Every latency intuition in the design, and the per-tier
interrupt latency argument in 13.3 specifically, which the M8 deviation
has already put in doubt. Also whether a tickless timer is affordable on
a part where reprogramming is a slow peripheral write, which cannot be
answered here and needs saying rather than guessing.

*Watch for.* The thing M8 spent a stage learning: a number that is
usually right is worse than no number, because it makes a check fail
sometimes and a check that fails sometimes gets deleted. Every figure in
the compared file must be a count. Every duration must be somewhere it
can move without breaking anything.

*Size.* Medium, and the estimate is unchanged rather than confirmed.
Mostly instrumentation and discipline, plus one piece of mechanism and
one section that may have to be rewritten.

### M10. The Submission Engine

**Section 17: the count exceeds one.**

**Done.** All six stages are built. Section 13 says what it answered,
what it could not, what the building kept finding, and what it left
untested. It was the last milestone in this list, and M11 below is the
experiment it named as the one that would settle its open question.

Deliberately last. The engine needs issuing ops to be interesting, issuing
ops need a DMA-capable device, and that wants the driver work in M6
underneath it. Building it earlier against blocking ops alone would
exercise the machinery without testing the idea.

**That premise did not hold, and this entry is written knowing it.** M6
built a block store, and it is a user process holding bytes behind an
IPC protocol, not a device that transfers on its own. Nothing in this
tree does direct memory access, `SYS_OP_DMA_ISSUE` and its two
neighbours are still opcodes that answer `SYS_ERR_NOT_SUPPORTED`, and
no stage of any milestone has written a device driver that a transfer
could be in flight on. So the situation the paragraph above warns
against is exactly the situation this milestone is in.

*Needs.* Entry arrays with per-entry status. Typed registers with tags.
`LINK` chaining with abort semantics. Issuing ops returning timelines.
Prepared programs validated once and executed many times. Preemption
points at entry boundaries.

**One of those is already built.** M9 stage three put a preemption point
at the batch entry boundary, where section 17.6 says one belongs, and it
fires once per call because the count is one. Raising the count is what
makes it mean anything, and it makes section 13.3's claim about
`SYS_BATCH_MAX_ENTRIES` checkable for the first time: the section says
the constant does not appear in interrupt latency at all and that the
cost of one entry does. Nobody has ever been able to test that.

*What splits in two, and only one half can be tested here.* The engine
makes two claims and they are independent.

**That a dependency-ordered set of operations costs less than the same
operations submitted one at a time.** This is about crossings of the
trap edge and about work the kernel does twice, and it needs no device
at all. Blocking operations answer it completely.

**That the work runs with three transfers in flight, in the submitting
thread's context, at its priority, with no worker pool.** This needs a
device that can have a transfer in flight. There is none, so this half
is untestable in this tree and the milestone must say so rather than
building something that looks like a test of it.

*What M6 already measured, one level up.* Section 25.7 asked the same
question of the filesystem protocol: whether one compound message beats
several round trips. The answer is in the compared report and has been
since M6.

```
fs_client   5 rendezvous, 1 connections, 12 entries, 6 invalidations
fs_batch    3 rendezvous, 0 connections, 4 entries, 2 invalidations
```

Batching paid there, and the second column is why: a compound never
draws from the connection pool. The submission engine asks whether the
same shape pays one level down, at the trap edge rather than at the
message. M6's numbers are the precedent for how to answer it and the
warning that the interesting difference may not be the one being looked
for.

*What M9 leaves for this milestone.* Two rules and a warning.

**A count is a property of this kernel and a duration is a property of
the host.** The question of whether the engine pays must be answered in
counts: crossings, entries, rendezvous, switches. A figure in
microseconds comparing two ways of doing the same work would be a figure
about the machine it ran on, and this milestone would publish it as if
it were about the design.

**A comparison is where a measurement most easily includes something
else.** M9 found five intervals that were measuring something other than
what they claimed, and every one of them looked reasonable. Comparing
two forms of the same work is the shape most likely to differ in
something nobody controlled, so both forms have to do the same work and
the check has to say what makes them the same.

**And the thing to watch for is a batch that wins because it was
allowed to skip something.** The one-at-a-time form pays for validating
each entry, entering the kernel each time and taking the lock each time.
If the batch form skips a check rather than amortising it, it will look
faster and be wrong.

*Stages.* Six. The first three build the mechanism section 17 specifies,
the fourth answers the question the milestone exists for, the fifth is
the part of section 17 that is about repetition rather than batching,
and the sixth decides whether the section stays.

**1. A count greater than one.** *Built.*

- `SYS_BATCH_MAX_ENTRIES` exists and bounds the array. Every entry is
  validated before any entry executes, per section 17.2, so a batch that
  cannot run does not half-run.
- Per-entry status written back into the caller's array, per section
  17.6.

*Check.* A batch of several entries performs several operations and
writes a status for each. A batch with a bad entry in the middle
executes none of them and returns `SYS_ERR_INVALID_ARG`, which is
checked by observing that the first entry's effect did not happen. A
batch longer than the bound is refused.

*Built.* `SYS_BATCH_MAX_ENTRIES` is sixteen, a submission carries
between one and that many, every entry is checked before any entry runs,
and each writes its own status.

**The descriptor already had the shape.** `flags`, `status`, `user_data`
and the two register tags have been in `struct sys_op` since M1, and
`sys_op_exec` has been writing the status all along. Nothing needed
adding to the interface: what was missing was a count that could exceed
one, which was refused in three lines at the top of the entry path.

**Signals on a timeline are what the entries do**, because a timeline
only goes up. After a batch of three the counter says how many ran, and
after a batch that was refused it says whether any did. An operation
with no visible effect would leave the second question unanswerable,
which is the question the stage is actually about.

**An existing check caught the first attempt, and it was checking the
right thing.** Validating up front and returning on the first bad entry
meant that entry never reached the code that writes its status, so a
refused entry said nothing about why. `trespass` has asserted since M2
that a descriptor pointing at memory the program does not own gives
`SYS_ERR_FAULT` from the call *and* from the entry, and it failed. The
validator writes the status it refuses on now. Section 17.6 wants every
entry to report its own outcome and an entry refused before it ran is
still an entry with an outcome.

**Both halves are falsifiable.** Validating as it goes rather than up
front leaves a refused batch having advanced the counter, which fails
with `a refused batch advanced it to 6`. Executing only the first entry
fails with `three entries advanced it to 1`.

The full bound runs too, sixteen entries in one call, which is what the
next stage measures against section 13.3.

*Reviewed.* Two defects, one of them a regression the stage introduced
into a rule it was otherwise being careful about.

**A killed process ran the rest of its batch.** The check that a dying
thread leaves the kernel was where it had always been, after the
operation, and the stage turned one operation into up to sixteen without
moving it. So a process killed while blocked in the first entry would
execute the other fifteen before noticing. Section 10.5 asks for the
unblocking to be prompt, and the entry boundary is exactly where section
17.6 says the batch is interruptible, so the check belongs inside the
loop and is there now.

That is the same mistake in shape as the one the stage made and caught
with the status write-back: a thing that was correct when the count was
one, left alone while the count stopped being one. Both were in the same
twenty lines. Turning a scalar into a loop means asking of every line
around it whether it belonged to the operation or to the call.

The check is still load-bearing. Removing it fails the run with `join
orphan: it did not finish`, which is a program killed while wedged in a
server that never replies, and the whole of section 10.5's machinery
resting on that one line.

**And the check put a kilobyte of arrays on a four-kilobyte kernel
stack.** Seventeen descriptors and seventeen argument structures, in a
frame called from the boot path, which already has frames of its own.
The guard word below each kernel stack would have caught the overflow
and named it, which is not the same as not writing it. They are static
now; the check is single-threaded and runs once.

**What was looked at and left.** The kernel's own `run` helper reaches
`sys_op_exec` directly rather than going through the entry point, so the
boot checks that use it do not exercise the validation this stage added.
The batch check goes through the real entry point and does. That is an
asymmetry worth knowing about rather than a defect: `run` predates the
entry path having anything to validate, and every user program goes the
long way.

**2. `LINK`, and what a chain does when an entry fails.** *Built.*

- A per-entry flag. Linked entries run in order, the first failure
  cancels the rest of that chain with `SYS_ERR_CANCELED`, and the chain
  ends at the first entry without the flag.
- Several independent chains may share a batch, and one failing does not
  touch another.

*Check.* A chain of three whose middle entry fails leaves the third
`SYS_ERR_CANCELED` and the first's effect intact, and an unlinked entry
after it still runs. Chains are not graphs, so the check also confirms
that the thing section 17.5 says is expressed with fences is not
expressed with `LINK`.

*Built.* `SYS_OP_LINK` is the low bit of a flags field that has been in
the descriptor since M1 and had never meant anything. Linked entries run
in order, the first failure cancels the rest of that chain, the chain
ends at the first entry without the flag, and an entry outside it is
untouched.

**Five entries say all of it.** A chain of three whose middle entry
fails, then two on their own. The first runs, the second fails, the
third reports `SYS_ERR_CANCELED` without running, and both of the
independent entries run. The counter reaches three of five, which is
what says the cancelled entry did not quietly happen anyway.

The failing entry fails by signalling a value the timeline has already
passed, which section 9.1 makes an error. That is worth saying because
the alternative was arranging a broken handle, and an entry that fails
for a reason the reader has to reconstruct makes the check about the
arrangement rather than about the chaining.

**Both directions falsify.** Never cancelling leaves the third entry
reporting `0`, which fails with `the rest of the chain said 0`. Never
ending a chain cancels the independent entries after it, which fails
with `an independent entry was cancelled with it`. The two mistakes are
opposite and a check that caught only one of them would be half a check.

**And the thing `LINK` cannot say is checked too.** Section 17.5 says
chains are not graphs and that a join is written with a fence instead:
two producers advancing a timeline and a consumer waiting on the final
value. That batch runs, and there is no arrangement of flags that would
express it, because a flag can only say that the next entry belongs to
this one. Checking the mechanism that replaces `LINK` in the same place
as `LINK` is what keeps the section's claim from being a sentence nobody
tested.

**What is not built and is section 17.5's last line.** On abort, every
timeline the batch was going to signal is force-signalled in error
state. That is about issuing ops, which return timelines and do not
exist, so there is nothing for a cancelled entry to have been going to
signal. It is recorded here rather than implemented against nothing.

*Reviewed.* Two defects, and the first one changed an answer that had
been decided on purpose somewhere else.

**The validator tested the length where it should have tested the
pointer.** `sys_op_exec` refuses an entry whose argument pointer is
non-zero and unreachable, and lets a null pointer with a length through
to the operation, which answers `SYS_ERR_INVALID_ARG`. The comment there
says so and says why: a null pointer with a length is a malformed
descriptor rather than an unreachable one. Stage one's up-front
validator tested `arg_len != 0` instead, so that case became
`SYS_ERR_FAULT`, and a decision written down in one place was reversed
in another by someone who had not read it.

Nothing caught it, because no check in the tree submits an entry shaped
that way. There is one now, and putting the old condition back fails
with `an entry with a length and no arguments: expected INVALID_ARG, got
FAULT`.

**The two checks are the same check, and that is the point rather than
the accident.** Section 17.2 wants every entry validated before any
entry runs, and each operation validates again when it runs, so an entry
is reach-checked twice. That is what not half-running a batch costs, and
it is a number stage four has to put on the batch side of its comparison
rather than a duplication to be pleased about having noticed.

**And the join check would have hung rather than reported.** It waits
on a timeline value the two entries before it have already reached, so
it returns at once; with no deadline it would wait for ever if those
entries ever ran out of order. That is a check whose failure mode is a
hang, which this milestone has now called out twice in other people's
work. It carries a deadline that only an ordering failure can reach.

**Checked and found sound.** The kill check sits after the branch rather
than inside it, so an entry that was cancelled still leaves the kernel
if the process has been killed. A cancelled entry does not become the
call's return value, which is right: the call reports the first entry
that failed, and an entry that never ran did not fail.

**3. What a batch does to interrupt latency.** *Built.*

- Section 13.3 says `SYS_BATCH_MAX_ENTRIES` does not appear in interrupt
  latency and the cost of one entry does. That has never been testable
  because the count has never exceeded one.
- The preemption point M9 placed at the entry boundary is what makes it
  true, if it is true.

*Check.* The points a batch passes equal its entry count, which is a
count and is identical in all four configurations. The longest
interrupts-disabled region does not grow with batch size: measured at
one entry and at the maximum, using M9's instrument, with the site
reported both times so that a change of site is not mistaken for a
change of length.

*Built.* Section 13.3 says `SYS_BATCH_MAX_ENTRIES` does not appear in
interrupt latency and that the cost of one entry does. The sentence has
been in the design since before there was a batch. It is true at Tier 0
and false at Tier 2, and the reason is the deviation M8 recorded.

| | one entry | sixteen entries | site |
|---|---|---|---|
| Tier 0 riscv64 | 59 ticks | 49 ticks | `timeline_signal` |
| Tier 0 aarch64 | 349 ticks | 306 ticks | `timeline_signal` |
| Tier 2 riscv64 | 134 ticks | 795 ticks | `klock_enter` |
| Tier 2 aarch64 | 725 ticks | 3431 ticks | `klock_enter` |

**The site says the whole thing.** At Tier 0 the longest region inside a
batch is the operation's own critical section, and there are sixteen of
them rather than one long one, so the batch's length does not enter the
figure. At Tier 2 the longest region is the kernel lock, taken once at
the entry and held with interrupts disabled until the call returns, so
sixteen entries is one region sixteen entries long.

Both ports agree at each tier, and the site is the same in each pair,
which is why the check reports it: a figure that grew because a
different region appeared would be a different result with the same
numbers.

**So the preemption point is a count and not yet a preemption point.**
M9 placed it at the entry boundary and said it counts rather than
yields, because what that stage needed was a number. This stage is what
the number was for, and it says the boundary is marked and nothing stops
there. Section 13.3's claim needs the batch to be interruptible at every
entry, and at Tier 2 it is interruptible nowhere, because the lock M8
took at the entry is held across all of it.

**The claim is not wrong about the design.** It is wrong about this
implementation, and the thing that makes it wrong is the same deviation
M9 measured and found cost nothing. It cost nothing on a workload with
no batches. This is the workload that shows what it costs, and the cost
is proportional to the batch size.

**Two ways the check nearly reported nothing.** It first ran before the
measurement was armed and printed two zeros and `the bound does not
appear in it OK`, which is a check passing on no data at all. It refuses
a zero now.

And the count it asserts, that a batch of sixteen passes sixteen points,
was already true before this stage and stayed true through both defects
above. A count being right is not evidence that the thing it counts is
doing anything.

*Reviewed.* One defect that was harmless by luck, and one half of the
check that could not fail.

**Measuring one batch threw away the measurement of the run.** The
windowed figure and the run-wide figure were the same fields, so
clearing the first to ask what a batch cost also cleared the second.
M9's headline number, the longest interrupts-disabled region in the
whole run, was being reset twice in the middle of every run.

It survived, and only because the largest region in this system happens
in the IPC-heavy part of the session, which is long after the batch
check runs. The reported figure stayed inside the range M9 measured, so
nothing looked wrong. Had the batch check moved later, or had the
largest region moved earlier, the run-wide number would have quietly
become the maximum since the last clearing. They are separate fields
now.

**And the check reported a result it could never fail.** It printed
whichever of the two outcomes it saw, which is right for the tier where
the answer is the finding and wrong for the tier where the answer is a
property that should stay true. Section 13.3's claim holds at Tier 0, so
that tier asserts it: a run where the batch bound starts appearing in
the figure is a regression and now says so. Tier 2 still reports,
because asserting the behaviour that is wrong would mean failing on the
day somebody fixed it.

Holding interrupts across the whole batch at Tier 0, which is what Tier
2 does by accident of the kernel lock, fails with `the batch bound
entered the figure, 676 against 97`.

*Since fixed.* That assertion was a duration compared against a
threshold, which is the one thing section 13.3's own rule says never to
do, and it was not safe: with eight machines running at once the one
entry figure ranges from 48 to 86 ticks and the sixteen entry figure
from 45 to 204, and a run at 231 against 54 failed a build. On aarch64
at Tier 0 the two sit at 292 and 925 on a quiet host, which is a ratio
of three against a threshold of four. The comment that stood here
claimed the margin was an order of magnitude wider than the effect. It
was about a fifth of it.

What the check wanted to know is countable, and counting it says the
same thing exactly. The question is whether anything lets interrupts in
between one entry and the next, so the instrument now counts the
interrupts-off regions in the window as well as timing the longest. A
batch interruptible at every boundary has a region per entry and one
that disables interrupts once for the whole call has a single region,
however many entries are in it. Tier 0 reads one region for one entry
and sixteen for sixteen, beginning in `timeline_signal`; Tier 2 reads
one and one, beginning in `klock_enter`, which is M8's deviation stated
as a count rather than inferred from a ratio. Both are the same on both
ports and in every run: eighty boots under the load that broke the old
form give sixteen every time. The durations are still printed, because
what the regions cost is worth seeing, and nothing is asserted on them.

Holding interrupts across the whole batch at Tier 0 now fails with `one
region covered all 16 entries`, and a run that is neither shape fails
too rather than being passed over.

*Reviewing that.* Three things, and the second one matters most.

The accessor for the new count was the only one of the three window
functions without a null guard on the core pointer. Its neighbours both
have one. Fixed rather than argued about.

The window is the core and not the batch, which this milestone had
already written down as making the figure an upper bound rather than
the batch's cost. That was an understatement, and the new count made it
matter: the count is asserted exactly, so one thread dispatched inside
the window would add its regions and fail a check about the batch for a
reason that has nothing to do with the batch. Both measurements now
hold the dispatch off. The cost of not having done so was visible in
the durations all along: twelve runs of the sixteen-entry figure on
aarch64 at Tier 0 read between 275 and 301 ticks with the dispatch held
off, and without it two of the twelve read 1030 and 1143. That is
another thread's work inside the window, and it is what used to push
the old assertion against its four-times threshold.

And the last thing is the argument for having stopped asserting
durations at all. Sixty-four boots under eight machines at once, with
the dispatch held off and nothing else in the window: sixty-three of
the sixteen-entry figures fall between 253 and 476 ticks and one reads
3972. Nothing was dispatched into that window. It is the host
descheduling the emulated core in the middle of a region, which no
amount of care inside the kernel can prevent and no threshold can
survive. The count was sixteen in all sixty-four.

*Reviewing the instrument itself.* The same fix was left half done a
third time, in the same place. Separating the console from the longest
region and not from the mean left the headline figure about the kernel
and the typical figure about the console, and the typical figure is the
one the console moves most: most regions are short and a few console
lines are very long. On one run core 0 at Tier 2 shows 23717 regions
with a mean of 112 ticks and 23370 without the console with a mean of
51. One and a half per cent of the regions doubled the typical one.

The count of regions and the total time behind the mean are now kept
both ways, and the report prints both. That the console-free means come
back to what M9 measured, 5 microseconds on riscv64 at Tier 2 against
the 5 in the table and 7 on aarch64 against the 7, is the first
independent check any of these figures has had: the number was recorded
in a document, the instrument was rebuilt underneath it twice, and it
came back the same.

A core that measured regions but none outside the console now fails
rather than reporting a zero as a figure, which is the rule the rest of
this file already follows.

Two things looked like defects and were not. `irq_dispatch_stamp`
dereferences its thread without a null check where the call that closes
the same measurement checks for one; the guards match their callers,
because both of its callers dereference the thread before calling. And
a region already open when another begins is silently abandoned rather
than counted, which is the same case the closing side documents and
declines to charge for.

*And a second time over the instrument.* Two defects, one of them
demonstrated rather than argued.

The dispatch check asserted that no interrupt reaches a program by way
of a preemption point, and tested it on one dispatch. The count was
recorded only when a dispatch was the longest so far, so it described
whichever dispatch happened to take the most time and said nothing
about the others. Planting three points on every dispatch that was not
the longest: the old form reports `0 points passed` and the run
completes, with fifteen of sixteen dispatches having passed three
points each. It now keeps the most any dispatch passed, and the same
plant fails with `3 points on the way`. The figure it prints is still
zero, so the claim holds; it is now a claim about all of them.

The other is latent. The report of a preemption-point range folds in
any core that had visits, but the range is only ever written by a visit
that could be counted, so a core whose visits all crossed a core still
holds the zeroes it started with and contributes a low of zero that
nothing measured. Every measurement here keeps its visits on one core,
which is why it has not shown; both sessions were dumped per core to
check that rather than assume it. The guard now skips a core with no
counted visit rather than a core with no visit.

*A third time over it, and the best finding yet was a comment.* The
clock check reads the counter many times on one core, which establishes
that it runs and never goes backwards, and says nothing about the other
core reading the same one. Its own comment named the hazard exactly, in
the file, and stopped there: a per-core counter "would pass every other
check in this file and make every figure that crosses a core
meaningless". Nothing tested it, and every cross-core figure this
system prints is a subtraction of one core's reading from another's:
the donation across cores, and section 13.3's dispatch term, which is
stamped in a handler on one core and closed when the thread runs.

The test is a bracket. The boot core reads the counter, asks the other
to read it, and reads again; the other's value has to lie between the
two. It does, 627 ticks into a 1527-tick bracket. Adding a constant to
the second core's reading fails with `core 1 read 1798896, outside
798273 to 799649 on core 0`. Two counters with any offset between them
fail this, and two that agree to within the bracket are the same clock
for every purpose here.

Two smaller ones. The round trip counted its preemption points without
asking whether it ended on the core it began on, where the donation
beside it has always asked; the answer was right, because a thread does
not leave its core unless something migrates it, but it was assumed
rather than checked and the failure it guards against is a subtraction
that wraps past zero. And `intr_region_worst_site` was declared,
defined, and called by nothing.

The corrupted-file trap in the notes caught me again on the way: a perl
substitution whose pattern contained an escaped `||` inserted its
replacement at the start of the file instead of at the match. The build
would not have compiled, but nothing about the failure points at the
cause. Both edits were redone with awk.

*A fourth pass, over the check the third one added.* One bracket says
the two counters differ by less than that bracket, and the bracket was
152 microseconds wide. That is a weak claim to have been reporting as
though it settled the question, and it was not stated with the figure
attached, so nobody reading the output could tell how weak.

The bracket is taken eight times now and the narrowest is printed. It
runs to about 500 ticks against the 1527 of a single try. The
improvement is measured rather than asserted: with a constant added to
the second core's reading, a skew of 400 ticks passes the single
bracket eight runs out of eight and fails the eight-round form eight
out of eight, and a skew of 200 passes both. So the skew this detects
went from somewhere above 400 ticks to between 200 and 400, and what
the check establishes is a bound rather than an identity. The line it
prints now says which bound.

A first guess that a 900-tick skew would slip past the single bracket
was wrong, and testing it rather than writing it down is what said so:
it fails both, eight out of eight.

And the secondary's wait is bounded. It runs before that core has its
way in to the interrupt controller, so a core stuck there is a core
that cannot be poked; it now gives up and lets the other core fail the
check rather than both waiting on each other. The window itself is
older than this check: a core announces itself up when it claims its
state, which is well before it can take an interrupt, and the lock
check already waited in the same gap.

*A fifth pass, and the question none of the others asked: what does the
instrument not see?* Two things are discarded by design and neither was
counted, so nothing said whether the regions reported were most of them
or a few of them.

They turn out to be different in kind, and printing them as one number
invited exactly the wrong reading, which I made myself before checking.
An end that arrives with no region open closes nothing and loses
nothing: the region was already accounted for and this is the return to
user mode or the idle path closing a second time. There are 721 of
those against 21364 regions at Tier 2 and 748 against 1452 at Tier 0. A
region replaced while still open is a measurement gone: it began,
nothing closed it, and its duration is discarded. There are 26 of those
per run at Tier 2 and 25 at Tier 0.

So the blind spot is about one region in eight hundred at Tier 0 and
one in eight hundred and twenty at Tier 2, and if one of those was the
longest region in a run then the figure reported for that run is not
the longest. That is a small hole and it is now a stated one. The
counts are within one of each other on both ports and at both tiers,
which is what says they are structural: particular paths that re-enable
interrupts without going through the restore, and not noise.

A sweep of every declaration in the header against its definition and
its callers turned up `meas_hz`, declared, defined, and referenced only
by a comment. Removed, and the comment now names the clock rather than
the function. The same sweep over the instrument's per-core fields
found none that are written and never read.

**A limit of the instrument, stated rather than fixed.** The window is
the core and not the batch: anything else that core does between the
clearing and the reading is inside it, an interrupt included. That makes
the figure an upper bound on what a batch cost rather than the batch's
cost, which is the safe direction here, because contamination can only
enlarge the one-entry figure and make the growth harder to see. The
growth is a factor of five or six, so there is room.

**4. Whether expressing dependencies as data pays.** *Built.*

- The milestone's question, answered in counts on the paths that exist.
  The same work, done as several system calls and as one batch, with
  every crossing counted.
- M6's precedent says to look at more than the obvious column.

*Check.* Both forms are reported side by side, in trap-edge crossings,
entries validated, and kernel entries taken, and both figures are
identical in all four configurations. What the batch form saves is
stated as a number. If it saves nothing, that is the answer and stage
six acts on it.

*Built.* Eight signals, done both ways through the same entry point,
counted.

```
8 signals, one call each: 8 crossings, 24 checks
the same 8 in one call:   1 crossings, 17 checks
it saves 7 crossings and 7 checks
and 7 of the lock, 8 against 1
```

Identical in all four configurations, which is what a count is for. The
work either side is the same work and the check says how it knows: the
timeline advances by eight whichever form carried it.

**What it saves is per call and nothing per entry.** Seven crossings of
seven, seven acquisitions of the kernel lock, and seven of the
twenty-four checks. The seven checks it saves are the seven descriptor
arrays it did not have to validate. The other sixteen are the entries
themselves, checked once before they run and once when they run, and
those are the same sixteen whichever form carried them.

So the saving is exactly the fixed cost of a call, multiplied by one
fewer than the number of entries, and nothing else. That is a smaller
claim than the section makes and it is the claim the numbers support.

**M6's warning was to look past the obvious column, and the obvious
column is the crossings.** Eight to one looks like the whole result and
is half of it: the checks went from twenty-four to seventeen rather than
from twenty-four to three, which is what a reader who saw only the
crossings would have assumed. The engine does not make the kernel do
less work per operation. It makes it enter fewer times.

**What this does not measure is the dependency half.** These eight
signals do not depend on each other, so the comparison is of batching
and not of expressing dependencies as data. What `LINK` adds over eight
separate calls is that the caller does not return to user mode between
entries to decide whether to continue, and stage two showed that works;
the crossings it saves are the same seven counted here. There is no
separate number for the dependency idea, because on this evidence its
saving is the same saving.

**And nothing here needs a device**, which was the point of splitting
the milestone's two claims. This is the half that could be answered.
Whether seven crossings is worth an entry array, a validation pass, a
status write-back and a second way of doing what one system call
already does is stage six's judgement, and it now has a number to make
it on.

*Reviewed.* The stage's headline number was printed and not checked, and
the thing the plan told it to watch for turned out to be answerable.

**Both columns are asserted now.** The saving was reported as seven
crossings and seven checks with only the crossings asserted, so the
second figure could have changed without anything failing. The
milestone's product is the numbers, and a number nobody keeps is a
number that drifts. Removing the up-front per-entry check fails with
`9 checks against 16, not 17 and 24`, which is what a changed claim
should look like: visible, named, and requiring somebody to write the
new figure down on purpose.

**The batch does not win by skipping anything, and this can be shown
rather than asserted.** The plan said to watch for a batch that looks
cheaper because it was allowed to omit a check. The check it makes once
instead of eight times validates `count * sizeof(*op)` bytes, which is
the whole array: eight separate calls validate one descriptor each and
the batch validates all eight together. The same memory is checked
either way. That is amortisation of a fixed cost over more work, which
is the only honest way for a batch to be cheaper, and it is the only
place the numbers say it is cheaper.

**And the counts are per core, which is why the window is clean.** The
figures are differenced on the core the checking thread runs on, so a
system call made on the other core in the same interval does not enter
them. That is the M8 lesson applied in advance rather than after the
fact, and the assertion on the crossings would have caught it if it had
not been: eight and one are exact, and any contamination makes them not
be.

**What the numbers still do not say** is what a crossing costs. They
count seven of them saved and measure none of them, which is the
milestone's own rule working as intended and also its limit: whether
seven crossings is worth the mechanism cannot be settled by counting
them alone. Stage six has to weigh a count against a quantity of code,
and it should say that is what it is doing.

**5. Prepared programs, validated once.** *Built.*

- Section 17 has a program validated once and executed many times, which
  is a different claim from batching: it is about repetition rather than
  about dependency.

*Check.* A prepared program executed N times validates its entries once,
which is a counter, and a program whose handles have gone stale between
executions is refused at execution rather than running with them.

*Built.* A program is a validated batch the kernel keeps. Loading
checks it once; executing runs it without checking it again.

```
4 entries, 4 times as a batch:   36 checks
loaded once and run 4 times:      26 checks
validating once saved 10 of them
```

**The entries are copied into the kernel, and that is what makes the
validation last.** A program whose entries stayed in the caller's memory
would have to be checked at every execution, because the caller could
have rewritten them between one and the next, and then nothing would
have been validated once. The copy is the mechanism, not an
optimisation of it.

**What cannot be validated once is the handles, and the check says so.**
A handle is a slot in a table, and what it names can change or go away.
Every execution looks them up again, so a program whose handle is closed
after loading is refused when it next runs rather than running against
whatever the slot now holds. Resolving them at load instead fails with
`executing it afterwards: expected STALE, got OK`.

So what a program saves is exactly the static half: the array is the
kernel's and needs no reachability check, and the opcodes were checked
when it was loaded. What it still pays, every time, is one check per
entry as the entries run, plus one for the operation that asked.

**The arithmetic had to be corrected twice, in the same direction both
times.** The predicted figure was 25 and the answer was 26, because the
operation that loads a program is itself an entry and pays a check like
one. Before that the prediction was 16 against an answer of 20, for the
same reason applied to executing. That is section 17.7's claim about
there being one implementation showing up in the accounting: an
operation that runs a batch is not outside the batch machinery, it is
inside it, and the counting notices before the reader does.

**And the load is inside the measured window.** Leaving it out would
have compared the executions of one form against the whole of the
other, which is how a mechanism comes to look free. The saving is ten
checks for four executions of four entries, and it grows with the
number of executions and not with anything else.

**What is not built, and no stage of this milestone claimed it.** The
interface in section 17.7 has `sys_prog_exec` take an array of handles
that the entries name by position, which is the "typed registers with
tags" of the *Needs* list. Programs here carry their own targets, so
they can be re-executed but not re-aimed. The six stages of this
milestone never covered that Need and this is the place to say so
rather than let it pass unmentioned.

*Reviewed.* Two defects, and both were in what happens to the object
rather than in what it does.

**A prepared program was never freed.** Nothing else names one, so the
slot should go back when the handle does, and the release path knew
about timelines, processes, threads, connections and channels and not
about this. Four slots meant the fifth program a process ever loaded
was refused for want of room nothing was using.

Nothing caught it, because the check loaded one program and closed it
once. Loading and closing twice as many as the pool holds is the check
that would have, and it does: putting the leak back fails with `load 3
of 8 ran out of slots`. The lesson is small and keeps recurring in this
milestone: a new object type has a lifetime as well as a behaviour, and
the behaviour is the part that gets checked because it is the part the
stage was about.

**And the field that says who owns it was written and never read.** A
handle can be moved between processes, and a program's entries name
addresses in the memory of whoever loaded it. Executing one somewhere
else is safe, because every execution re-checks that those addresses
are reachable in the process running it, and it is wrong, because they
would mean something different there. It is refused now, which is the
answer that needs no argument about which of those two facts wins.

**What was checked and found sound.** The per-execution handle lookup
is what makes a moved or stale handle refuse rather than resolve, and
that was already checked. The copy of the entries into the kernel is
what makes the validation last, and a program whose entries stayed in
the caller's memory would have to be revalidated; the copy is not an
optimisation and the stage said so.

**6. Whether section 17 survives.** *Built.*

- The only stage whose outcome is deleting a section, and the entry has
  said since it was written that removing it costs nothing else.
- The decision rests on stage four's counts and on what stage three
  found about latency.

*Check.* The saving is stated as a number and set against what the
mechanism costs: the entry array, the validation pass, the status
write-back, and a second way of doing what one system call already does.
The stage says which way the evidence points and, if it points at
removal, says what would have had to be true instead.

*Built.* The evidence, and the decision the entry has been promising
since it was written.

**What it saves.** A batch of eight saves seven crossings of the trap
edge, seven checks and seven acquisitions of the kernel lock, all of it
the fixed cost of a call amortised over more work and none of it
per-entry work. A prepared program of four entries run four times saves
ten checks, which grows with the number of executions.

**What it costs.** About four hundred and fifty lines, of which the
mechanism proper is the loop, the chaining, the validation pass and the
prepared program object; the rest is the counting that answered this
question. At Tier 2 it costs latency: stage three found the longest
interrupts-disabled region growing about fivefold with batch size,
because the kernel lock is held across the whole call, which section
13.3 says should not happen and which is M8's deviation showing up
where nothing had looked before.

**And what uses it: nothing.** The run makes 733 submissions at Tier 0
and 758 at Tier 2, and every one of them carries a single entry except
the handful this milestone's own checks submit. The engine was built
and no program adopted it.

**That is not the verdict, because of why nothing uses it.** The
pattern that motivates the mechanism is a chain: walk to a file, then
read it, where the second operation names a handle the first produced.
A batch cannot express that here, because an entry's target is fixed
when the batch is submitted and section 17.7's answer to this, an array
of handles the entries name by position, is the one Need of this
milestone that no stage built. So the workload that would justify the
engine is the workload the engine cannot yet carry, and the seven
crossings are a measurement of a check rather than of anything the
system does.

**The decision.** Section 17 stays, and its claims are narrowed to what
was measured rather than left as they were.

Removing it now would be deciding against the idea on evidence that did
not test the idea, which is the error this project's method exists to
prevent. Keeping it as written would be leaving claims standing that
five stages have shown to be larger than the facts.

What also argues for keeping it is a thing the milestone did not expect
to find. Section 17.7 says the direct-invoke shim means every operation
has exactly one implementation rather than a batch path and a separate
syscall path to keep in agreement, and that is now literally true: a
single operation is a one-entry batch and there is no other route into
the kernel. Deleting the engine would not delete that code, it would
delete the loop around it. The marginal thing being judged is smaller
than it looks.

**What would have had to be true instead**, and what would settle it:
build the argument substitution, convert one real client to it, and
measure the same way. If a walk-then-read in one submission does not
beat two, on counts, the section goes. That experiment is named here so
that the next person to open this does not have to rediscover which one
it is.

*Done when* one batch issues several operations, collects their results
and reports a status for each, with the submitting thread's own context
executing all of it and donation unbroken throughout, and a prepared
program re-executes without revalidation. **Not** when three transfers
are in flight, which needs a device this tree does not have; that half
of the original condition is recorded as untested rather than quietly
dropped.

*Would falsify.* The most novel idea in the design. Whether expressing
dependencies as data actually beats a few more system calls, and whether
the branchless form stays sufficient once real code uses it. If it does
not pay, it is removed, and section 23 is already arranged so that
removing it costs nothing else.

*Watch for.* The measurement being of the wrong thing, which M9 caught
five times in six stages. A comparison of two forms is the easiest place
in this project so far to measure something nobody meant, because the
two forms differ in more than the one thing being studied and the
difference that shows up is not necessarily the one being looked for.

*Size.* Medium, and unchanged. The mechanism is small; the measurement
is where the work is, and the honest accounting of what cannot be
measured is part of it.

### M11. Substitution

**Section 17.4: an entry that takes its argument from the one before it.**

**Done.** All six stages are built. Section 14 says what it answered,
what it could not, what the building kept finding, and what it left
untested.

M10 named the experiment that would settle its own open question, in
the paragraph above this one: build the argument substitution, convert
one real client to it, and measure the same way. This is that
experiment.

**Why the question is still open.** M10 measured what a batch saves and
the answer was exact: the fixed cost of a call, multiplied by one fewer
than the number of entries, and nothing per entry. But every batch it
measured was a list of operations that happened to travel together.
None of them depended on another, because the mechanism for expressing
a dependency was never built. So what M10 weighed was bundling, not
dependency ordering, and section 17's actual claim, that expressing
dependencies as data beats a few more system calls, went untested.

The descriptor has carried `reg_in` and `reg_out` since M10, with a
comment saying they are meaningful only to the parts of the engine that
are not built. This milestone is about making them mean something.

*Needs.* A register file per execution, typed and zeroed at entry
(section 17.4). An entry reading its target or an argument from a
register. `SYS_OP_IPC_SEND` writing a reply's moved handle to
`reg_out`. The abort rule of section 17.5's last line, which registers
are what make enumerable.

**The two halves M10 split are still split, and this one takes the
half that has no device in it.** Substitution needs no hardware: it is
about what one entry can hand the next inside a single execution. The
transfers-in-flight half still needs a device that transfers on its
own, and stage five asks honestly whether this tree can have one rather
than assuming either answer.

**1. A register file, typed and private to one execution.** *Built.*

- `SYS_BATCH_REGS` is four. The chains section 17 describes carry one
  intermediate value each, a walk handing a connection to the entry
  that reads on it, and a batch may hold several. It is interface
  rather than configuration, because a program names a register by
  number.
- Each register holds a handle or a scalar and says which. Empty is a
  third state and not the number zero: a register nothing has written
  is refused rather than read as a handle of value zero, which is what
  makes a batch that names a register an earlier entry never wrote fail
  instead of proceeding.
- The file lives on the stack of the call that runs the batch, and of
  the call that runs a single operation, because one entry is an
  execution of one entry. That placement is what makes "private to one
  execution" true without anything having to clear it afterwards.
- *Checked directly rather than through a submission*, because nothing
  writes a register from an entry yet; the entries that use it are the
  stages after this one. What can be established here is every property
  the rest of the milestone rests on, and those properties are all
  refusals. That an execution actually sees an empty file is checked
  end to end in the stage that gives an entry something to write.
- *What settles it.* Four ways to break it, each caught by a different
  line. Dropping the tag check from a handle read gives `empty register
  0 was readable`. Leaving the tags alone in the initialiser gives
  `register 1 survived an execution`. Dropping the bound from the tag
  enquiry gives `asking what is outside the file: expected
  INVALID_ARG, got OK`. Not updating the tag on a write gives `reading
  it back: expected OK, got INVALID_ARG`.
- *Reviewed.* The stage said the file lives on the stack of the call
  that runs the batch and it did not: nothing in the batch path owned
  one, and the check called the initialiser on a local of its own. The
  words described a structure that had not been built. It is built
  now, and `sys_op_exec` carries it without reading it, which is how
  the placement gets settled rather than asserted. The enquiry that
  reports what a register holds also answered `REG_EMPTY` for an index
  outside the file, which is the one answer that reads as an
  invitation to write it; it returns an error like every other call in
  the file now.

**2. An entry takes its target from a register.** *Built.*

- Two flags rather than a sentinel index, because register zero is a
  register: `SYS_OP_TARGET_FROM_REG` says the target is whatever
  `reg_in` names, and `SYS_OP_RESULT_TO_REG` says the handle this entry
  produces goes where `reg_out` names.
- *The writer had to be one section 17.4 names.* The stage as planned
  needed something to write a register before anything could read one,
  and the obvious candidate was to let any operation that produces a
  handle write one. That would have been interface this project
  invented. Section 17.4 gives two producers, the issuing ops and a
  send whose reply moved a handle, so the send is what writes here and
  the rest are refused by name with `SYS_ERR_NOT_SUPPORTED`.
- *Substitution happens on a copy.* The descriptor documents one field
  the kernel writes, and a prepared program of section 17.7 is
  validated once and executed many times from one stored copy, so a
  kernel that edits the entry has made the second execution a
  different program. The resolved target lives on a copy and only the
  status goes back.
- *Both registers are settled before the operation runs.* An entry
  naming an empty register is refused having done nothing, and an
  entry whose result register is out of range is refused before it
  sends rather than after, because a send that has happened cannot be
  taken back for want of somewhere to put the answer.
- A reply carrying no handle leaves the register empty rather than
  writing `SYS_HANDLE_INVALID` into it, which is what section 17.4
  asks for by name.
- *Check.* The pair the design uses to explain itself, against the
  filesystem server that has been in the tree since M1: a walk that
  answers with a connection, and a read sent on that connection, in
  one submission. The second entry never names the first.
- *What settles it.* Four ways to break it, each caught by a different
  line. Not substituting at all sends the read on the literal target
  and gives `the pair returned -7`. Substituting into the caller's
  entry instead of a copy gives `the kernel wrote into the entry`.
  Writing the moved handle even when the reply carried none gives `a
  pair whose first entry found nothing: expected INVALID_ARG, got
  STALE`, which is section 17.4's own argument reproduced: without the
  rule the dependent entry looks up handle zero instead of being
  refused. Checking the result register after the operation instead of
  before gives `a result register outside the file: expected
  INVALID_ARG, got OK`.
- *And it closed what stage one could not check.* That an execution
  starts with an empty file is now seen through the batch path: the
  dependent entry submitted on its own, in a later execution, finds
  nothing where the pair had left a connection.
- *Reviewed.* Three defects, two of them made by this stage.

  Adding register fields to an entry made two validators disagree. A
  submission checks every entry before any entry runs, per section
  17.2, and a prepared program checks them once at load so its
  executions do not have to; the second says in its own comment that it
  does "the same validation a submission gets", and after this stage it
  did not. A prepared program naming a register that does not exist
  would have half-run on every execution. Both now call one function,
  so the next field cannot make them drift again, and the rule for what
  belongs in it is written there: whether the answer can change between
  the check and the entry running. Which register a number names cannot.
  Whether that register holds a handle can, and stays where it is.

  The submission path had the same hole before that: the register
  fields were checked when the entry was reached rather than before any
  entry ran, so a batch refused for a bad register index had already
  run the entries before it. `trespass` now submits a pair whose second
  entry names one register past the end and asserts the first entry's
  status is untouched, which is section 17.6's own way of saying it was
  never considered. Removing either the check or the shared validator's
  register range fails it with `exited 16`.

  And a fault on an entry with a substituted target wrote the status
  into the copy the substitution ran on rather than into the caller's
  descriptor, so the caller was left with whatever it had written. It
  is reachable only if memory stops being reachable between the check
  every entry gets before any entry runs and the one the operation
  makes, which is the race that duplicate check exists for and one no
  check here can construct. Fixed, and stated as unchecked.

**3. A real client carries its dependency in a register.** *Built.*

*Re-scoped by stage two.* This stage was to make `SYS_OP_IPC_SEND` the
writer of a register and show a walk handing a connection to a read.
Stage two had to build that to have anything to read, and used the pair
as its own check. What was left is the part that was always the point:
a client that is not a check.

- *A third client rather than a converted one.* `fs_client` is the
  baseline `fs_batch` is measured against, and section 25.7's whole
  comparison rests on it staying the plain three-round-trip form.
  Converting it would have destroyed the measurement this milestone is
  trying to add to. `fs_chain` does the same work in one submission and
  sits beside both.
- Four entries: walk, keeping the handle its reply moved; read on that
  register; close, on the same register again; and a `HANDLE_CLOSE`
  taking its target from the register too. That last one is there
  because the program never learns the handle's value and so cannot
  close it any other way. Section 17.5 names that as a limit when a
  chain aborts; it is the same limit when a chain succeeds, and an
  entry reading a register is the answer to it.
- *Check.* The same bytes, so the exit status is unchanged and
  `run_client` asserts it. What moved is the crossings, which are now
  counted per process rather than for the machine: the machine-wide
  figure includes whatever else was running, and on two cores that is
  not a small difference.
- *The three-way comparison, which is the stage's product.*

```
fs_client  5 submissions, 5 rendezvous, 1 connections, 12 entries, 6 invalidations
fs_batch   4 submissions, 3 rendezvous, 0 connections,  4 entries, 2 invalidations
fs_chain   2 submissions, 5 rendezvous, 1 connections, 12 entries, 6 invalidations
```

- Read across rather than down. `fs_chain` and `fs_client` agree on
  every column but the first: the same rendezvous, the same connection
  drawn from the server's pool, the same buffers remapped. Substitution
  saved four crossings of the trap edge and nothing else, which is
  exactly what M10 said batching saves and no more. `fs_batch` saves
  two rendezvous and the connection, and that is the protocol's saving
  rather than the engine's. The two mechanisms are visible apart
  because the three lines are side by side.
- *So the crossings are not the argument.* M10 already measured that a
  batch of N saves N-1 crossings whether or not the entries depend on
  each other. What substitution adds is that these four entries could
  not have been written down as one batch at all before it: the second
  names a handle that does not exist until the first has answered. That
  is a fact about what the caller can express, not a number, and the
  milestone has to say so rather than presenting a saved crossing as
  though it were the answer.
- *What settles it.* Two ways to break the dependency, both caught by
  the byte count. Taking the flag off the read so it uses its literal
  target, and taking it off the walk so nothing is written, each give
  `fs_chain: read 2 bytes, expected 31`.
- *What it cost the compared file.* Six more buffer remappings and one
  more page table at Tier 2, which is what a client that moves buffers
  costs and matches what `fs_client` costs on the same line. Updated
  deliberately.
- *Reviewed, and it found the thing this stage was standing on.*

  The program was called `fs_chain`, its comment described a chain, and
  it set no `LINK` flag on anything. Section 17.5's chaining is the
  mechanism for "these belong together and a failure ends the rest",
  and without it a failed walk let three more entries reach the kernel
  and be refused one at a time. Linked now, and checking the replies in
  the order they ran rather than an order that would name a later entry
  as the cause of an earlier one's failure.

  Then the check written to prove the chain aborts showed that it does
  not, for a reason neither the program nor the stage had noticed. A
  walk for a name that does not exist **succeeds**: the message was
  delivered and answered, and the refusal is in the server's payload,
  which the kernel does not read. Section 12.4 requires a `status`
  field on the message for exactly this, and says what happens without
  it in words that describe this program: "a `FS_OP_WALK` for a file
  that does not exist is a completely successful `SYS_OP_IPC_SEND` as
  far as the kernel can see, so a linked chain would carry on and send
  a read on a connection that was never returned." The field is not in
  `sys_msg_t`. The rule is not built.

  What stops the read is the empty register, because a walk that fails
  moves no handle. That is the right outcome by the wrong route, and it
  holds only for as long as no operation both fails and moves a handle.
  The check now asserts what is true, names what is missing, and would
  fail the day the kernel learns to see a server's refusal, which is
  how it will be noticed. Stage four takes the field.

**4. What a chain does when a server says no, and what an abort
signals.** *Built.*

*Widened by stage three's review*, which found that the first half of
this was missing entirely and that a program was relying on it without
either of us noticing.

- **The status field of section 12.4.** A reply now carries a `status`
  of type `sys_err_t` outside the opaque payload, and the kernel copies
  it into the entry, treating non-zero as failure and learning nothing
  else about it. Section 25.6 already makes servers report failure as
  `sys_err_t`, so the protocols pay nothing they were not paying; both
  of the filesystem protocol's reply shapes begin with one, which is
  why one line in each server sets it.
- *What it changed everywhere else.* A request the server refused is no
  longer a successful send. `sys_msg_send` returns what the server
  said, so a caller learns of a refusal without decoding a payload, and
  the four places in `trespass` that asserted the old split now assert
  the new one. That is the interface change section 12.4 implies and it
  is worth saying out loud rather than leaving to be discovered.
- **And section 17.5's last line.** A batch that aborts force-signals,
  in error state, every timeline it was going to signal, so a thread
  waiting for a value that will now never arrive is told rather than
  left to a timeout.
- *Not one something else in the batch will still signal.* The chain
  being cancelled ends at the first entry without the link flag and
  everything after it runs, so a timeline one of those will advance is
  not a timeline nobody can advance. Forcing it would break an
  independent chain for the failure of this one, which is what the
  first attempt did and what M10's chaining check caught within a
  minute of it being written.
- *Check.* A thread waits on a timeline with no deadline, so a pass
  cannot come from a timeout. A chain whose first entry fails is
  followed by the entry that would have signalled it. The waiter is
  released with `SYS_ERR_OWNER_DEAD`.
- *What settles it.* Three ways to break it. Not reading the reply's
  status gives `a walk for a name that is not there: expected NO_ENT,
  got INVALID_ARG`, which is the register catching what the status
  field should have. Signalling nothing on abort leaves `the waiter
  still waiting for a signal that will never come`. Forcing a timeline
  a later entry will signal gives `an independent entry was cancelled
  with it`.
- *What this did not settle.* Error state is permanent and belongs to
  the timeline rather than to the batch, so a batch aborting poisons an
  object other processes may hold a signal right over. Section 17.5
  asks for exactly that and section 9.3 rule 2 is the same shape, but
  the two are not obviously the same situation: a party that has died
  cannot signal again, and a batch that failed once might. It is built
  as the section says and recorded here as a question rather than
  smoothed over.
- *Reviewed.* The field was added to the interface and set in two
  servers out of three that can refuse a request. The block store,
  which answers everything but a read with `NOT_SUPPORTED`, and the
  console driver both left it zero, so their refusals were still
  invisible to the kernel and a chain sent to either would still have
  carried on. That is the defect section 12.4 exists to prevent,
  surviving in two thirds of the places it applies. Both set it now.

  A server that forgets is not detectable by any check. What a check
  can reach is the servers it talks to, and the one it talks to is
  covered twice over: a refusal about a name and a refusal about an
  operation, so that the field is exercised by more than one path
  through one server. Making the kernel-side server report `SYS_OK`
  for everything fails with `a walk for a name that is not there:
  expected NO_ENT, got INVALID_ARG`, which is the register catching
  what the field should have.

  Also checked and sound: the inline payload is a fixed sixty-four
  bytes rather than a remainder, so the new field grew the message
  rather than shrinking what a message can carry.

**5. Whether an issuing op can exist in this tree at all.** *Built, and
the answer is no.*

- *The answer, and the reason.* An issuing op starts work and hands
  back a timeline. Starting work means a device that transfers, and
  nothing in this tree transfers: there is no driver here that moves
  bytes without a thread moving them. The three opcodes go on refusing.
- *The route that looks open and is not.* The block store answers
  reads, so a read that issues could mean sending a request without
  blocking and signalling a timeline when the reply lands. That is an
  issuing send, and section 17.3 rules it out by name for a structural
  reason rather than a convenient one: a thread with an outstanding
  issuing send is not blocked on it, so the blocked client can no
  longer be the queue slot, and something else has to hold the reply's
  destination. That is a new object or a new thread state.
  `PROPOSALS.md` carries it.
- *What the stage found instead, which is narrower and more useful
  than the question.* The half of section 17.3 that is missing is not
  "issuing ops". Collecting timelines and waiting on them in one
  submission is built. Hardware that finishes on its own schedule is
  built: a timer advances a timeline at a deadline with no thread
  involved. What is missing is only an operation that starts device
  work, and it is missing because there is no device work, not because
  the engine cannot express it.
- *Check.* Three timers armed one after another and none waited for,
  then one submission that waits on all three. All three arrive and the
  submission returns. The same three armed and waited for one at a
  time, which is what a caller without an issuing op must do, is
  reported beside it.

```
    all three at once, in one submission: about 60 ms
    one at a time, the same three:  about 121 ms
```

- Counts asserted, durations reported. Sixty against a hundred and
  twenty-one shows that several completions can be outstanding at once
  and waited for in one submission by one thread with no worker pool,
  which is the engine's shape. It does not show concurrent work and
  cannot: a timer does no work, and the two figures are arithmetic
  about a clock. See the review below.
- *What settles the premise.* All three opcodes are asserted to refuse
  as unbuilt rather than as unknown. The dispatch check in the tree
  tries one of them; a premise resting on two opcodes nobody has ever
  called is not one to leave untested. Making them answer
  `INVALID_ARG` fails.
- *A stage that answers no is not a stage that failed.* It is recorded
  here and in section 17 rather than in a commit message.
- *Reviewed, and the numbers claim less than the words around them did.*

  A timer does no work. Three deadlines set from one origin are reached
  by the latest of them and three set one after another are reached by
  their sum, so sixty against a hundred and twenty-one is arithmetic
  about a clock and not evidence that anything ran at the same time as
  anything else. Nothing was transferring while the thread waited,
  because there is nothing here that transfers. The entry said "three
  things in flight", which invites reading the pair as a measurement of
  concurrent work, and it is not one.

  What the pair does show is the engine's shape: several completions
  outstanding at once, waited for in one submission, by one thread with
  no worker pool. That is worth having and is what isolates which half
  is missing. It is also the strongest thing available, because no
  arrangement of what is in this tree would demonstrate concurrent
  work: there is no work to make concurrent, which is the stage's
  conclusion reached a second time from the demonstration's own
  weakness.

  Checked and sound: the six timelines this adds bring the run to
  twenty-three of sixty-four, so the pool is not under pressure; and
  the one-shots are destroyed and their handles closed after they fire.

**6. What substitution pays.** *Built.*

- The measurement M10 could not make, on the client M10 named, with
  the counts asserted rather than reported. They are the same on both
  ports, at both tiers and in every run, which is what makes them a
  property of this kernel and these three programs rather than of the
  machine underneath.

```
fs_client  5 submissions, 15 checks, 5 rendezvous, 1 connections
fs_batch   4 submissions, 12 checks, 3 rendezvous, 0 connections
fs_chain   2 submissions, 12 checks, 5 rendezvous, 1 connections
```

- *What substitution saves.* Three crossings of the trap edge and three
  validation checks, against `fs_client` doing the same work. Nothing
  else moves: the same five rendezvous, the same connection drawn from
  the server's pool, the same buffers remapped. The three checks are
  the three array reach checks the three calls it removed would each
  have paid, so the saving is the fixed cost of a call multiplied by
  one fewer than the number of entries, and nothing per entry. That is
  exactly what M10 measured for batching without dependencies, arrived
  at again by a batch that has them.
- *So substitution does not pay in counts.* It pays in what can be
  said. Those four entries could not have been one batch before it,
  because the second names a handle that does not exist until the first
  has answered, and a caller had no way to write that down. What the
  numbers establish is that expressing it costs nothing extra: the
  dependency is carried for free, and the saving that comes with it is
  the ordinary saving of making one call instead of four.
- *And the answer to section 17's question is uncomfortable, which is
  why it is worth having.* Set beside the other row: the compound
  protocol request of section 25.7 saves two rendezvous and never draws
  a connection at all, where the engine saves three crossings and one
  fewer array check. For this workload, changing the protocol beat
  changing the kernel. The engine's saving is real and small and fixed;
  the protocol's saving is larger and grows with what the server can be
  asked to do in one message.
- *What settles it.* The table is asserted, so the numbers cannot drift
  without a run failing. Changing a row fails with the pair printed
  side by side. Taking the register off the third entry of `fs_chain`
  fails with `2 submissions, 11 checks, 4 rendezvous, 1 connections,
  against 2, 12, 5, 1`, which is the assertion catching a lost
  rendezvous rather than a lost byte.
- Entries and invalidations are on the same line and not in the table.
  They are zero at Tier 0, where one address space means a buffer that
  arrives is already where it needs to be, so they are a property of
  the tier and are reported rather than asserted.
- *Reviewed.* Two defects, and the first is the shape this project
  keeps finding.

  The table is keyed by image name and the lookup fell off the end
  without saying anything, so a client with no row asserted nothing and
  the run passed because it had not looked. A typo in a name, or a
  fourth client added later, would have removed the milestone's whole
  measurement in silence. A name that is not in the table is now a
  table that needs a row: renaming a row fails with `fs_chain: run as a
  client with no cost recorded for it`.

  And the per-process check count omitted what a prepared program pays
  at load, where the machine-wide count beside it includes them. None
  of the three clients loads one, so the table was not wrong, but a
  counter that means one thing here and another there is a counter
  waiting to be read wrong. Both count the same events now.

  Checked and sound: `fs_client` is run twice in a boot, from two call
  sites, and reports the same four counts both times, which is worth
  knowing because it means the figures do not depend on what ran
  before.

*Done when* one submission carries a dependency that separate calls
would have had to carry in user code, and the report says what that
saved. **Not** when three transfers are in flight, unless stage five
finds a way; that half stays recorded as untested rather than quietly
dropped, exactly as M10 left it.

*Would falsify.* Section 17.4. If substitution costs more than it
saves, or if the branchless enumeration turns out not to be
determinable once real code uses it, the register file goes and section
17 loses its second half. Section 23 is arranged so that removing it
costs nothing else, which is the same insurance M10 relied on.

*Watch for.* Measuring the wrong thing, which M9 caught five times in
six stages and M10 warned about in the entry above. The two forms being
compared here differ in more than substitution: one makes fewer calls
as well. Separating the saving that comes from bundling, which M10
already measured, from the saving that comes from the dependency, which
is what this milestone is for, is the whole difficulty.

*Size.* Small to medium. The register file is a few dozen lines. The
abort enumeration and the honest accounting in stage five are where the
work is.

---

### M12. The Latency Floor

**Section 13.3's first term: the region that spans a context switch.**

**Done.** All six stages are built. Section 15 says what it answered,
what it could not, what the building kept finding, and what it left
untested.

M9 measured both terms of section 13.3 and found the section aimed at
the wrong region. It says Tier 0 bounds latency by where preemption
points are placed. The binding region contains no preemption point and
cannot, because it is the switch itself: all four configurations agree
that the longest interrupts-disabled region begins in `channel_reply`
and ends in `channel_send`. A server replies, hands the reply over and
dispatches to the client, and the region does not close until that
client is back in the kernel sending again.

M9 named what would lower it and did not attempt either: not disabling
interrupts across a switch, or making the switch interruptible. This
milestone attempts the first.

**Why the first and not the second.** Making a switch interruptible
means an interrupt arriving with half a thread's registers saved, which
is a port question with a different answer on each architecture and no
way to get it wrong quietly. Not disabling interrupts across a switch
is a question about where this kernel chooses to put its
interrupts-off regions, which is the same kind of question the rest of
this plan has been answering, and it is answerable without touching
either port's assembly.

*Needs.* Nothing new. Every part of this exists: the regions are
instrumented, the figure is reported per core with the console
separated out, and the window figure can be cleared and read around any
stretch of code.

**1. Where the time in the binding region actually goes.** *Built.*

- A waypoint inside a region, placed where `channel_reply` stops
  manipulating state and starts dispatching. Everything before it is
  the reply's own work; everything after is the context switch and
  whatever the client does before it lets interrupts back in.
- *The longest marked region and not the longest region.* The two are
  not always the same one, and reporting only when they coincide said
  nothing at all in one configuration out of four. Tracking the marked
  ones separately answers the question in every run.
- *The answer.* The reply's own work is between a quarter and a half of
  the binding region, consistently across all four configurations.

```
riscv64 tier0    5396 ticks: 2382 before the dispatch, 3014 after
riscv64 tier2    6548 ticks: 3324 before the dispatch, 3224 after
aarch64 tier0   35978 ticks: 9675 before the dispatch, 26303 after
aarch64 tier2   42926 ticks: 22855 before the dispatch, 20071 after
```

- So the switch and what follows it is the larger half everywhere, and
  the reply's own work is a quarter to a half. Separating that work
  should take a quarter to a half of the floor away and leave the
  switch. That is what stage two is for, and stage one has told it what
  to expect rather than leaving it to be discovered afterwards.
- *What is asserted and what is reported.* The durations are the host's
  and are reported. What is asserted is that the region was seen at
  all: the reply path is walked hundreds of times in every run, so a
  report with no marked region is an instrument that stopped measuring.
  Removing the mark fails with `no region carried a mark, so the reply
  path was never measured`.
- *Reviewed, and the first figures were of the wrong region.* A reply
  dispatches only when the client outranks the server, so a marked
  region that did not switch is an ordinary short region and not the
  one section 13.3 is about. The instrument counted both, and named
  whichever was longest after the path that switches. On one core the
  figure was a 405-tick region that never switched, sitting beside a
  6658-tick one that did; on riscv64 at Tier 0 the published 7263 was
  itself a region that never switched, against 5396 for the real one.
  The mark now records which thread placed it and the figure takes
  only regions that ended in another, so the name and the number
  describe the same thing.

**2. The reply's own work stops sharing a region with the switch.**
*Built.*

- `channel_reply` disabled interrupts once and did everything under it:
  returning the buffer right, copying the reply, moving a handle,
  unwinding a donation, releasing the client and dispatching to it. The
  release and the dispatch are now two regions instead of one, using
  the deferral the scheduler already has.
- *Why the split is where it is, argued from the state and not from the
  figure.* What the work above needs interrupts off for is that the
  client is being taken apart and put back together: blocked on us, its
  buffer right returning, a handle moving into its table, a donation
  unwinding. A thread that saw any of that half done would see a client
  that is neither blocked nor runnable. By the time the deferral is
  lifted none of it is true any more: the client is released and
  enqueued, this thread has given up what it held on the client's
  behalf, and all that is left is which thread runs next. An interrupt
  arriving in the gap finds a consistent pair and may dispatch to the
  client itself, which is the same outcome by another route.
- *The instrument changed with the stage.* Stage one's waypoint is
  gone. A region now records which thread was running when it began, so
  one that ends in another thread is recognised as having spanned a
  switch without any site opting in. That covers every path, including
  a path that switches without meaning to, which is a thing this
  milestone would want to know about.
- *What is solid.* The binding region no longer begins in
  `channel_reply`. In all four configurations it now begins in
  `channel_send`, which is a client blocking and switching away rather
  than a server replying and switching to it. A site is deterministic
  per build, so this is the host-independent half of the result.
- *What is suggestive.* Five runs of each configuration, before and
  after, sorted, in ticks:

```
riscv64 tier0  before 4784 5300 5316 5793  5910   after 4381 4413 4514 4774  6228
riscv64 tier2  before 5679 7310 7413 7721 11377   after 4917 5515 5640 6831  7448
aarch64 tier0  before 30593 32116 32727 34377 37597   after 28154 28507 28555 46767 47714
aarch64 tier2  before 37257 37873 46693 51493 59898   after 38890 42149 43515 44619 52492
```

- The medians fall by seven to twenty-four per cent and the ranges
  overlap, on aarch64 heavily. These are durations and a maximum of a
  sample, so the arithmetic is worth exactly what M9 said it is.
- *What did not happen, which is the finding.* Stage one predicted the
  floor would fall by a quarter to a half, being what the reply's own
  work was worth. It fell by much less, because the region behind it is
  nearly as long: taking `channel_reply` off the top exposed
  `channel_send`. **The binding region was not unique.** That is what
  stage three is for, and it now has a reason rather than a
  completeness argument.
- *What is not available.* The plan proposed asserting that the region
  count rises by one per split. It does not move measurably: about
  22,400 regions either way with a hundred and forty of run-to-run
  variation, which swamps the hundred and fifty replies that split.
  The count is not the countable half here; the site is.
- *What settles it.* Not lifting the deferral fails with `sched: server
  blocked while preemption was deferred`, which an existing check
  catches. Moving the restore before the release, which is the ordering
  the safety argument rests on, changes nothing that any check notices:
  the run completes. That ordering is argued and not checked, and
  saying so is better than implying the break test covered it.
- *Reviewed.* Two findings, and the one that mattered went the other
  way from what was feared.

  Section 13.3 has two terms and this stage moved one of them. Nothing
  in the stage checked the other, and lowering an interrupts-off region
  by handing the delay to a deferral would have moved the cost rather
  than removed it. Measured on the paced session, which is the one that
  dispatches at all: without the split 1914, 2340 and 2900 ticks; with
  it 1588, 1609 and 1927. The second term improved too, because an
  interrupt in the gap is now taken at once where before it waited for
  the region to end, and the deferral it then waits on is a few
  instructions long. Both terms down is the answer, and it was worth
  asking for rather than assuming.

  The comment describing the split was wrong about the mechanism. It
  said an interrupt arriving in the gap may dispatch to the client
  itself, and it may not: the deferral suppresses dispatch on the core,
  which is what a deferral is and is the whole reason it is held. What
  is true is that the interrupt is taken at once and the thread it
  wakes waits for the lift, which is the next instruction. The
  correction matters because the wrong version made the deferral sound
  incidental when it is the mechanism.

**3. The same wherever else a long stretch shares a region with a
switch.** *Built, and it changed nothing measurable.*

- Two more splits, both on the far side of a `sched_block`. A thread
  that wakes from a rendezvous was copying a message out before it
  restored interrupts, and that copy was inside the region the other
  thread had opened. On the receive side it is the staged message going
  to the caller's buffer; on the send side it is the reply doing the
  same.
- *Why they are safe, argued from the state.* Both copies are between
  two things only the waking thread can reach: its own staging message,
  which the other thread filled before it released this one and will
  not write again, and the caller's buffer, which is this thread's. On
  the send side the thread's own wait fields are cleared before the
  restore, so a second reply arriving afterwards finds `t_reply` empty
  and is refused as stale, which is the right answer to a reply to a
  request already answered.
- *What moved.* The region now ends immediately after the wake rather
  than after the copy. In every configuration it runs from
  `channel_send` disabling interrupts, across the switch, to
  `channel_recv` restoring them on the first instruction after waking.
  Nothing a woken thread does is inside it any more.
- *What did not move: the figure.* Five runs of each configuration,
  sorted, in ticks, against the same measure after stage two:

```
riscv64 tier0  stage2 4381 4413 4514 4774 6228   now 4281 4478 4851 5040 6552
riscv64 tier2  stage2 4917 5515 5640 6831 7448   now 4871 5147 5728 6100 7574
aarch64 tier0  stage2 28154 28507 28555 46767 47714   now 25360 31689 37249 38139 41670
aarch64 tier2  stage2 38890 42149 43515 44619 52492   now 26923 28703 30139 32365 49407
```

- The medians move in both directions and the ranges overlap almost
  entirely. The honest reading is that these two splits took nothing
  measurable out of the region, because what they took out was not
  much of it.
- *Which is the finding, and it is for stage four.* What is left in the
  region is the sender's rendezvous work and the switch. The rendezvous
  work is the part that genuinely needs exclusion: it pops the
  channel's receiver list, hands a buffer over between address spaces,
  walks a donation chain and puts a thread on a queue, all of it shared
  with whatever else might run. So stage four's question, whether what
  remains is the switch alone, already has half an answer: **it is not.
  It is the rendezvous plus the switch**, and the rendezvous is not
  obviously reducible.
- *The plan's check for this stage does not work and is recorded as
  such.* It asked for a region count that went up and a worst case that
  went down. Stage two established that the count is swamped by
  run-to-run variation, and the worst case did not go down. What is
  left is the site, which is deterministic per build and did move.
- *Reviewed, and the split had taken shared state out of its
  exclusion.* Moving the restore before the copy also moved
  `pulse_lost_flag` before it, and that is a read-modify-write on the
  channel: it reads whether pulses were dropped, reports it, and clears
  it. The other side of that flag is set in `channel_pulse` under
  `md_intr_disable`, so both sides were protected and this stage
  unprotected one of them. A pulse arriving from a handler between the
  read and the clear would have been dropped without anyone being told,
  which is the whole thing the flag exists to prevent.

  It is a take now: read and cleared while interrupts are still off,
  and the bit it becomes applied to the caller's message after the
  copy, because the copy overwrites the flags. The copy stays outside
  the region and the shared word stays inside it.

- *And the reason that was invisible.* The check for this flag asserted
  that a loss is not reported twice and never that it is reported once.
  Making the flag always read false passes that check perfectly: no
  loss is reported, so none is reported twice. The comment above it
  said "the receiver was told it missed something" and nothing tested
  it. Counting the reports and requiring exactly one now fails with
  `the loss was reported 0 times, expected once`, which is what the
  first break test of this fix should have said and said nothing.

**4. What is left, which should be the switch alone.** *Built, and it
is not.*

- The switch is measured across rather than around. A thread about to
  give up the core stamps the clock, and the thread the core comes back
  in reads that stamp on its first instruction after its own switch
  returned. Timing around `thread_switch_to` would measure how long the
  outgoing thread was away, which is a property of the workload.
- *The answer, measured inside the region rather than beside it.* How
  much of the binding region was the switch, for the same instance of
  it:

```
                  region   the switch in it
riscv64 tier0       4642                 41
riscv64 tier2       4929                 52
aarch64 tier0      27512                240
aarch64 tier2      54915                506
```

- **The switch is about one per cent of the region that spans it**, in
  every configuration, on both ports and at both tiers.
- The switch on its own, across the whole run, for scale: 469 of them
  at Tier 0 and about 4,480 at Tier 2, with a mean of forty-eight to
  four hundred and twenty-seven ticks, which is four to seven
  microseconds on either port. The two ports agree about that and about
  the region, which is what says both are properties of the kernel's
  shape rather than of either machine.
- *So section 13.3's floor is not the switch.* M9 established that the
  binding region spans a switch and that no preemption point can
  therefore break it, and both of those remain true. What does not
  follow, and what this milestone assumed until it measured, is that
  the switch is what the region is made of. It is not. The region is
  the rendezvous work that has to be atomic with the switch, and the
  switch is a rounding error inside it.
- *Which means the two remedies M9 named are aimed at one per cent.*
  Not disabling interrupts across a switch is what stages two and three
  did, as far as the state allows. Making the switch interruptible
  would attack the forty ticks and leave the four thousand six hundred.
- *What settles it.* That a switch was measured at all is asserted, for
  the same reason the crossing region is: a run measuring none has an
  instrument that stopped rather than a machine that never scheduled.
  Removing the stamp fails with `no switch was measured`. The durations
  are the host's and are reported.
- *The instrument costs what it measures.* Two clock reads per switch,
  inside the region being measured, which is the same trade M9 recorded
  for the regions themselves and is worth restating where the figure
  being read is one per cent of another figure.
- *Reviewed, and the first version of this answer was arithmetic
  between two things that do not belong together.* It set the mean
  switch against the worst region and reported the ratio as one per
  cent. The mean switch is sixty-six ticks and the mean region is
  fifty-four: the switch is *larger* than a typical region, and the
  only reason the ratio looked small is that the numerator was a mean
  and the denominator a maximum. The figure is now taken inside the
  binding region itself, for that one instance, which is the
  measurement the claim always needed. It comes out at one per cent, so
  the conclusion stands and the way it was reached did not.
  Incidentally the same instrument shows the idle path's region is
  seventy-nine per cent switch, which is what a region that is almost
  nothing but a switch looks like and is a useful thing to have seen.

**5. Whether the kernel lock puts it back at Tier 2.** *Built, and it
does not.*

- *The mechanism first, because it settles the question before the
  measurement does.* `klock_drop` and `klock_regain` touch the spinlock
  and nothing else. The lock is put down across the switch, as M8
  arranged, and interrupts are not: they were turned off before it was
  taken and stay off until somebody restores. So the lock's presence
  does not change the interrupts-off structure at all, and there is
  nothing for it to put back.
- *What it does change, which is worth stating.* At Tier 2 a user
  thread's region is opened by `klock_enter` at the trap, so by the
  time `channel_send` disables interrupts they are already off and its
  own restore does nothing. **The splits of stages two and three are
  no-ops for user threads at Tier 2.** They take effect only for
  threads that entered the kernel with interrupts on, which means the
  kernel's own.
- *And the figure is the same at both tiers anyway*, which says the
  splits were not what set it. Five runs of each, sorted, in ticks:

```
riscv64 tier0   4852 4878 5221 5544 6986
riscv64 tier2   4920 5044 5108 5259 8819
aarch64 tier0  29263 29804 32330 39147 40923
aarch64 tier2  32701 34189 35285 36799 43708
```

- The medians are 5221 against 5108 on riscv64 and 32330 against 35285
  on aarch64: within nine per cent, in both directions. In
  microseconds every configuration lands between five hundred and ten
  and five hundred and seventy. Two ports and two tiers agreeing to
  that degree is the strongest evidence this milestone has that the
  figure is a property of the kernel's shape.
- *The absence is the finding, and it was worth asking for.* M8's
  deviation raised the count of interrupts-off regions from six hundred
  to twenty-two thousand and left the longest one untouched, which M9
  measured. This stage is the same result from the other end: the lock
  does not lengthen the binding region either, because what the lock
  contends for and what the region protects are different things.
- *Where the binding region is.* `channel_send` in ten runs out of
  eleven, at both tiers, with one run finding `thread_cancel` at a
  similar length. So it is not one path but a family of them, all of
  the same size, which is what a floor set by the shape of a rendezvous
  rather than by one code path would look like.
- *No code changed in this stage.* It is a measurement and a reading of
  two functions, and the check is the comparison itself.
- *Reviewed, and the figure this milestone has been reporting is not
  the one that matters.*

  The stage inferred that at Tier 2 a user thread's region opens at the
  lock and not where the splits are, and did not check it. Recording
  which kind of thread was current when the binding region opened
  settles it: **a kernel thread, in every run at both tiers.** The
  inference holds.

  But the same record answers a question the milestone had not thought
  to ask. Beside the binding region, the longest crossing region that
  began while a thread other than the kernel's own was current:

```
riscv64 tier0   kernel 503 us   with a program current  96 us
riscv64 tier2   kernel 456 us   with a program current  83 us
aarch64 tier0   kernel 450 us   with a program current  90 us
aarch64 tier2   kernel 547 us   with a program current  56 us
```

- **Four to ten times shorter.** The figure section 13.3 has carried
  since M9, and every figure this milestone has published, belongs to
  the kernel's own threads doing rendezvous work in the checks. What a
  program is actually delayed by is between fifty-six and ninety-six
  microseconds.
- *Which thread was current, and not which code did the work.* A
  handler runs in the context of whatever it interrupted, so a region a
  timer opens while a program runs counts in the second column. That is
  the right question for latency, because it is that program the region
  delays, and the distinction is worth stating because the first
  reading of these figures had it as "what a program's own path costs",
  which is a different thing.
- *This is for stage six.* Section 13.3 is about what bounds the
  latency a program sees. The number it should carry is the second
  column, and the first is the latency of a test harness.

**6. What the floor is now, and section 13.3 rewritten around it.**
*Built.*

- Section 13.3 has a new part, written after the measurements rather
  than before them, and the old table now points at it. What it says is
  what the five stages found, in the order they found it.
- *The figures it carries.* Medians of five runs each, in
  microseconds:

```
                              t0 riscv  t0 aarch  t2 riscv  t2 aarch
longest across a switch,
  kernel thread                    449       559       465       530
longest with a program current      83       115        60        70
```

- **The second row is the one the section is about.** It is about the
  latency a program sees, and the first row is the latency of a test
  harness: the longest region that spans a switch is opened by a kernel
  thread in every run at both tiers, doing rendezvous work in the
  checks. Every figure this section has carried since M9 was the first
  row.
- *And the account of the floor changes for the second time.* M9
  corrected the section from "preemption point placement" to "the
  switch". M12 corrects it again: the region spans a switch, which is
  still true and still means no preemption point can break it, but its
  length is the rendezvous and not the switch, which is about one per
  cent of it. Lowering it further means making the rendezvous shorter
  or making less of it atomic.
- *What is asserted and what is reported.* The durations are reported,
  as this plan's rule has required since M9. Two things are asserted:
  that a region spanning a switch was measured at all, and that a
  switch was. Removing either fails the run with its own message. The
  instrument's blind spot is reported and not asserted, because there
  is nothing sensible to assert about it: the count of regions replaced
  before they closed is about twenty-six a run and moves, so requiring
  a number would be asserting a threshold on something that is not a
  property of the kernel. The entry said three and there are two.
- *If the floor did not move, the section says so.* It did not move
  much: three splits, all argued from the state, none of them shifting
  the figure by more than the run-to-run spread. That is the result,
  and the section says it rather than presenting three changes as an
  improvement.
- *Reviewed.* Two corrections, both to what the stage said rather than
  to what it measured.

  It claimed three things were asserted and there are two. The report
  panics if no region spanning a switch was measured and if no switch
  was; the instrument's blind spot, the regions replaced before they
  closed, is reported and nothing checks it. There is nothing sensible
  to check: the count is about twenty-six a run and moves, so requiring
  a number would be asserting a threshold on something that is not a
  property of the kernel.

  And the switch-inside-the-region figures in section 13.3 were four
  single runs presented as the measurement, which is the mistake stage
  four's review had already caught once in a different form. Twenty
  runs, five in each configuration, put it between zero and three per
  cent, so the claim holds and is now stated as a range. The same
  twenty confirm on both ports what six runs had confirmed on one: the
  binding region is opened by a kernel thread every time.

*Done when* the longest interrupts-disabled region is the switch and
nothing else, or the milestone can say what else is in it and why it
cannot be taken out.

*Would falsify.* Section 13.3's account of what bounds latency, for the
second time. M9 corrected it from "preemption point placement" to "the
switch"; if the switch turns out not to be the floor either, the
section's per-tier argument needs a third account of what sets it.

*Watch for.* Splitting a region and measuring the split rather than the
kernel. Every one of these changes adds a region, so the count rises by
construction and the mean falls by construction, and neither is
evidence of anything. The worst case and its site are the figures that
mean something, and M9 caught five measurements of the wrong thing in
six stages.

*And the other one.* Interrupts enabled where they were off is a
correctness change wearing a measurement's clothes. Every split has to
be argued from what the state being manipulated is, not from what the
figure does afterwards.

*Size.* Medium. The changes are small and the arguments for why each is
safe are not.

---

### M13. Giving Memory Back

**A process that exits returns what it was given.**

**Done.** All six stages are built. Section 16 says what it answered,
what it cost, what the building kept finding, and what it left
untested.

Section 1 has said since M9 that memory is never reclaimed, and every
retrospective since has repeated it without anyone acting on it. What
that means precisely: the frame allocator has a working `page_free`
that nothing calls except a boot check, so a loaded program's image
pages and, at Tier 2, its page tables are taken at spawn and held for
ever. There is an `md_as_create` and no `md_as_destroy`.

**So how long this system can run is bounded by how many processes it
has ever started**, which is a limit the shell reaches by being used.
Everything else with a lifetime already returns: pool slots go back on
reap, handles close, connections are released, and the reference counts
M11 fixed are what decides when. Frames are the exception.

*Needs.* Nothing new. The bitmap allocator frees, the pools free, the
reference counting that says when an object is finished exists, and the
report already prints how many frames are in use.

**What this is not.** Not swap, not eviction, not a page cache. Section
15 rules those out for reasons that have not changed: without dirty
tracking the kernel cannot know whether a writable page needs writing
back. This is the narrow thing, which is that what was taken for a
process is given back when the process is gone.

**1. What a process holds, counted before anything is freed.** *Built.*

- Frames in use were reported and never attributed. Which of them
  belonged to a process, which to its page tables, and which to the
  kernel was not recorded anywhere.
- *Check.* Spawning a process and reaping it, with the frame count read
  either side, says how many frames a process costs and how many come
  back. The second number is zero today, and the check asserts the
  first: a count of frames is a property of the image and the tier, not
  of the host, so it is the same in every run.
- *Settled.* **A process costs 264 frames on riscv64 and 262 on
  aarch64, at Tier 2, and none of them come back.** That is about a
  megabyte for a program whose image is four kilobytes, and the reason
  is in `proc_load`: the loader allocates and maps the whole slot,
  because the archive carries the size of the image and not the size of
  the memory the program needs.
- *Where they go*, measured by probing the spawn rather than inferred
  from the total, which is how the first version of this entry got it
  slightly wrong. **The load costs 257 on both architectures**, being
  the slot's 256 pages and one leaf table built on the way. **The
  address space costs the rest**, 7 on riscv64 and 5 on aarch64, which
  is `md_as_create`'s root table plus what `map_kernel_into` builds.
  Nothing else in a spawn costs a frame: `proc_create`, the command
  line and `thread_create` are all pools and all free. Stage two turned
  out to recover 256 of these and not 257: the leaf table is reached
  through the address space and goes back with it, which leaves stage
  three 8 frames on riscv64 and 6 on aarch64 rather than 7 and 5.
- *And at Tier 0 a process costs nothing at all.* A flat-memory image
  is copied to the address it was linked for and runs in memory the
  archive already occupies, so there is no per-process allocation and
  nothing about a process to reclaim. **Every frame this milestone can
  give back is a Tier 2 frame**, which stages two and three now say
  rather than assume. The check asserts the two answers separately: at
  Tier 0 that the cost is exactly zero, at Tier 2 that it is at least
  the slot. A floor rather than a figure, because the figure is
  per-architecture and the floor is the claim being made.
- *Measured twice.* `page_used()` counts the whole system, so anything
  that allocated inside the window would be attributed to the spawn,
  and at Tier 2 the other core is running while this measures. The
  check spawns and reaps twice and asserts the two costs agree. That is
  not proof of no interference, but it is the difference between a
  number that was measured and a number that was read once, and it puts
  M9's rule to the test inside a single boot: a count is asserted
  because it is the same in every run.
- *Broken to check it.* Raising the floor above what the loader spends
  panicked with the real count. Reading the count before the spawn
  instead of after panicked at zero, which is the failure the check
  exists to catch, since a measurement taken at the wrong moment reads
  like a system that costs nothing. Making a flat spawn allocate one
  frame panicked on the Tier 0 arm. Making the second spawn of the same
  program cost one frame more panicked with "cost 264 then 265".
  Allocating a frame between the exit and the reap panicked rather than
  reporting that four billion frames came back, which is what
  subtracting unsigned counts the other way round would have printed.
  Reading the before-count after the spawn panicked on the count
  falling.

**2. A reaped process gives its image pages back.** *Built.*

- The pages `proc_spawn_image` took, freed when the last thing that
  could ask about the process lets go, which is where M11 put the
  decision.
  Stage one counted them: 257 of the 264 a process costs, being the
  slot and one leaf table. The slot is the 256 this stage returns; the
  table is reached through the address space and is stage three's.
- *Check.* The count after a reap returns to the count before the
  spawn. Spawning and reaping the same program many times leaves the
  count where it started rather than climbing, which is the difference
  between reclaiming and merely freeing once.
  Stage one made this a Tier 2 check: at Tier 0 the count never moved
  in the first place, and the arm to assert there is that it still does
  not.
- *Settled.* **256 of the 264 frames come back, and eight processes one
  after another hold 64 frames rather than 2112.** The 257th, the leaf
  table the load built on its way, is reached through the address space
  and goes back with it in stage three, so the residue per process is 8
  on riscv64 and 6 on aarch64. `proc_free_slot` walks the slot, and for
  each page that is mapped it unmaps first and frees second, because a
  frame back in the allocator while an entry still reaches it belongs to
  two owners at once.
- *The join is not the reap*, which was the first thing this stage had
  to find out and which stage one had measured across without knowing.
  Waiting on the completion timeline says the process exited, and
  closing its handles says nothing can ask about it any more, but its
  last thread is *parked* at that point rather than collected:
  `thread_reap_parked` runs at the top of the next switch and
  `proc_reap` runs from there. The count read straight after a join is
  read before any freeing has happened, which is why stage one saw a
  second spawn appear to cost 8 frames. The check now waits for
  `pool_lookup` on the process identifier to go stale, which is the reap
  exactly, and stage one's numbers are the numbers after that wait.
- *A place can be reused before its occupant is reaped*, which is the
  defect this stage found and the one that mattered. `cohort_place`
  gives away the place of a member that has *exited*, not one that has
  been reaped, and a cohort's address space and slot addresses are
  reused across generations. So a replacement member can be loaded at
  the same address in the same space while the previous occupant is
  still waiting to be reaped, and that reap then unmapped and freed the
  replacement's memory. It showed up as the replacement faulting on its
  own first instruction. `proc_free_slot` now asks `cohort_holds` first
  and frees nothing when the place has moved on. The frames of the
  member that lost its place are orphaned by the load that mapped over
  them, and stay lost: section 18.2 is where a place becomes something a
  reap gives back rather than something an exit does.
- *The pager boundary, which stage four is what tested.* A process with
  a pager is given its image pages from a grant on a fault, at an
  address the pager did not choose, and those frames belong to the
  pager's grant rather than to the process. `p_pager_bytes` records
  where `proc_load` stopped allocating so the reap can start there.
  Removing it broke nothing any check could see when this stage was
  written: the frames go back to the allocator while a live grant still
  names them, and nothing looked. `page_free` catches a double free but
  a grant is never freed, so there was nothing to collide with. The
  guard was argued from `pager_map_page` and left standing on the
  argument. Stage four is what demonstrates it: with the boundary
  removed, the frames the pager supplied stop being held, and its table
  says so.
- *Not solved: no shootdown.* `md_tlb_flush_all` is local to the core
  that calls it. A cohort peer running on another core shares the space
  a member's slot was in and can hold a translation to a page just
  freed. Section 13.3 has no cross-core TLB maintenance and this does
  not add one. What it means is that reclaiming a cohort member's
  memory while a peer runs is not safe, and the checks reap processes
  that are in no cohort.
- *Broken to check it.* Freeing nothing at the reap panicked with "0 of
  264 came back". Freeing all but one page panicked with 255. Not
  waiting for the reap panicked with "cost 264 then 8". Dropping the
  `cohort_holds` test reproduced the replacement faulting at its entry
  point.
- *And the check itself was broken first.* The run of eight processes
  measures the residue and then asserts the residue, which cannot fail
  on the thing it is for: with the freeing removed it measured a
  residue of 264, found 264 of them eight times, and passed. What
  catches that is asserting what came back against the slot, which is a
  number the system does not get to choose.
- *And the review found the allocator had no lock.* Every other shared
  structure in the kernel takes one at Tier 2: the pools, the object
  references, the timer list, the console. The frame allocator took
  none, and disabling interrupts is exclusion on one core and nothing on
  two. It had not mattered while the only caller that freed a frame was
  a boot check, and this stage is what made freeing something a reap
  does, at the top of whichever core's switch collects the parked
  thread. `page_alloc`, `page_take` and `page_free` now take a leaf
  spinlock, on the pool's pattern.
- *Measured, though, only one core ever enters it*: 43849 operations on
  riscv64 and 43706 on aarch64, all of them on the core running the boot
  thread, at two cores and at four. So `page_report_cores` prints the
  split and, unlike `pool_report_cores`, does not halt a boot in which a
  core never appears, because that would halt every boot there is. The
  lock is still right: a fault serviced by a pager, or a spawn made by a
  program, is this allocator entered from wherever that program runs.
  **It is correct and untested by contention**, and printing the zero is
  what keeps that visible.
- *Two smaller things the review changed.* `proc_as` answers with the
  kernel's own space for a process that has none, and the slot addresses
  are mapped there too, so a process reaching the free without a space
  would have had it free the frames behind the kernel's own mappings.
  Nothing can reach that today; it is refused rather than assumed,
  because the failure is silent and total. And the count after the spawn
  was read after the child had been let go rather than before, which a
  program that allocates nothing made look right either way.
- *Broken to check those.* Clearing the address space root before the
  free panicked with "its memory is in a space it does not have".
  Removing the frame lock changed nothing that runs today, which is the
  same answer the per-core count gives and the reason it is printed.


**3. And its page tables, at Tier 2.** *Built.*

- An `md_as_destroy` on each port, walking what `md_as_create` and the
  mapping calls built.
  Stages one and two counted these: the 8 frames on riscv64 and 6 on
  aarch64 that a process still holds after its slot has gone back,
  being the root table, what `map_kernel_into` builds on top of it, and
  the one leaf table the load built to reach the slot.
- *Check.* The same as stage two with the tier that has tables, and a
  count that returns rather than climbs. A table freed while still
  reachable is the failure this must not have, so the check spawns
  again afterwards and reads the same file, which would fault on a
  table that had been handed back and reused.
- *Settled.* **Everything a process costs now comes back: 264 of 264 on
  riscv64, 262 of 262 on aarch64, and eight processes one after another
  hold nothing at all.** That is the sentence section 1 has carried
  since M9 stopped being true for the ordinary case. By the end of the
  boot the machine has taken 10746 frames at its peak and holds 10218,
  where before the two numbers were the same.
- *What is freed is tables and only tables.* Every leaf under a root
  points at memory somebody else owns: the process's slot, which stage
  two returns before this runs, or the kernel's own image and devices,
  which `map_kernel_into` wrote into every space and which freeing
  would take out from under the kernel. The walk asks at every level,
  because the identity map is made of gigapages and megapages and a
  leaf at the top is as real as one at the bottom. On aarch64 it also
  has to know which level it is on: that format gives a table and a
  page the same two bits, and 0x3 means a table at levels one and two
  and a page at level three.
- *The check now reads a file afterwards*, which is why it moved to
  where a connection to the file server exists. It reclaims, then
  spawns a program into the frames it has just returned and takes it
  across the trap edge and through a server in another address space. A
  table freed while something could still reach it is a frame handed to
  the next caller with a live mapping into it, and this is what would
  find that.
- *And the assertion is zero, not the residue.* Stage two's run
  measured the residue and then checked against it, which cannot fail
  on the thing it is for. Since this stage there is no residue, so what
  is asserted is that a run of processes holds nothing and that every
  frame a process cost came back, both of which are numbers the system
  does not get to choose.
- *A cohort's space goes back when its last member does*, which needed
  the decision to be read off the array of members rather than off
  `c_placed`. The two disagree in exactly the case stage two found: a
  place handed on before its previous holder was reaped is one
  increment and no decrement, so a count would never reach zero.
  `c_placed` is reset with the space, which keeps it meaning what
  `proc_load` reads it for.
- *Not exercised: that last part.* No check in this boot reaps every
  member of a cohort, because the members' handles are held until the
  machine stops, so the path that destroys a cohort's space never runs.
  Removing it changes not one frame of what the boot reports, which is
  how that was established rather than assumed. It is kept because the
  alternative is a class of address space that leaks for ever, and it is
  written down here because nothing tests it.
- *And the review found what freeing a root costs.* A root is a frame,
  and since this stage a frame goes back and comes out again, so the
  same root address can name a different address space. The context
  switch decided whether to activate by comparing that address against
  the one this core already had, which before this stage was a sound
  identity: a root was allocated once and never freed. Now it is not.
  What the mistake would cost is not a wrong root register, which would
  still point at the right table, but the flush that goes with writing
  one: skip the activation and the core keeps translations for the space
  that used to be there, at the same addresses, pointing at frames since
  given to somebody else. The allocator hands out the lowest free frame
  and the checks spawn the same program eleven times over, so this is
  the likely case rather than a remote one.
- *`vm_as_epoch` is the fix*, a count of address spaces destroyed, read
  by the switch beside the root and cached per core. Any destroy makes
  every core activate once more, at its next switch, which is the first
  moment it could use a translation anyway. A counter rather than a
  message to each core, because section 13.3 has no cross-core call and
  this milestone did not add one. **It costs two extra activations
  across a whole boot**, 472 against 470, which is also the measurement
  that says it does something.
- *A probe was written first to see whether it could happen today*: a
  switch that skipped activation while the space was a different object
  with the same root. It did not fire, because the boot thread is a
  kernel thread and every switch through it activates the kernel's own
  space and resets what the core has cached. That is what stands between
  this system and the fault, and it is not a property anything asserts.
- *Broken to check the rest.* Never destroying the space panicked with
  "256 of the 264 a process cost came back". Destroying it before
  freeing the slot panicked on the guard the previous review added, "its
  memory is in a space it does not have", because the slot walk then had
  no space to walk. Following a leaf as if it were a table panicked in
  `page_free` on both ports, at 0xc000000 on riscv64 and 0x9000000 on
  aarch64, which are the interrupt controller and the console: the walk
  had reached the identity-mapped devices, which is exactly the memory
  it must never follow.

**4. What holds a frame that no process holds.** *Built.*

- Buffers lent across a rendezvous, grants mapped into another address
  space, and anything a pager supplied. Each has an owner that is not
  the process whose frame it is.
- *Check.* The count returns after a run that used each of them. A
  frame still held is a number that says so, and the stage names what
  holds it rather than freeing it and finding out.
- *Settled.* Of the three the plan named, **one holds nothing and two
  hold what they should**, and the boot now prints the table:

  | what | riscv64 | aarch64 | held by |
  | --- | --- | --- | --- |
  | a page given away as a grant | 1 | 1 | the grant |
  | pages a pager supplied | 5 | 6 | the pager's grants |
  | a buffer lent across a rendezvous | 0 | 0 | nothing |
  | a client that ran and was reaped | 0 | 0 | nothing |

- *A grant holds the frame its creator allocated.* `grant_create` is
  handed a physical address and does not own it: the caller found the
  memory and the grant describes it, so a grant's teardown has nothing
  to give back. A device grant is the case that makes that right, its
  memory being a chip and not a frame at all. What would change it is an
  owning grant, which section 6 does not have and this milestone does
  not add.
- *A buffer lent across a rendezvous holds nothing*, which the plan did
  not expect. It is the client's own memory mapped into the server for
  the length of the call, so there is no frame to hold, and the tables
  that describe it where the server sees it were built by the first such
  call and are reused by every one after.
- *And the numbers are read from the run rather than written down.* What
  the pager holds is asserted against the pages the pager supplied,
  which is why 5 and 6 are both right: the `paged` image is a page
  larger on one port. A literal there would have been a check on the
  build rather than on the mechanism, and would have failed on the
  architecture it was not written for.
- *And the review found the number was read too early.* Stage two
  established that the join is not the reap, and these brackets read the
  count the moment a check returned, with the process it spawned still
  holding its slot until the next switch collected its last thread. The
  figures were right anyway, because each of these checks does enough
  after its join for the reap to happen, which is luck and not
  structure. The failure it would produce is a panic naming the wrong
  culprit: a whole process counted against the grant.
- *The first fix was the wrong observable.* Waiting for the process
  pool's count to return to what it was before the check assumes the
  check is the only thing making processes. Under the paced session it
  is not: the console has input to deal with while this runs, a process
  appears that the check never started, and the wait failed on somebody
  else's work with "1 processes outlived the check that made them". It
  passed every burst run, which is what made it look right.
- *What it waits on now is the figure itself.* `frame_holder` reads the
  count, yields, and reads again until what is held is what is expected
  or a deadline passes. That needs no theory about which processes are
  whose: the thing being asserted is the thing being waited for, and a
  number that never arrives still panics with what it found.
- *This is also where stage two's pager boundary stopped being an
  argument.* That stage kept the reap off pages a pager supplied and
  recorded that removing the guard broke nothing any check could see.
  It breaks this one: freeing them panics with "pages a pager supplied
  left 0 held, not 5". Giving a second page away panics with "a page
  given away as a grant left 2 held, not 1", which is what says the
  grant figure is measured and not a constant that happens to be one.

**5. Whether the system can now run indefinitely.** *Built.*

- *Check.* A loop that spawns, runs and reaps a program many more times
  than there are frames, with the count read at the start and the end.
  Before this milestone that loop cannot run at all; the number of
  iterations it survives is the milestone's headline figure, and it is
  a count.
- *Settled.* **A program was spawned and reaped 40000 times, on a
  machine with 32768 frames, and the count came back to where it
  started.** On riscv64 that run asked the allocator for 10,560,000
  frames and took 430 seconds; on aarch64, 10,480,000 frames and 413
  seconds. Either way it is more than three hundred times what the
  machine has. Before this milestone the same loop stopped at about the
  ninety-fifth iteration with no memory left.
- *The boot runs 200 of them*, not 40000, because seven minutes is not
  a check anyone runs. Two hundred cycles still ask for 52800 frames
  against the 32768 that exist, so the boot check is a real one and the
  headline came from a run made on purpose. Which figure came from
  where is written here because the two are not the same claim.
- *The program is `brief`, which does nothing at all.* Every other
  program in the archive computes something, talks to a server or
  faults on purpose, and the cheapest of them spends eighty
  milliseconds doing it, which multiplied by the number of frames in
  the machine is a check nobody would run. What a cycle of `brief`
  costs is what a spawn and a reap cost, which is what this stage
  measures.
- *The duration is reported and not asserted*, per M9's rule, which M12
  had to relearn: how long a run of spawns takes is a property of the
  host. What is asserted is the count of cycles, the frames each moves,
  and that the count at the end equals the count at the start.
- *And a stretch of the boot goes quiet.* `exit_quiet` turns off the
  line every exiting program prints. For the rest of the boot an exit
  is something a check is watching for; here the two hundredth exit
  says nothing the first did not, and the check's output would be its
  own noise.
- *The count at the end is waited for, not snapped*, which the review
  changed for the reason stage four's review settled on. `page_used()`
  is the whole machine's count, and asserting it is exactly what it was
  asserts that nothing else moved a frame in the instant this looked.
  The last cycle's reap is the one thing certain to be in flight, since
  the join is not the reap. A leak still fails, because a count that
  never returns runs out the deadline and panics with what it found.
- *And the review found something it did not fix.* Running both ports'
  long tests at once, on one host, made an aarch64 boot panic with "cpu
  1: started and never answered". The rendezvous that waits for a
  secondary core spins a million times and then gives up, and a million
  spins is not an amount of time: it is however long a million spins
  take on a host that may be running something else. That is M9's rule
  about counts and durations pointing at the boot, where a count is
  being used to bound a wait. It is written down here and nowhere else
  yet, because it is not this milestone's subject and the fix is a
  change to how the machine starts.
- *Broken to check it.* Keeping one frame back on every sixteenth reap
  panicked in the stage-two check, "8 processes came and went and held
  1 frames", which is the earlier check doing its job. Making it every
  hundredth reap got past every check before this one and panicked
  here: "200 cycles left the count at 10732, not 10730". That is the
  failure this stage exists for, and a reclamation that returned all
  but one frame each time is exactly the thing no shorter run can see.
- *And it had to move to the end of the boot.* Run before the shell it
  delays it by two seconds, and the paced session feeds the shell a
  line every two seconds from the moment the machine starts, so the
  transcript lost the echo of the first command typed. A check that
  changes what it is checking is worse than no check; nothing runs
  after this now but the reports.

**6. What is still held, and section 15 written around it.** *Built.*

- *Check.* The figures, asserted as counts, and the sections that
  describe reclamation matched to what was built. What is deliberately
  not reclaimed is named with the reason, so that the next reader does
  not have to rediscover which absences are decisions.
- *Settled.* The boot ends with a ledger, and **it balances to zero in
  all four configurations**: every frame the machine has spent has a
  name. On riscv64 at Tier 2, of 10732 frames still held:

  | | frames |
  | --- | --- |
  | spent before anything ran | 7836 |
  | held by processes still running, ten of them | 2352 |
  | held by cohort address spaces | 22 |
  | held by grants over ordinary memory | 7 |
  | orphaned when a cohort place moved on | 512 |
  | added to the kernel's own address space | 3 |
  | **held by nothing that says so** | **0** |

- *A ledger is a different claim from a total.* It is weaker than
  "nothing leaks", because most of those rows are frames nothing will
  ever give back. It is much stronger than a number, because it fails
  when a new way of holding a frame appears and nobody writes it down.
  That is what the last row asserts.
- *Finding the rows was the work.* The remainder started at 516 and was
  taken apart from there. **512 of it is the orphaning stage two found**
  and could only describe: a cohort place handed on before its holder
  was reaped, twice over in this boot, at a slot's 256 frames each. They
  cannot be counted by walking, because the load that placed the
  replacement overwrote the only entries that named them, so the reap
  counts the event instead. **Three more are the kernel's own address
  space**, which grows after boot when a grant or a buffer refines a
  mapping the identity map made out of megapages. **The last one was a
  second grant**, in `namespace_checks`, which had been invisible
  because stage four measured the grant it knew about rather than
  grants as a kind.
- *So the grant row counts grants*, not the two the checks happened to
  bracket. It walks the grant pool and sums what the non-device ones
  describe, which also absorbed the pager row: pages a pager supplied
  *are* its grants' frames, and two rows for one thing was double
  counting waiting to happen.
- *Section 15.5 is the writing.* It says when a process gives its memory
  back and why that is the reap rather than the exit, that the join is
  not the reap and a count read between them has a whole process in it,
  and that freeing a root makes a root address ambiguous. Then it names
  the five things deliberately not reclaimed, each with the reason: what
  the port spent before anything ran, memory a grant describes, pages a
  pager supplied, writable pages by eviction, and a cohort member's
  frames when its place moved on. Section 18.2 gained the paragraph that
  says what a cohort restart costs, and section 15.2's teardown
  paragraph now says which part of it is the loader service that does
  not exist yet and which part is the kernel doing it today.
- *Broken to check it.* Taking one frame and naming it to nobody
  panicked with "1 held by nothing that says so". Counting device grants
  instead of ordinary ones panicked with 6. Not counting the orphaned
  frames panicked with 512. Each is the ledger doing the only thing it
  is for.
- *And one that was not a break.* Tier 0 kept reporting the old figures
  through several rounds of this, because `proc_frames_orphaned` had
  been written inside the Tier 2 guard: the link failed, the build
  carried on with the previous kernel, and the run said what it had said
  before. A build error read as a stale result for longer than it should
  have.

*Done when* a program can be run more times than the machine has
frames, and the count comes back to where it started.

*Would falsify.* Nothing in the design, which is the unusual thing
about this milestone: section 15 already says memory is reclaimed when
its owner is gone, and the kernel has simply never done it. What could
falsify is the assumption that ownership is already known well enough
to free on: if the reference counting cannot say when a frame is
finished with, then something about how frames are owned has to change
rather than something about when they are freed.

*Watch for.* Freeing a frame that something still reaches. Every other
milestone's mistakes have been visible in a figure; this one's would be
a program reading another program's memory, which no figure shows and
which the checks have to be built to catch on purpose.

*Size.* Medium. The freeing is small and knowing what may be freed is
not.

---

### M14. What an Image Actually Needs

**The archive carries a layout, and the loader maps what a program has
rather than a slot.**

**Done.** All six stages are built. Section 17 says what it answered,
what it cost, what the building kept finding, and what it left
untested.

M13 measured what a process costs and found the answer was a megabyte:
`proc_load` allocates and maps the whole slot, one region, writable and
executable, because the archive carries the size of an image and not
the size of the memory a program needs. That single gap costs three
separate things.

**It costs memory.** Measured against the images that exist, a program
needs seven to eleven pages of the two hundred and fifty-six it is
given: one or two of text and rodata, five of bss and stack. The
smallest program in the archive is handed thirty-six times what it
uses.

**It costs protection.** One region means one set of permissions, and
the one that lets a program run is read, write and execute. Every
program's text is writable and every program's stack is executable, on
the tier whose whole purpose is that the hardware enforces boundaries.
`proc_load` says so in as many words and names the archive as the
reason.

**And it costs section 15.1.** That section has writable segments
copied eagerly and read-only ones supplied on demand, which the loader
cannot obey while it cannot see where text ends. The pager path skips
image pages wholesale and the comment there says a program with
initialised writable data would break it. No program has any, so
nothing has.

*The boundary already exists.* The user linker script page-aligns the
start of writable memory and says why: "so that no page of this image
is both text and data", and "a boundary the loader can see". It is in
the link and it stops there. What this milestone does is carry it four
steps further: into the archive, into the loader, into the mapping, and
into what a fault means.

**Tier 0 gets the archive change and none of the rest**, the same
division M13 found. There is no MMU, so there are no permissions to
set and no per-process memory to save: a flat image runs in place in
memory the archive already occupies. Every claim below about cost and
protection is a Tier 2 claim, and the entry says which are which rather
than discovering it at stage one again.

*Needs.* Nothing new in the kernel. The linker script has the
boundary, `md_map` takes permissions and is already called per page,
the archive record has room to grow, and M13's ledger and endurance
loop are the checks that will say whether the smaller mapping still
adds up.

**What this is not.** Not ELF loading: the archive stays a table of
flat images at fixed addresses, and section 15.2 is still where a
loader starts placing them. Not a variable slot: the address stride
stays a megabyte, because slots are assigned by link address and
changing that is a different milestone. What changes is how much of a
slot is ever backed by a frame.

**1. The archive carries a layout, not a size.** *Built.*

- The linker script names the boundary it already creates and the end
  of what a program needs. The tool records both. The kernel reads them
  back.
- *Check.* The boot prints, for every member, what it needs against
  what a slot is, and the figures agree with the link map to the page.
  A member whose recorded boundary is not page-aligned, or lies outside
  its own image, is refused at load rather than mapped wrong.
- *Settled.* Every image in the archive now says what it is made of,
  and **the answer is six to eleven pages of the two hundred and
  fifty-six a slot is**: `brief` 1+5, `fs_server` 2+5, `console` 2+6,
  `threads` 1+7, `paged` 5+5, where the pair is read-only pages plus
  writable ones. The figures were checked against `llvm-nm` on the
  linked image for seven programs and agree exactly.
- *How the numbers travel.* `_rw_start` and `_mem_end` are symbols the
  linker script defines at boundaries it was already creating. The link
  rule writes them beside the binary as offsets from the load address,
  and `mkbootarch` reads that file rather than taking the numbers as
  arguments: a tool told the layout on a command line would let the
  archive and the link disagree with nothing to notice. The record grew
  two fields and the format went to version 2.
- *And the first thing it found is a program with initialised data.*
  `peer_b` has four bytes of `.data`, which had gone unnoticed because
  nothing had ever needed to know. It matters twice over. The image's
  bytes cross the boundary, so `bm_size` is not where text ends and the
  tool's first check said it was, which building it corrected. And
  stage five is now about a case that exists rather than one to be
  invented: `proc_load`'s comment says a program with initialised
  writable data would break its pager path, and one is in the archive.
- *And the review found the check ran too late to mean what it said.*
  It was called from the archive report, which `user_slice` runs long
  after the first program has loaded, so a bad layout would have been
  caught after a dozen programs had already been mapped by it. The
  entry's own words, "refused at load", were not what was built. Every
  member's layout is now checked once, the first time anything asks the
  archive anything, which is before the first load by construction: with
  a bad layout the boot now stops with no program having run at all.
- *And the archive did not depend on the layouts.* `boot.img` listed the
  binaries and not the files beside them, so an archive older than a
  layout would carry numbers that were no longer true. Building this
  stage hit that twice and both times it read as a result rather than as
  a stale build, which is the same mistake M13 stage six made with a
  link error. The two lists are now one variable and its derivative, so
  a third copy cannot drift.
- *Broken to check it.* Shifting the recorded boundary eight bytes off
  a page made the tool refuse the build, naming the image. Deleting a
  layout file and asking the tool directly made it refuse rather than
  assume. Removing the tool's check and writing a layout whose end is
  at its beginning got past the build and was caught by the kernel at
  boot, which is the arm that exists because a build can half change.

**2. A process is mapped as what it needs.** *Built.*

- `proc_load` maps text and rodata, then writable memory, and nothing
  beyond. The slot stays a megabyte of address space and stops being a
  megabyte of frames.
- *Check.* A process costs what its image needs and not 264 frames.
  M13's ledger still balances to zero and its endurance loop still
  returns the count, which is what says the smaller mapping did not
  simply lose the accounting.
- *Settled.* **A process costs 15 frames on riscv64 and 14 on aarch64,
  where M13 measured 264 and 262**, and all of it still comes back. For
  `spin` that is six pages of image and stack, one for the startup
  record, seven of address space and one leaf table. The reduction is
  seventeen-fold and it is the whole of what the archive now carrying a
  layout buys.
- *One of those frames is the record's*, still in the last page of the
  slot where nothing else is mapped any more, so the loader allocates
  one for it rather than reusing a page the loop had already mapped.
  Stage four is where it moves inside what the program has and that
  frame stops existing.
- *And the cohort loss shrank with it.* The frames a restart orphans are
  the frames the previous member had, which was a slot and is now what
  its image needed: the ledger's row went from **512 to 14** on riscv64
  and 16 on aarch64. The count had to move to the loader to stay right,
  since a member whose place has moved on has had its mappings written
  over and the number is no longer recoverable from the tables. It is
  recorded as the loader maps, which is the one place it is known.
- *The endurance loop needed more cycles to make the same claim.* Its
  point is that a program runs more times than the machine has frames,
  and at fifteen frames a cycle that takes 2400 rather than 200: 36000
  frames against the 32768 that exist. A cycle is cheaper than it was,
  3.8 milliseconds against 11, but 2400 of them add seven seconds to
  every boot. That is the price of keeping the claim in the boot rather
  than in a run made by hand.
- *M13's own assertion had to change*, because it said a process costs
  at least a slot and that is exactly what stopped being true. It now
  reads the floor out of the archive: at least what this image says it
  needs. The same check asking the same question of a loader that knows
  the answer.
- *And the review found the rows that expect nothing could not fail.*
  Two of the four measure a window and expect it to hold nothing, and
  the difference was clamped at zero before being compared: anything
  else giving memory back inside the window drives it negative, the
  clamp reads that as nothing held, and a leak of the check's own would
  sit hidden under it. Both are signed and exact now. Freeing one frame
  inside the window panics with "left -1 held, not 0" where it used to
  pass. The rows measured against a holder were made signed for the same
  reason, though nothing destroys a grant today, so that one is a
  refusal to wrap rather than a fault found.
- *Broken to check it.* Mapping one page fewer than the image needs
  broke a program's stack and the boot failed, though at a later check
  and with a message that names something else: the loader does not
  notice, and what noticed was a program behaving oddly. Mapping the
  whole slot again panicked with "264 of the 265 a process cost came
  back". Leaving the orphan count at a slot's worth panicked in the
  ledger with "the parts come to more than the whole".

**3. Text is not writable and the stack is not executable.** *Built.*

- Two mappings with the permissions each region should have.
- *Check.* A program that writes its own text faults, with the kind and
  the address that say so; a program that jumps into its stack faults.
  Both are new programs, because no existing one can do either. Putting
  the permission back makes each succeed, which is the break test and
  also the proof that the check was testing the mapping.
- *Settled.* At Tier 2 a program that writes its own text is refused
  with an access fault at exactly its load address, and one that jumps
  into its stack is refused with an access fault inside its own writable
  region. **Text is read and executed, everything else is read and
  written, and the comment on `VM_PROGRAM` that named this as what the
  archive would buy can be deleted.**
- *`proc_prot` is where the decision lives*, rather than two constants
  at the mapping site, because the loader is not the only thing that
  maps a program's page. A buffer lent across a rendezvous is unmapped
  from the client, mapped into the server, and mapped back, and "back"
  used to mean `VM_PROGRAM` because every page was the same. Asking
  where the page is gives the same answer every time it is asked.
- *The startup record is read-only*, which it can be: section 20.4 has
  the loader place it in the child's memory, and the loader writes it
  through the physical address before any of this is mapped, so nothing
  in the program ever needs to write it.
- *Two programs had to exist.* `poke_addr` writes to an address it is
  handed, which is `touch_addr`'s other half and was missing because
  reading is all any program here did to an address; a mapping that
  permits everything and one that permits reading answer a read the
  same way. `leap` jumps into its own stack, which nothing could do,
  because every other program jumps only where the linker sent it.
- *The address is asserted and not only the fault*, which matters more
  here than usual: an unmapped page and a page that refuses a write are
  both `SYS_FAULT_ACCESS`, so a check that asked only whether something
  faulted would pass against a loader that had mapped nothing at all.
- *And Tier 0 says the other thing.* Flat memory permits the write, as
  section 13.3 says it must. It permits the jump too, and what ends that
  program is the zeroes it jumped to failing to decode, which is
  `SYS_FAULT_INSTRUCTION` and not `SYS_FAULT_ACCESS`. The check asserts
  that distinction rather than accepting any fault, because the two
  kinds are how a reader tells "the machine let me run from my stack"
  from "the machine stopped me", and only the second is a protection.
  The first version of this check expected no fault at all at Tier 0 and
  was wrong about its own tier.
- *And the stage passed its own check by doing nothing, once.*
  `proc_prot` asks `proc_slot` where the process's memory is, and
  `proc_slot` answers from `p_entry`, which the loader set *after* the
  mapping loop. So every page was mapped while the process had no slot
  yet, `proc_prot` fell back to permitting everything, and both new
  programs ran to completion. The loader now sets the entry and the two
  boundaries before it maps anything.
- *And the review found the tightening did not survive a lent buffer.*
  A buffer sent to a server is unmapped from the client, mapped into the
  server, and mapped back when the reply comes, which is the one path
  where a program's own page is re-mapped by something that is not the
  loader. The take-back asked `proc_prot` what the page should be and
  asked it of the **borrower**, which does not own that address, and the
  answer for an address a process does not own is everything. Measured
  at the moment of the take-back: the borrower answers 0xf and the owner
  answers 0xb. Every buffer a program lent came back executable, on
  exactly the pages a program writes to.
- *`leap` learned to lend first*, because nothing else would have found
  it. With an argument it reads a file into its buffer and then jumps
  into that buffer, so the page it executes is one that has been out to
  a server and back. Asking the borrower again panics with "ran from a
  buffer it had lent and lived", and so does restoring `VM_PROGRAM`
  outright, which is what the code said before this milestone.
- *It runs last in the slice, which is part of the check.* Placed before
  the frame measurements, its sixteen frames came back in the middle of
  one and the row after it read minus sixteen. Closing the process
  handle does not reap it either: something still holds a reference at
  that point and releases it later, on work the boot thread does rather
  than on a yield, so a loop waiting there waits for ever. What holds it
  is a question for a stage about references and is written down here
  rather than chased.

- *Broken to check it.* Giving text write permission back panicked with
  "a program wrote its own text and lived, kind 0". Giving writable
  memory execute back panicked with "ran from its stack and lived, kind
  4", the kind being the illegal instruction it then hit, which is the
  same answer Tier 0 gives and the reason that kind is spelled out
  there.

**4. The startup record moves inside what the program has.** *Built.*

- Section 20.4's record lives in the last page of the slot today,
  chosen because it is the one place a program's own layout does not
  reach. Nothing will map that page any more.
- *Check.* A program still finds its namespace and its arguments, and
  the page the record is in is inside the region the archive says the
  program needs. A record placed outside it faults on first use rather
  than being read from memory that happens to still be there.
- *Settled.* The linker script reserves a page above the stack and the
  record goes there, which is **the last page of what the program
  needs**, so the loader knows where it is without being told: `bm_mem`
  minus a page. Above the stack rather than below, because the stack
  grows down and away from it. The boot prints where it lands, seven
  pages into eight for `poke_addr` on both ports.
- *It costs nothing.* Stage two had to allocate a frame for the record
  at the slot's end, because the loop that maps a program stopped before
  it. The page is inside that loop's range now, so the frame it maps is
  the record's and a process still costs 15 frames on riscv64 and 14 on
  aarch64.
- *And the record is read-only*, which the last stage made possible and
  this one puts inside a writable region, so `proc_prot` has to name it
  before the range that contains it. `poke_addr` pointed at a program's
  own record faults at exactly that address.
- *The loader checks it can be reached.* The record is the one thing a
  program is handed by address rather than in a register, so a record
  placed where nothing is mapped is a program that faults on its first
  instruction with nothing saying why. The loader now looks it up in the
  space it just built and refuses rather than starting the program.
- *And the review found the check could pass with the record
  misplaced.* Poking the address the record should be at proves that
  page is read-only, and `proc_prot` makes the last page of a program's
  memory read-only wherever the record actually went. So a loader that
  placed it somewhere else would leave that page read-only, the poke
  would still fault, and the row would read as though nothing were
  wrong. The check now compares the address the loader chose against
  the one the archive implies, which is what makes the fault mean what
  it says. Expecting the record a page lower panics with "the loader
  put the record at 0x81f07000, not 0x81f06000".
- *That comparison is Tier 2 only*, because at Tier 0 the record is not
  in the program's memory at all: there is one address space, and the
  record lives in the process object where a program can read it
  because nothing stops it. The same division as everywhere else in
  this milestone, found by the check panicking on a tier it was not
  written for, which is the second time that has happened here.
- *Moving the record a page lower in the loader is caught*, but by a
  check four earlier: the record lands on the stack, a program's frame
  is overwritten, and something unrelated dies first. The address
  comparison is what would catch a misplacement that corrupts nothing,
  and that is the case it exists for.
- *Broken to check it.* Leaving the record at the slot's end, where it
  was before this milestone, panicked with "its startup record at
  0x805ff000 is not mapped", naming the image. Without that guard the
  same break took the boot down four checks later with a message about
  something else, which is why the guard is there. Leaving the record's
  page writable panicked with "a program wrote the record it was handed
  and lived".

**5. A program with initialised writable data.** *Built.*

- Stage one found that `peer_b` has four bytes of it, so this is a case
  that exists rather than one to be invented. Every other program has
  an empty `.data`, and the eager-copy half of section 15.1 has never
  run on any of them.
- *Check.* A program with initialised data reads its own values and
  writes them, and the paged program's text arrives from its pager
  while its data does not. That is the case `proc_load` says it cannot
  handle, so the check is whether the comment can be deleted.
- *Settled.* The comment is gone, and what replaced it is the
  condition. `proc_load` left every page with bytes in it to the pager,
  so a paged program with initialised data would have had that data
  arrive read-only, because section 15.1 maps a page supplied to a
  fault read-only. The condition is now "has bytes **and is below where
  writable memory begins**", which is the boundary stage one taught the
  archive to carry.
- *The plain half was already true and already checked.* `peer_b` holds
  a word in `.data`, tells its peer where it is, and both verify what is
  there before and after. The loader copies the image's bytes wherever
  they land, so that has worked all along; what had never happened is
  the same program under a pager.
- *So `paged` now carries a word of `.data`*, reads it before it
  checksums itself and writes it afterwards, returning a distinct status
  for each failure. Its text still arrives a page at a time from its
  pager, and the check asserts from the outside what the program
  asserts from the inside: **the pager was asked for exactly the pages
  below the boundary and for none above it**, five on riscv64 and six on
  aarch64.
- *And the review found the check counted pages without looking at
  which.* `pager_pages` says how many arrived, so a pager that supplied
  a data page while some text page went untouched would give the same
  number and pass. The pager now records the end of the highest page it
  was asked for, and the check compares that against the boundary: asked
  as high as 0x5000 against a boundary of 0x5000 is the whole claim in
  one comparison. Narrowing the expectation by a page panics with the
  real figure, which is how a check with nothing to catch is shown to be
  reading anything at all.
- *That assertion does not catch the loader break*, because restoring
  the old condition kills the program before the pager is ever asked
  high: the checksum assertion fires first. The two cover different
  failures, and the entry says which so that neither is mistaken for the
  other.
- *Broken to check it.* Restoring the old condition reproduced the
  failure the comment predicted: a page fault at 0x81205008, eight bytes
  into the data page, after one page had arrived. The program never
  returned a status at all, which is why the check asserts the pager's
  count as well as the program's answer.
- *And the frame checks stopped using `spin`.* They spawn a program
  eleven times to measure what a spawn costs, and `spin` computes for
  eighty milliseconds, which put most of a second between the last check
  and the shell. The paced session feeds a line every two seconds from
  the moment the machine starts, so the shell was reading its first
  command out of a buffer instead of echoing it, and the transcript lost
  a line on aarch64. `brief` does nothing, which is what these checks
  want measured. The shell is back to two seconds from three.

**6. What it cost, and section 15 written around it.** *Built.*

- *Check.* The figures, asserted as counts, in all four configurations,
  and sections 15.1, 15.2 and 24.4 matched to what the archive now
  carries. What a slot is for, once it is not what a process costs, is
  named rather than left as a number nobody revisits.
- *Settled.* **A process costs 15 frames on riscv64 and 14 on aarch64**,
  where M13 measured 264 and 262, and the boot now prints the figure
  broken in two: seven of the fifteen are what the image needs and eight
  are the space it runs in, and on aarch64 eight and six. The two halves
  answer to different things, which is why they are printed apart: the
  first changes when a program does, the second when a port's table
  shape does.
- *The figure is asserted twice, by two measurements that share
  nothing.* What the allocator says a spawn took is a difference of
  frame counts; what the process is holding is a walk of its own address
  space. A loader that allocated a frame it did not map, or mapped a
  page twice, moves one and not the other. Allocating one frame more
  than it maps panics with "brief took 16 frames and is holding 15";
  leaving the address space out of the walk panics with "took 15 frames
  and is holding 7".
- *What the machine holds at the end fell with it*, from 10732 frames to
  8519, of which the processes still running account for 119 across ten
  of them where they used to account for 2352. The archive itself grew,
  because this milestone added three programs, so what was spent before
  anything ran went from 7836 to 8354. The ledger still balances to
  zero.
- *And the cohort loss fell out of it.* Section 18.2's restart orphans
  what the previous member was mapped, which was a slot and is now an
  image: **512 frames across a boot became 14.**
- *Section 15.5 is new* and says what a program is made of: the two
  offsets, what each region permits, that the bytes may cross the first
  boundary, and that both numbers come from the link rather than from
  the command line that assembles the archive. Section 15.1 gained the
  reason a loader has to be told which segments are writable, and that
  the pager supplying only read-only pages is a property of what it is
  asked for as much as of what it answers. Section 24.4 records the
  layout and the format version. Section 18.2's cost is now the image's
  pages rather than a slot's.
- *And what a slot is for is written down.* It is an address range and
  not an amount of memory: what keeps two programs from overlapping, and
  what one may grow into. Making it smaller would save nothing, because
  nothing is spent on the part that is not mapped, and what would change
  it is a loader that relocates, which section 15.2 already names.

*Done when* a program is mapped as text that cannot be written and data
that cannot be executed, costs what it needs, and the frame ledger
still balances.

*Would falsify.* That the boundary in the link is the boundary the
loader needs. It separates read-only from writable, which is what
permissions want; if what the loader turns out to need is a third
region, or a boundary the linker cannot express for a flat binary, then
the archive is carrying the wrong thing and this becomes ELF loading,
which is a different milestone with a different cost.

*Watch for.* A check that passes because a fault happened for the wrong
reason. Three of the six stages assert that something faults, and an
unmapped address and a permission violation are both faults: the kind
and the address have to be asserted, not the fact.

*Size.* Medium. The mapping is small, the build plumbing that carries
two numbers from the linker to the kernel is most of it, and the
programs that prove the permissions do not exist yet.

---

### M15. A Deadline on a Rendezvous

**A client that sends to a server which stops answering gets its thread
back.**

Section 1 has said since M9 that a rendezvous carries no deadline, and
it is the last of the absences this project named more than once and did
not take. What it means is that a wedged server hangs its client for
ever: cancellation exists and something outside has to notice and use
it, and nothing notices on its own.

*The design already says all of it.* Section 14.3 has every blocking
call take an absolute deadline and names both sentinels. Section 12.1
goes further and specifies the two cases: a SEND-blocked client is
removed from the queue and the server never learns it existed, and a
REPLY-blocked client abandons work in progress, so the server's reply
returns `SYS_ERR_STALE` and releases whatever it committed on that path.
`sys_msg_send()` is documented as taking a deadline. It does not have
one.

*And most of the mechanism is built.* `THREAD_WAIT_SEND` and
`THREAD_WAIT_REPLY` are already distinct states, because section 22.2
needs to report them apart. `timeline_wait` already takes an absolute
deadline and arms a one-shot timer for it.
`channel_thread_release` already undonates a lent priority, takes back a
buffer, clears the message state and tells the server, because M8 needed
all of that for cancellation and death. What is missing is the deadline
itself, the timer for a blocked sender, and the answer to what happens
when the reply and the deadline arrive together.

**What this is not.** Not a deadline on receive: a server waiting for
work is not waiting for a party that can fail it, and section 12.1 says
so. Not a watchdog, and not restart policy: what happens after a client
is released is section 18.3's and stays there.

**1. A send carries a deadline.** *Built.*

- The argument gains an absolute deadline, and the kernel reads it.
- *Check.* A send with a deadline already past, to a channel nobody is
  receiving on, returns `SYS_ERR_TIMEOUT` without blocking, which is
  what section 14.3 says `SYS_DEADLINE_NOW` means. Every existing
  client keeps working unchanged, which is most of the boot.
- *Settled, and the trap decided.* The deadline is read only when the
  entry carries `SYS_OP_DEADLINE`. Without the flag a send waits for
  ever, which is what every client written before this did and still
  does. **A field read unconditionally would have turned every one of
  them into a poll**, because `SYS_DEADLINE_NOW` is zero and zero is
  what `memset` leaves in a structure a program has not thought about.
  Section 14.3 fixes both sentinels' values, so no choice of constant
  removes the ambiguity; what removes it is the caller saying whether
  it filled the field in.
- *The check is the same send twice*, with the same zeroed deadline.
  With the flag and nobody receiving, it returns at once. Without the
  flag it waits, and a server started below it arrives to prove that it
  did: had the field been read anyway, that send would have returned
  `SYS_ERR_TIMEOUT` before the server ever ran.
- *The poll is asked where the thread would queue*, not at the top of
  the function. A send that finds a server already waiting completes
  without blocking, and a deadline has nothing to say about it. It is
  also asked before the priority chain is raised, so there is nothing
  to undo.
- *And the review found the flag meant nothing on anything else.* It
  could be set on any operation and every one but a send ignored it,
  which is a caller saying something the operation does not do. Section
  17.4's result register is refused the same way and for the same
  reason, so a deadline on anything but a send is now
  `SYS_ERR_NOT_SUPPORTED`. It is a list of one operation rather than an
  assumption, so that the day a receive gains a deadline this is where
  it is said.
- *The check was asking the wrong path, twice.* The kernel's own helper
  calls an operation directly and skips the static checks a program's
  entry gets, so a refusal written into `entry_static_check` was
  invisible to it: the first version of this check read OK where the
  kernel had refused nothing. Both halves now go through the submission
  path. With only the first converted, refusing the flag on a send as
  well passed unnoticed, because the send that was meant to catch it was
  still going the other way.

- *Broken to check it.* Reading the deadline whether or not the flag is
  there took the boot down at the first client that clears its
  arguments, which is the trap this stage exists to avoid, reported as
  "client: send failed". Ignoring the flag, and refusing to poll at
  all, both stop the boot at this check with the connection made and no
  answer: a send with no deadline is a thread that does not come back,
  so the failure is a boot that stops rather than a panic that names
  it.

**2. A SEND-blocked client gives up, and the server never learns.**
*Built.*

- A timer for a queued sender, and expiry that takes it off the queue.
- *Check.* A client sends with a deadline to a channel no server ever
  receives on. It returns `SYS_ERR_TIMEOUT`. A server that receives
  afterwards finds nothing, the channel's queue is empty, and no
  per-connection state was ever allocated: section 12.1 calls this case
  clean and the check is what says it is.
- *Settled, and it needed three lines.* `time_sleep_until` after the
  sender queues, `time_sleep_cancel` after it wakes, and the comment
  that said a sender carries no deadline. Everything else was already
  there: `thread_wake_timeout` calls the thread's unwait hook, which for
  a channel takes it off the sender list and clears what it was blocked
  on, and then sets `SYS_ERR_TIMEOUT` and readies it. M8 built that for
  cancellation and it fits a deadline exactly.
- *The wait is asserted and not only the answer.* A deadline already
  past returns the same error without any waiting having happened, so
  the check reads the clock either side and requires that at least the
  asked-for interval elapsed. How much longer than that is the host's,
  and is printed rather than asserted.
- *The queue is read directly rather than by having a server look.* A
  receive carries no deadline, so a server sent to find nothing would be
  a thread that never came back. What section 12.1 claims is that
  nothing was left behind, and an empty sender list is that claim.
- *A send with a deadline it did not need* is the fourth case, and it is
  the one that makes the cancel matter. Every other send here either
  polls or gives up, so none of them leaves a timer armed for a thread
  that goes on to be served. Without that case, removing the cancel
  changed nothing any check could see; with it, the boot no longer
  finishes.
- *And the review found a priority the raiser leaves behind.* A sender
  that queues calls `chain_raise`, which lifts every blocked server down
  the chain to the sender's priority. Nothing records who raised whom,
  so when that sender gives up there is nothing to take back: the boost
  is withdrawn later, when each of those servers' own calls complete and
  their own donors undonate them. It is bounded rather than leaked, and
  it is still the thing `channel_thread_release` refuses to allow one
  level in, where it says in as many words that a server serving a
  client that has been cancelled has no reason to keep running at that
  client's priority. The same argument reaches one level further out and
  the mechanism does not.
- *Not fixed, and not reached.* Lowering a chain safely needs a record
  of what this sender raised, because a boost may be owed to somebody
  else by the time it is given up, and that record is section 12.3's
  mechanism rather than this stage's. No check here reaches it either:
  the channel these checks use has nobody inside a request, so
  `chain_raise` returns before raising anything.
- *And a REPLY-blocked client's deadline is still ignored*, which is
  stage three and is said here so that the flag is not read as working
  everywhere it is accepted. A send that finds a server already waiting
  arms no timer. The stage's own new case goes through the queue on both
  ports, which the cancel break confirms by failing on both.
- *Broken to check it.* Leaving the client on the queue when the timer
  fires panicked with "a client that gave up is still queued". Never
  cancelling a served sender's deadline stopped the boot part way
  through the shell session, which is the stale timer firing against a
  thread doing something else. Never arming the timer hangs at this
  check, for the reason stage one's breaks hang: a send with no deadline
  is a thread that does not come back.

**3. A REPLY-blocked client gives up, and the reply says so.** *Built.*

- Expiry for a client whose message a server is holding, and a reply
  path that discovers it.
- *Check.* A server receives a message and does not reply until after
  the client's deadline. The client returns `SYS_ERR_TIMEOUT`; the
  server's reply returns `SYS_ERR_STALE`; the server's per-connection
  record is released on that path and the count says so.
- *Settled.* The timer is armed for both states now rather than only for
  a queued sender, and `channel_unwait_thread` is where the two part
  company: a queued sender comes off the queue, and a client whose
  message a server holds keeps nothing that would let a reply land.
  `channel_reply` already answers `SYS_ERR_STALE` to a client with no
  reply slot, so the server's discovery needed no new path.
- *The heavy work cannot happen in the timer.* The first version called
  `channel_thread_release` from the expiry, which undonates and takes
  the buffer back, and both charge their work to `thread_current()`.
  In a timer interrupt that is whatever the core was doing and may be
  nothing at all: it wrote to offset 0x150 of a null pointer, which is
  where `t_charging` lives, on three of the four configurations and
  showed up as an unbalanced charge on the fourth. The interrupt now
  does only the pointer work that makes a late reply stale, and the
  client does the rest when it runs again, where there is a thread to
  charge it to and the client is the thread doing it.
- *The check was testing stage two twice.* Its server was created below
  the boot thread, so it had not run when the send arrived: the send
  queued, the timer was armed on the way through `THREAD_WAIT_SEND`, and
  the client only became REPLY-blocked afterwards. Arming the timer for
  queued senders alone, which is stage two's code, passed it. With the
  server above the boot thread it is already in receive when the send
  arrives, and that break now fails with "expected TIMEOUT, got OK".
- *And it could not spin.* The boot thread outranks a server at
  `PRIO_BOOT - 1`, so waiting for the reply with `sched_yield` never let
  the server run at all. It waits on a timeline the server signals.
- *And the review found a server left inside a request it had finished.*
  `channel_reply` answers `SYS_ERR_STALE` and returns before it reaches
  the point where a reply takes the server off the channel's list of
  threads inside a request. Nothing else takes it off. That list is what
  `chain_raise` walks to donate a queued sender's priority down the
  chain, so a server left on it is a thread raised for work it is no
  longer doing, and the walk then follows what it is blocked on into
  somebody else's chain. Every timed-out request would have added one.
- *Reachable only since this stage.* A stale reply used to mean a double
  reply or a client that had been cancelled, and cancellation ends the
  server's serving state by another route. A client that gives up on its
  own is the first ordinary way to make a server's reply arrive too
  late.
- *Cleared only when the client named is the one this server was
  holding.* A reply to an identifier that was never its own is refused
  the same way and has nothing to do with the request it does have, so
  it must not take that request off the list. Leaving the state set
  panics with "the channel still has a server in a request".

- *Broken to check it.* Arming the timer only for a queued sender
  panicked with "expected TIMEOUT, got OK". Skipping the release the
  woken client does panicked with "it gave up still donating to its
  server", which is the assertion that had to be added when the first
  version of that break passed: this send lends no buffer, so what the
  release still has to undo here is the donation.
- *Not tested here: clearing the reply slot in the interrupt.* Removing
  it changes nothing, because the client clears the same fields itself
  when it runs, long before this server replies. What it is for is a
  reply that arrives in the window between the timer firing and the
  client running, which is stage five.

**4. The buffer a client lent.** *Built.*

- A REPLY-blocked client has its memory mapped in the server. Giving up
  has to take it back, and M14 made a page's permissions part of what
  taking it back means.
- *Check.* After a timeout the client can read and write its buffer
  again, with the permissions it had and not more, and the server
  writing into what it still thinks is the client's buffer does not
  reach the client's memory.
- *Settled.* The take-back was already in stage three, because the panic
  that says a thread ran again with its buffer still lent out would have
  fired otherwise. What this stage adds is the check, and the check
  needed a **user program**: a kernel thread shares the kernel's address
  space, so there is nothing to take away from it, and what this stage
  is about exists only between two address spaces. `patient` lends a
  page, gives up, then writes it and reads it back. If the page came
  back read-only, which is what asking the wrong process for its
  permissions would do, it faults there instead of returning.
- *A program cannot make a deadline.* Section 14.3 has every blocking
  call take an absolute one, and section 6.2 names what makes that
  affordable: a shared page a program reads without a syscall. That page
  does not exist, so a program has no way to read the clock at all. The
  kernel computes the instant and passes it as an argument, the way
  `paged` is passed the bounds of its own image, and the entry says so
  because it is an absence rather than a decision.
- *And the timeout put preemption points where an assertion says there
  are none.* Section 13.3's second term is the time from a device
  interrupt to the thread waiting on it, and the boot asserts that no
  such dispatch passes a preemption point. A client that gave up has a
  page to unmap and map on its way out, which is two page-table charges
  and two points, and `patient` arrived with exactly those two. The
  stamp is not taken for a thread woken by its own deadline: the timer
  is what tells the time, not what it was waiting for, and the same
  function already declines to measure kernel threads for the same kind
  of reason. **The assertion is right about the path it is named for**,
  so what changed is which dispatches are that path.
- *Not checked: that the server cannot reach it.* The holder here is a
  kernel thread, and the kernel can reach any memory whatever its page
  tables say, so nothing it did afterwards would prove anything. What is
  checked is the client's half, which is where the permissions live.
- *Broken to check it.* Not taking the buffer back panicked with
  "patient ran again with its buffer still lent out", which is the
  sentence M13 left in that path for whenever a deadline arrived.

**5. A deadline that expires while the reply is in flight.** *Planned.*

- The race the other stages assume away: the server replies at the
  moment the timer fires.
- *Check.* Exactly one of the two outcomes happens. A client that was
  given `SYS_ERR_TIMEOUT` does not also receive a reply, and a client
  that received a reply is not also failed. Constructed rather than
  waited for: the check arranges the two to coincide rather than hoping
  they do.

**6. What it cost, and section 12.1 written around it.** *Planned.*

- *Check.* The figures, asserted as counts, and section 12.1 matched to
  what was built. What a deadline does not do is named: no deadline on
  receive, nothing about restart, and the rule that a server which must
  not have its work discarded should not be given a deadline to discard
  it.

*Done when* a client sending to a server that never replies is released
at its deadline, the server learns when it replies, and neither the
client's memory nor the server's records are left in the other's hands.

*Would falsify.* That the two blocked states can be released the same
way. Section 12.1 says they differ and the difference is the server's
view; if releasing them turns out to need one path and not two, then
12.1 is describing a distinction the implementation does not have and
the section is wrong rather than the code.

*Watch for.* A check that passes because the timeout never happened. A
deadline that is never reached and an operation that succeeds look
identical from the outside, so every check here has to assert that the
wait actually expired and not only that the call returned.

*Size.* Medium. The release path exists, the timers exist, and the two
sentinels are already in the interface. The race in stage five is the
part with no precedent.

---

## 4. Milestone 1, in Retrospect

The slice was chosen as the smallest thing that could falsify the design
rather than the largest that could demonstrate it. This is what it
falsified, what it confirmed, and what it cost.

### 4.1 The Four Questions

**Is send, receive and reply with donation as small as assumed?** Yes.
Message passing is about three hundred lines and donation about ten of
them. The receive queue being ordered by sender priority and a donated
thread changing run queue were both cheap, because the scheduler already
had exactly one place that decides what runs.

**Do timelines earn a separate object?** Yes, but not as the *single*
object section 9 claimed. Interrupts, timer activation, one-shot
completion and wait deadlines all landed on the one counter, and timer
overrun detection fell out of it rather than being designed. IPC did not:
a thread waiting for a reply is not waiting for a counter, so a channel
became a second thing to block on. The invariant that mattered survived,
because it was always section 5's "at most one object" rather than section
9's "one kind of object". Section 9 is corrected.

**Is handle-relative naming with no ambient root workable?** Yes, and
easier to build than to describe. A client handed one connection has
nothing to name and nothing to guess, and the server needs no notion of a
root at all. The client in the slice holds exactly one handle and can
reach exactly one directory, which took no enforcement machinery: it is
what having one handle means.

**Does a protocol with the offset in the message survive a real client?**
Yes. It removed state rather than adding a parameter.

### 4.2 What It Cost

About 5400 lines of kernel, plus 1000 lines of the checks that exercise
it. Two architectures from one source, with output identical apart from
the banner.

Kernel object memory is 319936 bytes, and the shape of that number is more
useful than the number. Thread stacks are 274432 of it and process handle
tables 34560, so 96% is two per-instance arrays sized by two constants.
Every channel, timeline, timer and connection in the system together comes
to about nine kilobytes. Section 8 claims kernel memory is a property of
the configuration rather than the workload, and this is that claim holding
in a way that also says the defaults are wrong: sixty-four threads at four
kilobytes each is not a Tier 0 configuration.

### 4.3 What Is Still Untested

Worth stating because the milestone is easy to mistake for more than it
was. Nothing here has been measured. No user code has run, so the trap
edge is still unexercised and the operation entry point has only ever been
called from inside the kernel. Tier 0 does not enforce the memory
contract, so section 6.1 is unexercised and so is every fault path.
Cohorts, multicore, namespaces, the submission engine and the whole of the
MMU remain as they were.

Two constants describe mechanisms that are not there. `SYS_PRIO_CHAIN_MAX`
and `SYS_ERR_DEADLOCK` presuppose a donation walk that turned out to be
unnecessary for transitivity and is therefore absent, so a cycle of
servers would deadlock undetected. Section 12.3 now says so.

### 4.4 What the Building Kept Finding

Ten of the twenty-one commits fix work committed minutes earlier. That
ratio is the useful output of the milestone, and the faults fell into
recognisable kinds.

**Obligations the design implies without stating.** Waking a thread has to
take it off whatever it was blocked on. Releasing several waiters has to
be atomic against preemption. Every change to what is pending has to
recompute what the timer is armed for. An interrupt handler has to finish
with its device before it advances anything. A core that is idle must have
no current thread. None of these appear in the sections that require them,
and each was found by something breaking.

**Asymmetry between two paths to the same state.** The signal path
unlinked a waiter and the timeout path did not. The scheduler owned thread
states and one caller set them anyway. Wherever two mechanisms could end
the same wait, the second one written was the one that forgot something.

**Tests that could not fail.** Four stages running shipped a check that
passed while testing less than it claimed, and in three of them repairing
the test immediately exposed a real fault in the code it was meant to
cover. This was the highest-yield defect finder in the milestone, ahead of
either reading the code or reviewing it.

---

## 5. Milestone 2, in Retrospect

M2 asked whether the interface survives a real protection boundary, and
answered it by putting one there. This is what it settled, what it cost,
and what it left alone.

### 5.1 The Three Questions

**Does the operation descriptor survive crossing a real boundary?** Yes,
unchanged, and nothing about it wanted a different shape. The trap path
shifts no registers on either port, because the descriptor pointer and the
count are already where the C calling convention puts them. What the
question cannot reach on this tier is the part that matters most: section
23.2's address check has nothing to consult without an address space, so
what M2 produced is a statement of what is trusted rather than a check.
Section 23.2's real test is M4.

**Is the rendezvous of section 12 what a server actually wants**, now that
a server is a program rather than a function the test calls? Yes, with no
change to it. The loop a program writes is the loop section 12.6
describes, and the cookie carried the server's per-connection state
without the kernel learning anything.

**Does the compound request of section 25.7 pay?** Yes, and not for the
reason the section leads with. Three rendezvous become one, which was
expected. The connection pool goes from one allocation to none, which is
the structural saving, because section 25.2 makes that pool a build-time
bound on every file open at once and a client reading a small file end to
end has no use for a durable reference to it. A third saving turned up
that the section does not mention: three messages give a busy server two
chances to be elsewhere when the next one arrives, and the compound cannot
queue at all, because the client has given the server nothing else to do.

### 5.2 What It Cost

About 2900 lines added across sixty-four files, of which 530 are the three
user programs and the rest is kernel, libc, the boot archive tool and the
build changes to carry an archive.

Kernel object memory went from 319936 bytes to 321728, and the shape is
unchanged: thread stacks and process handle tables are still 96% of it.
The startup record and the trace ring together are under a kilobyte. M2
added a boot archive, a loader, three programs, a tracer and a debug
console without moving the number that section 8 says is a property of the
configuration.

### 5.3 What the Building Kept Finding

**The specification was wrong in three places, and building it is what
found them.** Section 25.7 put an entry index in the field a read needs
for its file position, so a compound read at a non-zero offset could not
be written at all; it also had a close entry that does nothing, because
the server releases an intermediate node when it replies whether or not
anything asks. Section 6.1's reply-path caveat and section 25.5 both
assumed a grant object for bulk payloads, which is sized for a case in
section 18.4 that nothing before M6 needs. None of the three was visible
by reading.

**An estimate was wrong in a way worth recording.** The scheduler defect
was written down as needing a change with deferral nesting thought
through, and it needed neither: `sched_ready` does two things and the
caller wanted one. The error was reading `sched_defer` as the only way to
make two scheduler operations atomic rather than asking whether the second
was needed at all. An estimate that argues for deferring work should be
checked before it is acted on.

**Tests that can fail found everything else.** Every claim in this
milestone was confirmed by breaking it: entering at EL1h to prove the
programs are at EL0, demanding the wrong `mcause` to prove they are in
U-mode, changing the server's string to prove the bytes running are the
bytes built, walking a name that does not exist to prove a compound stops,
overrunning a compound's payload to prove it is refused before it runs,
swapping the client order to prove the pool measurement is not an
artifact, reverting the scheduler fix to prove the new check catches it,
and compiling the debug console out to prove the system does not need it.

**Two of those found real defects rather than confirming the expected.**
Raising the server above its client to test the tracer corrupted the run
queues, which is a legitimate configuration nothing else in the system
reaches, since donation raises a server to exactly its client's priority
and equal priority does not preempt. And the pool measurement would have
reported zero connections for whichever client ran second whether or not
it allocated one, because `highwater` is monotonic; the order swap is what
turned that from a passing test into a correct one.

### 5.4 What Is Still Untested

Nothing has been measured. Tier 0 enforces nothing, so section 6.1 is
still a convention and every fault path is unexercised. The trap edge now
runs, but only the system-call half of it: no program has faulted, because
a fault on this tier is a kernel bug rather than a message to a pager.

The identifier split for 32-bit targets compiles and round-trips and has
never run. The buffer field of section 12.4 is rewritten by a function
that is the identity, so the half of it that matters arrives with the MMU.
Cohorts, multicore, namespaces, the submission engine and death remain as
they were.

**One thing M2 built is permanent, and was nearly written down as
temporary.** The debug console of section 22.4 is ambient authority and a
deliberate exception to the capability model, and the first draft of this
section had M3's console service delete it. That was wrong, and the
correction is worth keeping because the reasoning generalises.

A driver here is an ordinary user process. On a board nobody has run this
system on, that process is the least able thing in the system to report
what is happening: the console service it would report through needs a
working UART driver, which is the thing being brought up. Every serious
embedded kernel carries a polled debug UART for this reason, and leaving
it out does not remove it, it makes each porter write one locally against
a board that does not work yet.

So it stays, gated rather than deleted: compiled out or in, and where it
is in, live or dormant at run time. The second gate is the one that
matters, because a facility present only in development builds is missing
exactly when a production fault needs it, and then the image being
debugged is not the image that broke. A GDB stub over the same UART is
named in 22.4 as the next thing of this kind and is not built.

## 6. Milestone 3, in Retrospect

M3 asked whether the parts built so far compose into something a person
can use, and answered it with a shell. This section is what the milestone
entry points at.

### 6.1 The Two Questions

**Is one thread per event source affordable?** Yes, and the number is
small enough to state plainly. Having no wait-set cost this system one
thread, the console driver's second, at 4352 bytes of kernel object
memory and 4096 more of user stack. A system with N devices that must
stay responsive while serving pays N of them, which is linear rather than
alarming. The trigger in `PROPOSALS.md` stays untripped.

**Does an interrupt the handler cannot finish with break section 13.2?**
It does not, and the fix needed no device knowledge in the kernel: the
handler masks the source at the controller and the driver unmasks it
after draining. Section 13.2 and section 26 carry the rule now. It was
also exactly the predicted failure: the machine wedged in the handler
before printing anything.

### 6.2 What It Cost

About 2600 lines across forty files, of which the console driver, the
shell and the two utilities are half. Kernel object memory moved little,
because the milestone added threads and channels rather than pools. The
recorded session and the two ways of feeding it are perhaps the most
valuable thing built, and were the cheapest.

### 6.3 What the Building Kept Finding

Four things the design did not say, each written into the section that
should have said it: a child may not outrank its parent (11.1), one live
process per image while loading is a copy to a fixed address (15.2), what
protects state shared between a driver's two threads and when that
argument expires (19.3), and which of a reply's two payload paths a read
uses (25.5). The rule two writers to one device need is in 22.4.

The check that mattered most was the one that could fail. An interactive
session compared against a recorded transcript found a scheduler defect
that a byte count would not have: a server whose base priority exceeded
its client's corrupted the run queues.

### 6.4 What Is Still Untested

A second reader of the console, a client that dies inside a rendezvous,
and any recovery at all from a server that stops answering, which hangs
its client because a rendezvous carries no deadline. All three are M7's.

## 7. Milestone 4, in Retrospect

M4 is the milestone the design exists to be tested by. Section 2.1 claims
one interface across two protection models, and until Tier 2 ran that was
a claim about one tier, which is not a claim. This is what it settled,
what it cost, and what it left alone.

### 7.1 The Two Questions

**Does the interface have to differ between tiers?** No. The evidence is
four configurations of one source tree, two architectures by two tiers,
and a recorded shell session that is one file compared against all four.
Enforcement cost three lines in the rendezvous: an unmap when a buffer is
handed over and a lookup and a map when it comes back. No operation, no
argument structure and no error code exists at one tier and not at the
other.

That has to be stated precisely, because the interface did grow. A grant
is a new object with a new operation, faults are reported through new
fields, and two well-known namespace identifiers were added. Every one of
those is present and meaningful at both tiers: `sys_grant_map` returns an
address at Tier 0 with nothing mapped, because the grant is what makes
the permission explicit rather than what creates it; a fault kind is
reported at Tier 0 for the faults Tier 0 can take. What section 2.1
forbids is a program having to know which tier it is on, and no program
here does.

What differs is what a machine can do and what it notices. Two live
processes from one image are possible at Tier 2 and refused at Tier 0,
which is the one place a request succeeds on one tier and fails on the
other; the refusal is in the loader rather than in the interface, and the
program that provokes it takes either answer. A program that breaks the
move contract faults at Tier 2 and is believed at Tier 0. Those are the
differences the design predicted and wanted.

**Does modelling a fault as IPC from the faulting thread reuse the reply
path, the blocking invariant and donation without extending any of them?**
Yes, and this is the clearest result in the milestone. `thread_fault`
records three fields and calls `thread_exit`, because a fault is an exit
with a reason. `pager_fault` builds a message and calls `channel_send`,
because a faulting thread is an ordinary blocked sender. Nothing in
sections 10, 12 or 21 needed a case added. Priority inheritance across a
page fault was not implemented at all: the pager runs at the faulting
thread's priority because a client donates to a server, and the check
had to be written to observe a property nobody arranged.

One thing did have to be added, and it was in the ports rather than in
the design: a fault that returns. Every fault before this one ended a
thread, so neither port had a path back into the frame it had built.

### 7.2 What It Cost

Nineteen commits, sixty-seven files, about 7700 lines added and 270
removed. Roughly 1800 of those lines are in the two ports, 5800 in the
kernel and the rest in programs, the library and the build.

Kernel object memory went from 380992 bytes to 504896. Almost all of the
growth is the process and thread pools, raised because nothing is reaped
and the boot check sequence spawns thirty-two processes over a run where
two are ever alive. That is the check suite sizing the pools, and it is
said where the numbers are; M7 is what makes them a property of a
workload instead.

The window held for loaded programs grew from twelve megabytes to
eighteen, because five programs were added and every image is now padded
to a page boundary between its read-only and writable halves.

Five new programs: `trespass`, `touch_addr`, `grabber`, `granted` and
`paged`. Every one of them exists to break something on purpose.

### 7.3 What the Building Kept Finding

**Enforcement finds what trust hides, including in the enforcement.** The
clearest case is not a kernel bug at all: `grabber` passed the top of its
thread's stack where the library wanted the bottom, so the library put a
thread's stack inside a message buffer. The move took the buffer away
with the stack in it and the kernel faulted writing a status to a
descriptor that was no longer there. Nothing but enforcement would have
found it, and it had been silently writing past the end of an array.

The same shape appeared twice more. A page a pager supplied was
executable whether or not it was granted as such, which only a check that
withheld the right could show. And the RISC-V linker scripts named `.bss`
and not `.sbss`, so the linker placed the small sections as orphans:
outside the range that zeroes bss in the kernel, and in the middle of the
text in a program, where the first demand-paged run faulted on it.

**Two bugs cost most of the milestone's time, and both were about what a
register held.** The AArch64 interrupt path restored `elr` and `spsr`
into `x9` and `x10` after restoring the general registers, so interrupted
code resumed with a code address in `x9`, which is `memset`'s
destination. It was found by noticing that the faulting store address was
always exactly the program counter minus five. The RISC-V trap entry
clears `sscratch` unconditionally and the kernel return path put nothing
back, so an interrupt landing in the two-instruction window inside
`enter_user` left a thread reaching user mode with no kernel stack
parked. That one had been there since M3 and needed a loaded host and
fourteen machines at once to see.

**A program could stop the machine with a number.** The kernel
dereferences the descriptor array a program passes, the argument pointer
inside each descriptor, and the vectors and strings a spawn points at.
Section 23.2 has required checking those since M1 and nothing did. Stage
5 did not create the hole; it created the ability to see it, as a panic
naming the kernel's own instruction and a program's address.

**A refusal has to name something.** Three defects were error codes that
were true of the code path and false about the world: a page already lent
reported a bad argument, a buffer that granted no access became a page
table on one port, and `SYS_EXIT_FAULT` was documented as a value no
program returns by a milestone whose own program returns arbitrary words.

**The pmap could not refine a block, and the stage that found it fixed
the wrong half.** Mapping a grant over a page of a two-megabyte block
panicked; making the device regions the size of the devices fixed the
console and hid the rule. All of ordinary memory is mapped in blocks too,
so the next grant panicked in the same place, and so would every page a
pager supplies.

### 7.4 What Is Still Untested

**Nothing in the check suite runs under load.** All four configurations
passed on an idle machine with a fault-path bug present that a loaded
host reproduces in seconds. The suite is not the place to fix that, but a
stage that changes how often threads switch should be run under load
deliberately, and this one was only after a flake made it obvious.

**Nothing tears an address space down.** A spawn that fails after its
tables exist leaks the frames in them. A grant remembers nothing about
which process mapped it, so closing a handle puts the region back whether
or not this process ever mapped it, and two grants over overlapping
regions would undo each other. All of it is the same bookkeeping, and it
belongs with M7's reclamation.

**The move contract is enforced for a buffer and not for a grant.** A
grant crosses a boundary and stays, which is what section 6.1 says it is
for, and nothing revokes one except closing its handle.

**There is one pager and it is the kernel.** A user pager would need the
image bytes from somewhere, which is a filesystem, so the chain has never
been longer than one link. Rule 3 of section 15.4 is enforced and shown
by hand rather than by a check, because what it does is halt the machine.
`sys_mem_lock()` is not an operation: the right that would guard it
belongs to a root task that does not exist.

**`SYS_FAULT_EXTERNAL` cannot be provoked on either board**, so that row
of the mapping is written and untested. The mapping is otherwise checked
exhaustively, by asking about every encoding at boot, which is what found
two missing abort groups.

**A demand-paged program may not have initialised writable data.** The
loader pages in what is inside the image and maps everything else
eagerly, and the boundary it uses is the end of the image rather than the
end of the read-only part. The linker aligns those to the same page today
because no program here has a `.data` section. The archive carrying a
layout rather than a size is what would fix it.

## 8. Milestone 5, in Retrospect

M5 asked whether a system with no absolute paths is usable by ordinary
programs, or whether the complexity simply moves into the C library and
gets worse there. It is the second of the two provisional mechanisms, and
this is what it settled, what it cost, and what it left alone.

### 8.1 The Question

**Is a system with no absolute paths usable, or does the complexity move
into libc and get worse?** It is usable, the complexity did move, and the
number that matters is that it moved once rather than per program.

The evidence the entry asked for was the utilities. `cat` was a hundred
and twenty-two lines, of which about fifty were message building, because
the only way to name a file was to send a walk to the one directory it
had been handed. It is eighty-seven lines now and none of them are
protocol. Its binary went from 6710 bytes to 8514, so what it stopped
carrying was smaller than what the library carries for it, and the shell
grew by about a kilobyte for the same reason.

The other half of the question is what a program that never names
anything pays. `status` is 3837 bytes before and after, to the byte: the
library is an archive, and a program that opens nothing links neither the
resolver nor the file layer nor the composer. That is the whole answer to
"does it get worse there". It got bigger there, once, for the programs
that wanted it.

**So section 20 stands.** Nothing in it needed changing, and section 25
needed nothing either: a walk carries a handle and a relative name
because a message to a server cannot carry anything else, and the only
thing that changed is where the name comes from. The claim that
resolution costs no system call is a file that includes `<ns.h>` and
`<string.h>`.

**What did change is a decision that was written down against this.** The
startup record argued for a table keyed by number, because a string-keyed
one costs a name buffer per entry. That was right while an entry was a
role a parent and child agreed on and wrong for a mount table, and the
milestone re-decided it and rewrote the comment rather than leaving the
two contradicting each other.

### 8.2 What It Cost

Thirteen commits, twenty-five files, about 2100 lines added and 150
removed. Roughly 830 of those are the library, of which the resolver, the
file layer and the composer are 645; the rest is the kernel carrying
names through spawn, two new programs, and the documents.

Kernel object memory did not move, because the milestone added no pools.
What moved is the boot check sequence: 42 of 48 processes at Tier 2
against 32 before it, because six of the thirteen commits added a program
or a case that spawns one, and nothing is reaped until M7.

### 8.3 What the Building Kept Finding

**One answer standing for two questions, five times.** A path too long to
hold and a path that names nothing. An empty path and the root. A
remainder that did not fit and a caller left holding an uninitialised
buffer. A union hiding a broken half behind a missing file. A `..` on a
file answered as a refusal rather than as a connection that names no
directory. Every one of them was a place where the code was right about
what to do and wrong about what to say, and every one of them would have
sent somebody looking in the wrong place.

**Three times, the defect was the prose rather than the code.** The
roadmap claimed the shell handed its children a namespace before the
shell had been taught to name anything. It claimed `cat /dev` failed
because it was a mount point when it was not one. And it stated a
stricter rule than section 20.3's and called it the rule. The checks kept
passing throughout: what drifts is the sentence next to the check, and
nothing in the suite reads sentences.

**A new field in a structure fifteen callers build is fifteen wild reads
the day it lands.** The namespace entry's name started as a pointer,
which is what a caller would write, and the machine stopped in the boot
checks dereferencing a handle value. Putting the name inside the entry
removed the pointer, removed the obligation to check it, and made an
uninitialised entry a wrong name rather than a wild read.

**A library that chooses attenuation for its callers undoes their
policy.** The composer duplicated handles with the right to duplicate,
silently widening what the shell hands out, and the shell had been
stripping that on purpose since M3. Rights are the caller's decision.

**A check written for one stage caught a later stage's regression, once.**
The resolver's eighteen cases failed on the first run of the union work,
because taking the nth entry that matched at all had quietly turned the
longest-prefix rule into a first-match rule. That is the first time in
five milestones that an older check has caught a newer defect, and it is
an argument for checks that are tables rather than sessions.

### 8.4 What Is Still Untested

**A path with more than one component below a mount point.** The resolver
produces them and the protocol carries them; the file server has one
level and matches them against a flat table, so `a/b` fails as a name
rather than being walked. Nothing here has subdirectories yet.

**A server that resolves `..` physically.** Section 20.3 says such a
server must apply its own root check to the physical walk, and the one
here refuses every `..` instead, which is stricter and simpler and does
not exercise the rule the section actually states.

**A union whose half is broken** is reasoned and was checked by hand, by
mounting a console in one, and is not in the suite: keeping it would mean
a deliberately misconfigured namespace in the recorded session for the
rest of the run.

**Composition of a namespace holding more than one kind of handle.** A
composer hands on one set of rights, and section 7.2 refuses rather than
clamps a duplication asking for what a source lacks, so a namespace with
a grant in it is composed in two calls. The wall is met on every boot and
the second call is not written, because nothing needs it yet.

## 9. Milestone 6, in Retrospect

M6 asked whether zero mapping within a shared protection domain is worth
the shared domain. Section 18 was never marked provisional, and the M5
close called it the mechanism most likely to be deleted rather than
built, which was a guess and not a reading of the document. This is what
it measured, what the measurement turned out to be about, and why the
question it was written to ask is not yet answered.

### 9.1 The Question

**Is a shared protection domain worth what it saves?**

The measurement is the same programs and the same recorded transcript,
one line of a build-time table apart:

| | out of the cohort | in it |
|---|---|---|
| pages remapped | 42 | 12 |
| page-table entries written | 84 | 24 |
| tables built | 4 | 3 |
| single-address invalidations | 42 | 12 |
| address space switches | 395 | 358 |

**The prediction was written before the building started, and its rate
was exactly right.** It said a cohort removes four page-table entry
writes and two TLB operations per page per layer crossed. Fifteen
crossings went away and took sixty entries and thirty invalidations with
them, which is that rate to the unit.

**"And nothing else" was wrong twice.**

It also removed thirty-seven address space switches. Neither port
assigns address space identifiers, so every switch discards the whole
translation cache, and a dispatch between two members does not change
the root register and is skipped. That was not in the prediction, and it
is the more expensive of the two things.

And it removed a copy. Stage two found what the plan did not know: the
move contract cannot forward. A server holding a page on loan may not
lend it on, because section 23.2 checks a descriptor against the
sender's own image, so a read through two layers copies once. Inside a
cohort the filesystem hands the store the client's own address and the
block never passes through the filesystem at all.

**The comparison the plan set up measured nothing.** Stage four built
the fair competitor the plan named, which was invalidating the addresses
that moved rather than flushing everything, and then found the buffer
path's TLB maintenance to be redundant. Removing it entirely breaks
nothing observable on either port: there is an address space switch
between the hand-over and the receiver touching the page, and another
between the take-back and the sender touching it again, and each already
discards what would have been stale. Invalidating by address closed none
of the gap, because the gap was not there.

**And the competitor that matters was not built.** What a cohort removes
that costs anything is the whole-TLB flush on a switch between members.
The mechanism that removes the same cost without a shared fault domain
is address space identifiers, which neither port has. They would remove
the flush from all three hundred and ninety-five switches rather than
from thirty-seven, and they would keep every process apart.

### 9.2 The Answer, Such As It Is

The falsification clause said that if invalidating by address closed
most of the gap, section 18 should be deleted. It closed none of it, so
by the letter of the clause the mechanism survives. By the substance it
is not so simple, because the clause was aimed at a cost that turned out
not to be the cost.

Of the three things a cohort was measured doing:

- **Thirty-seven of three hundred and ninety-five whole-TLB flushes.**
  Address space identifiers would remove all of them and keep the
  isolation.
- **Sixty of eighty-four page-table entry writes.** Real, and stores.
  Small.
- **One copy per whole block.** Nothing else in the design removes this.

Two of the three are better served by a mechanism that costs no
isolation. The third is the only thing a cohort uniquely provides. **The
case for section 18 now rests on forwarding rather than on mapping**,
which is not the case section 18 makes for itself.

**And there is a cheaper candidate for forwarding that was not tried.**
Let a process lend on a page it currently holds on loan. That is a
change to the move contract of section 6.1 rather than a shared address
space, and it hands the block driver one page instead of the
filesystem's entire memory. It has a real difficulty, which is that the
loan becomes a chain and the reply has to unwind it, and that difficulty
is smaller than a shared fault domain. If it is sound, the cohort's last
unique benefit goes with it.

**So the milestone does not settle its own question.** Saying otherwise
would be the easy thing, and the numbers above do not support it. What
it settled is what the question should have been: not whether a shared
domain is cheaper than mapping, but whether it is the cheapest way to
forward a page without copying it, and that has to be answered against
two alternatives that do not exist yet.

Section 18 is therefore kept and marked, not deleted and not endorsed.
The next milestone that touches either alternative decides it.

### 9.3 What It Cost

Thirteen commits, thirty-four files, about 3100 lines added and 78
removed, of which 2240 are code.

The mechanism is small. The cohort object and its build-time table are
266 lines, its header 149, and the changes to the rendezvous, the
process and the thread come to 295 lines across five files. Everything
else is apparatus: a block store, three peer programs, and the
accounting that makes a boundary countable. That ratio is the honest
shape of this milestone, and it is the second time a milestone has cost
more to measure than to build.

Nothing is reaped, so the process pool went from 48 to 56 and the thread
pool from 80 to 96. The third milestone running to raise them, and the
last: M7 is next.

**The fault-domain cost is not a number, and it is the largest thing
here.** One program, one fault, two outcomes. In a cohort:

```
$ peer_c
cohort peers: member peer_c faulted, instruction at 0x0, pc 0x81800042
  the whole cohort is the recovery unit, per section 18.2: peer_a peer_b peer_c
  tearing them down together and restarting them is M7, and is not built

panic: cohort peers cannot be recovered and must not be left running
```

With one line of the membership table changed:

```
$ peer_c
sh: peer_c: instruction fault at 0x0, pc 0x81800042
[-16]
```

A filesystem and a block driver in one cohort means a bug in either
takes down the storage stack. Section 18.2 says so; this is what it
looks like.

### 9.4 What the Building Kept Finding

**A comment claiming what the code did not do, three times.** Stage one
said a TLB flush was kept because removing it broke a check, and
removing it breaks nothing. Stage three said nothing else could run
between the two halves of a charged region because neither half can
block, and the timer interrupt dispatches on its way out. Stage four
said a port with address space identifiers would not change the callers,
and it would need two invalidations per page where one does now. Every
one was a sentence that sounded like reasoning and had not been checked,
and in a document whose whole method is falsifiable claims, an unchecked
one in a comment is the same error in a smaller place.

**One value standing for two questions, and once the reverse.** A new
error code for a condition the loader already answered with
`SYS_ERR_WOULDBLOCK`. A receiver that was NULL for "not known yet" in
one caller and would have meant "no process" in another. A
`SYS_ERR_FAULT` read as a verdict about the domain forever after rather
than as one refused send. And the reverse in stage three: two
denominators, entries per page and flushes per half, stated and checked
as one because in this tree every buffer is one page.

**Measuring the workload and calling it the mechanism.** The rate check
multiplied by the number of messages, which held only because every
message here carries exactly one page. A block size larger than every
file, so the loop that makes a filesystem a filesystem never ran twice.
A switch count that moves with when console input arrives, put in a file
compared exactly. Each of those passed, and each was a check about this
session rather than about the system.

**A rule attached to the wrong event.** The cohort's fault rule sat
where a process ends rather than where a thread faults, so a member
whose faulting thread was not the last one out ended nothing and
reported success. Every member in this tree has one thread, which is why
it passed its own check and every by-hand run of it.

**And the milestone undoing what an earlier one earned.** Tier 2 ended
the rule that an image may have only one live copy, and said so in a
comment that stage one read while writing the code that put it back for
cohort members. Reachable from the shell, and instrumenting the loader
showed a running member's slot base move from one frame to another
underneath it.

### 9.5 What Is Still Untested

**Address space identifiers**, which is the comparison the question
needs and the largest single cost in the system.

**Lending on a page held on loan**, which is the cheaper candidate for
the one thing a cohort uniquely does.

**The teardown of section 18.2.** A member's fault halts, which is the
honest stub and not the mechanism. M7.

**Scatter-gather and lazy mapping**, deferred out of this milestone
because nothing forwards a payload without inspecting it before M10.
**Cohort scheduling nodes**, deferred to M8, because one core running
nothing concurrently cannot observe time staying inside a group.

**A cohort with more than two live members.** The peers cohort names
three and never has three running, because the third one's job is to
die.

**Any load at all.** Every number here comes from a session whose entire
contents are three short strings, and the ratio between the buffer path
and context switching is a property of that session as much as of the
system. What does not depend on it is the rate: four entries and two TLB
operations to move one page across a boundary and back.

## 10. Milestone 7, in Retrospect

M7 asked what happens when an exit was not the program's own idea. It
had been deferred five times, and three milestones running had paid for
that by raising a pool size instead of reclaiming a slot. This is what
it settled, what it cost, and what it found so often that the pattern is
worth more than any of the instances.

### 10.1 The Question

**Is "the disappearance of the only party who could satisfy a wait is
itself an event" sufficient in practice?**

Yes, and the evidence is that nothing anywhere in this milestone uses a
deadline to discover a death. The two sides of a rendezvous fail
differently because they are waiting on different things: a server is at
a receive point and collects a pulse, a client is not and is failed with
`SYS_ERR_OWNER_DEAD`. Both directions were built and both were broken on
purpose. A supervisor destroying a server does not have to guess how
long its clients should wait, which is the property section 18.2's
restart depends on and the one a timeout could not have given.

**Do the generation counters close the recycling hazard under real
reuse?**

Yes, and this milestone is the first thing that could ask. Nothing had
ever recycled a pool slot before stage one: the counters had been tried
only against slots freed by hand. A thread exits, its slot goes back,
the next thread takes the same index with a bumped generation, and a
reply addressed by the old name is refused rather than delivered to
whoever inherited the slot.

**Does a restarted server leak per-connection records?**

Not answered, and the reason is worth more than the answer would have
been. `PROPOSALS.md` gates connection death notification on that
observation, and to make it a server has to hold state per client and
act on `SYS_PULSE_DEAD_OWNER` to let it go. Nothing here does: the check
collects the notice and confirms it names the right client, and no
server in this tree has state worth reclaiming.

What was found instead is larger and was not being asked about. **The
kernel never freed a connection at all.** One was destroyed only when
the handle it was going into could not be made, so the connection pool
counted connections ever created rather than connections open at once,
which is not the bound section 25.2 says it is. It was found by
exhausting it: a check added four more connections than the boot
sequence used to make, and `cat` reported `-4` on the second half of a
union.

### 10.2 What It Cost, and What It Gave Back

Thirteen commits, thirty-eight files, about 4600 lines added and 310
removed. Fifteen hundred of those are the check sequence, which is the
honest shape of a milestone about failure: most of the work is arranging
for something to go wrong on purpose and at a known moment.

**The pools stopped tracking how long the boot sequence is.** They were
raised in M5, M6 and M6 again because nothing was reaped, and stood at
56 processes and 96 threads. They are 24 and 32 now, which is about
twice what runs at once, and kernel object memory fell from 606976 bytes
to 232640.

| | before M7 | after |
|---|---|---|
| processes at once | 46 of 56 | 13 of 24 |
| threads at once | 65 of 96 | 16 of 32 |
| kernel object memory | 606976 | 232640 |

The prediction written before the building was under fifteen processes
and twenty-five threads. It holds.

**What does not come back is memory.** A slot returns to its pool; the
megabyte a process was loaded into does not, nor do its page tables. The
report says so with the two numbers equal:

```
frames: 23843 of 32768 used at most, 23843 still held
```

Closing that needs the loader to record which pages it allocated,
because a demand-paged process runs on frames belonging to its pager's
grant and freeing those would take memory from whoever granted it. It is
a stage of its own and was named as one rather than half-done.

### 10.3 What the Building Kept Finding

**Reclaiming an object turns every unreferenced pointer to it into a
hazard, all at once.** Four times, and it is one finding rather than
four. A completion timeline lives inside its process, so a handle to it
is a pointer into a pool slot. A channel keeps its owner. A process
keeps its pager as a connection object. A process keeps a list of its
threads. Every one of them was safe for as long as nothing was ever
freed, and every one became a way to name whoever took the slot the
moment something was. The generation counters do not help: they protect
identifiers, and these are pointers.

The rule that came out of it is that anything the kernel keeps a pointer
to, it holds. That is now true of processes, threads, connections and
the objects embedded in them, and it is written into the design beside
the mechanism rather than left to be rediscovered a fifth time.

**A check written against a state the system reached later than the
check looked.** Four times, and always passing while testing something
else. A cancelled client that never raised the server it was supposed to
have raised. A pulse delivered to a receiver that was not yet waiting. A
server killed before it had received the buffer the kill was supposed to
unwind. A place asserted free before the thing holding it had been
reaped. They have one cause: a program spawned at the checking thread's
own priority does not run when it is readied, only when the checker
blocks or yields, so a check that spawns and then acts is acting on a
program that has not started.

Every one of them was found by asserting the *precondition* rather than
only the result. A test of undoing something that never happened passes
for the wrong reason, and the way to find out is to say what must have
happened first and check that too.

**Ordering inside a teardown, twice, twenty lines apart.** Telling a
server that its client had gone woke the server in the middle of
dismantling that client, and a server that outranks the thread doing the
dismantling runs at once: it found a client that still looked like one
waiting for a reply, replied to it, and released a thread that was
halfway through being released. The fix was to defer, so that a run of
readies is one step. The same defect was then reintroduced in the code
released clients go through, written the same afternoon.

**And the answer that has to be two answers.** `SYS_EXIT_COHORT` rather
than `SYS_EXIT_FAULT` for a member that goes with its cohort, because
that process did not fault and a supervisor has to be able to tell which
one was the reason. `SYS_ERR_WOULDBLOCK` rather than `SYS_OK` for
cancelling a thread that is not blocked, because a caller told `SYS_OK`
may assume the thread is on its way out of a call. Both were decided the
right way this time, which is a change: the same question was got wrong
in M6 and had to be corrected.

### 10.4 What Is Still Untested

**Memory.** Frames, page tables and address spaces. The number is in the
report and nothing moves it.

**A server that acts on a death notice.** The mechanism delivers it and
nothing consumes it, so the observation `PROPOSALS.md` waits on is still
waiting.

**The storage cohort torn down and restarted.** `fs_server` is in the
recorded session, so making it fault would change the transcript rather
than test a mechanism. What that leaves untried is a cohort whose
members hold state for clients outside it.

**A thread that never enters the kernel.** Killing unblocks every thread
of a process and each leaves at its next return from the kernel, which
is what section 10.5 asks to be prompt. A thread computing in user mode
and calling nothing is not stopped, and there is no hook in the trap
path that would stop it.

**Two processes holding the receive right on one channel.** Ownership
follows the right and the last insert wins, which is arbitrary. Section
12.5 speaks of the owner as if there were one.

**Everything about concurrency.** Every ordering argument in this
milestone is about one core with interrupts disabled and a deferral
counter. M8 is where each of them has to be made again.

## 11. Milestone 8, in Retrospect

M8 asked whether this kernel could run on two cores at all, and then
whether the way section 11.4 says to do it is a way anyone would want.
It answered the first plainly and the second only in part. What it found
in between it found so many times that the pattern is the milestone's
real product.

### 11.1 The Question

**Is partitioned scheduling with no load balancing usable?**

Yes, as far as this evidence goes. The storage stack runs split across
two cores with the filesystem on one and the block store on the other, a
read still works, and nothing above the placement notices. The price is
four inter-processor interrupts and two donations per read, against none
for the same read with both servers on one core. Those are measured, and
the zero is measured too rather than assumed.

**Is the absence of automatic placement intolerable in practice?**

Not answered, and the milestone should not pretend otherwise. There was
nothing here for a balancer to have done better: the stack has one
obvious placement, it was placed by hand in one call, and it stayed.
Answering this needs a workload whose right placement changes while it
runs, and every program in this tree is started by the checker and does
a fixed amount of work. Section 11.4 is therefore neither deleted nor
endorsed. What is established is narrower and still worth having: the
mechanism it specifies exists, its cost per boundary is four pokes, and
a stack placed by hand across two cores behaves as the same stack on
one.

The analysability argument that motivated the choice is untouched,
because nothing here measured latency. That is M9's question, and it is
the one that will decide whether the trade was worth making.

### 11.2 What It Cost, and What It Gave Back

Six stages. The compared session is byte-identical in all four
configurations, and at Tier 2 it is produced with the storage stack
split across two cores: the shell reads its files through a boundary
that crosses a core, and the transcript does not know.

The numbers, at Tier 2 on one port:

- 2 cores, and a build that asks for two and finds one stops rather
  than running on one quietly.
- 4 pokes and 2 donations for one read across one boundary; 0 and 0 for
  the same read on one core. Over the whole run, 34 and 15.
- 432 address space switches, 430 on the first core and 2 on the
  second, with the sum checked against the parts.
- 16844 pool slots taken and given back, 8649 on the first core and
  8195 on the second, the second core's share being almost entirely the
  check that hammers a pool from both.
- 18 of 32 threads and 13 of 24 processes, against 16 and 13 at Tier 0.
  The two extra are the second core's idle context and one client.
- The second core went round its idle loop 19 times, having spun
  millions of times in stage three before there was an interrupt to
  wake it.

What it gave back is smaller than what it cost, and the honest summary
is that most of this milestone was not about two cores. It was about
finding every place where one core had been assumed, and most of those
places were assumptions nobody had written down.

### 11.3 What the Building Kept Finding

**One of something for the machine.** This is the pattern, and it turned
up six times in six stages: the deferral counter and the parked-thread
slot in stage one, the count of locks taken in stage three, the count of
scheduling decisions in the same stage's review, the value the interrupt
controller hands back on one port in stage four, and the whole set of
cost counters in stage five. Twice it was found within minutes of the
previous one, in the same file, by the same reasoning. The lesson is not
that shared state needs locking, which everybody knows. It is that a
field written by whichever core happens to be running is almost never
the field anyone meant, and the question to ask of every one is whether
it decomposes per core or is a property of the machine. A count of
events decomposes. A high-water mark does not.

**Checks that assert something adjacent to what they claim.** Four
times. The count of locks taken during a scheduling decision counted the
other core's system calls. The check that two cores never held the same
pool slot compared a value against what it had just written, through a
plain member, which the compiler deletes. The guard proving a read
crossed a core boundary asserted a poke had been sent, and a poke is
sent for any reason at all. And the first migration check waited by
spinning, so that when migration was disabled to watch it fail, it hung
instead of reporting. Every one of these passed. That is the point:
a check that cannot fail is indistinguishable from a check that passes.

**Windows between a test and the action it justifies.** Three: the trap
vector pointed at a handler before the register that handler needs was
set, the thread published to another core's list was then read to find
out which core to poke, and the idle loop enabled interrupts before
testing the queue rather than after. The last of these was a lost wakeup
that hung a core, and it was found only by running the check on a loaded
host, where it failed one run in three while passing every time on a
quiet one.

**And one hole that was not a race at all.** The per-core pointer lives
in a register, and on one of the two ports that register is an ordinary
one a program may write. Nothing put the kernel's value back on the way
in, so a program with one instruction in it could make the kernel read
its own per-core state through an address the program chose. It stopped
the machine dead, with no panic, because the panic path reads the same
pointer. That is the most serious defect this project has produced, it
survived five stages, and it was found by asking what a counter in the
exception path was indexed by.

### 11.4 What Is Still Untested

**The reference counts were listed and not fixed.** The milestone's own
opening names `p_refs`, `t_refs` and `cn_refs` as plain increments that
decide when an object is freed, and says a racing decrement turns M7's
stale-pointer lesson into a use-after-free. They are still plain
increments. The exposure is real rather than theoretical: a handle
closing on one core decrements through the kernel lock, and a thread
being reaped decrements from a dispatch path that does not always hold
it. Nothing has been seen to go wrong, which is exactly what this
milestone learned not to trust.

**The kernel lock covers entries and not the kernel's own threads.**
Stage two said this stops being sound in the stage that schedules
something on the second core, and stage three scheduled something there.
What M8 did instead of closing it was lock the two shared structures
that were actually reachable from both cores, the object pools and the
timer's lists. That is narrower than the claim stage two made, and the
difference is a real gap rather than a wording problem.

**Two cores, and only two.** `SYS_MAX_CORES` is 2. Nothing here has run
with more, and the arming of the timer names one core by a constant.
The interrupt controller on one port also addressed its external
sources to the first core's context, which was fixed after M10 closed;
the M8 entry that recorded it as a known limit says what was done.

**Nothing measured latency**, which is the property the whole
partitioned design was chosen for.

**And the check harness has its own limit.** Under host load heavy
enough to starve the emulator, the paced session can be outrun by the
pacing itself. One run failed that way with nothing in the guest to
blame, and it is recorded here rather than explained away.

## 12. Milestone 9, in Retrospect

M9 is where "no figures are claimed" stopped being true. The priority
mechanisms had been present since M1 and nobody had ever measured one.
Six stages later there are numbers for both terms of section 13.3, for
the round trip and for donation, on two ports and two tiers, and the
section they were taken for has been rewritten around them.

The measuring was the easy part. Four of the six stages published a
figure that turned out to be measuring something else, and the
milestone's real product is the account of how each one was caught.

### 12.1 The Questions

**Do the figures falsify the per-tier interrupt latency argument of
section 13.3?**

Yes, and not in the direction anybody expected. The section says Tier 0
accepts a latency floor bounded by preemption point placement, and Tier
2 removes that floor by enabling interrupts and locking finely. Both
halves are wrong about this implementation.

The floor is not set by any critical section. The longest
interrupts-disabled region begins in `channel_reply` and ends in
`channel_send`, which is a server replying, dispatching into its client,
and that client re-entering the kernel to send again. All four
configurations agree on both ends. It contains no preemption point and
cannot: preemption points sit at batch entries, message copy boundaries
and page table walks, and none of those is on it. Stage three placed
them where the section says and every one lowers a region that was never
the longest.

So Tier 0's latency is not bounded by preemption point placement, and
Tier 2 enabling interrupts would not remove a floor that the scheduler
sets. The section keeps its per-tier shape, because the tension it
describes is real, and its account of what sets the floor is replaced by
the measurement.

**Is the tickless timer affordable on a part where reprogramming is a
slow peripheral write?**

Not answered, and it cannot be answered here. Neither port has a timer
that is slow to reprogram: one writes a supervisor register and the
other a system register. The milestone said this needed saying rather
than guessing, and saying it is all that can be done.

### 12.2 The Figures, and What They Are Figures Of

Every number below was taken under an emulator on one host. The counts
are properties of this kernel and are identical everywhere they should
be. The durations are properties of that host and are not.

| | Tier 0 riscv64 | Tier 0 aarch64 | Tier 2 riscv64 | Tier 2 aarch64 |
|---|---|---|---|---|
| longest region | 596 to 1009 us | about 500 us | 571 to 733 us | about 800 us |
| mean region | 16 us | 16 us | 5 us | 7 us |
| regions in a run | 580 | 580 | 20500 | 20400 |
| interrupt to a program | 82 us | 147 us | 168 us | 189 us |
| points on that path | 0 | 0 | 0 | 0 |
| round trip, best | 40 us | 31 us | 42 us | 34 us |
| donation, one core | 15 us | 12 us | 18 us | 14 us |
| donation, across cores | none | none | 56 us | 75 us |

**Crossing a core costs about three times a donation that does not, and
both ports say so.** That is the same boundary M8 priced at four
inter-processor interrupts per read, seen from the other side and in
time, and two independent measurements agreeing is what makes it worth
believing.

**The tiers cannot be told apart in the worst case.** Four runs of each
give 596, 1009, 619, 621 microseconds at Tier 0 and 733, 601, 580, 571
at Tier 2. The spread is larger than any difference between them.

**They are sharply different in the typical case**, 16 microseconds
against 5, stable to a few per cent. The big lock M8 expected to make
Tier 2 worse makes twenty thousand short regions where the other tier
has six hundred longer ones, and leaves the worst case alone.

### 12.3 What the Building Kept Finding

**A measured interval that included something that was not being
measured. Five times.**

The idle wait, when the longest region came out at 480 milliseconds and
a timer fires every millisecond. The return to user mode, when it came
out at 248 and the region was open across everything a program did. A
server donating to itself, which put the two seconds between two typed
lines into a figure about the kernel. The boot, which is not a path the
running system takes and was the largest region in every run by two
orders of magnitude. And a difference between the tiers that was read
out of one run of each, twice, in opposite directions.

Every one of those numbers was plausible when it was published. That is
the whole difficulty: a wrong latency figure does not look wrong, it
looks like a latency figure. What caught all five in the end was the
same question asked twice, where did this interval start and where did
it end, and is each of those somewhere the question makes sense.

**A per-core counter differenced across cores.** The points a donation
passed, subtracted between the core that stamped it and the core it
arrived on, printed 18446744073709501435. M8 found this pattern six
times and named it in this milestone's own entry as the thing to watch
for, and it happened anyway.

**And a check that could not fail.** The count of preemption points was
compared against what had just been written to it through a plain
member, which the compiler deletes. The per-core counter check was made
with one core. The chain walk's raising half had never once run, and
"raises nobody" was reported as a result rather than as a gap.

### 12.4 What Is Still Untested

**Everything about durations, because none of this has run on hardware.**
The figures have the right shape and the wrong magnitude. What can be
read from them is which term dominates, whether the ports agree, and
whether the tiers differ. What cannot is anything a real-time engineer
would want.

**The retired instruction counter, which was the unit this milestone
wanted and could not have.** It is architecturally an instruction count
on one port and the emulator answers it with something time-shaped:
130000 for a thousand-iteration loop, and about the same again for a
loop twice as long.

**The chain walk raises nobody in this system's own work.** A workload
built to need it raises two, so the mechanism is exercised; nothing this
tree does for its own sake produces the case.

**The bound on the donation chain is enforced by counting**, so a cycle
and a chain that is too long are the same event. Nothing distinguishes
them and nothing needs to yet.

**And the reference counts M8 listed as unsafe are still plain
increments.** Two milestones have now named them without fixing them.

## 13. Milestone 10, in Retrospect

M10 was deliberately last of the ten this plan was written with, and
with it that list was finished. M11 was added afterwards, out of this
retrospective: the experiment named below as the one that would settle
the question this milestone could only half answer. It asked whether the most novel mechanism
in the design earns its place, and the answer it can give is narrower
than the question, for a reason it found rather than assumed.

### 13.1 The Questions

**Does expressing dependencies as data beat a few more system calls?**

Half answered. Batching beats separate calls by exactly the fixed cost
of a call, multiplied by one fewer than the number of entries: a batch
of eight saves seven crossings of the trap edge, seven validation checks
and seven acquisitions of the kernel lock. It saves nothing per entry,
because every entry is checked before it runs and again when it runs
whichever form carried it.

The dependency half was never tested. The pattern the mechanism exists
for is a chain, where one entry names a handle an earlier entry
produced, and an entry's target is fixed when the batch is submitted.
Section 17.7's answer to that is an array of handles the entries name by
position, and it is the one item of the *Needs* list that no stage of
this milestone built. So the workload that would justify the engine is
the workload the engine cannot carry.

**Does the branchless form stay sufficient once real code uses it?**

Not answered, because no real code uses it. A run makes between 733 and
758 submissions and every one carries a single entry except the handful
this milestone's own checks submit. The engine was built and nothing
adopted it, and the reason is the same missing piece.

**So section 17 stays, with its claims narrowed to what was measured.**
Removing it would be deciding against the idea on evidence that did not
test the idea. Keeping it as it stood would leave claims larger than the
facts. What would settle it is named in stage six: build the argument
substitution, convert one real client, and measure the same way.

### 13.2 The Figures

| | measured |
|---|---|
| eight operations, one call each | 8 crossings, 24 checks |
| the same eight in one batch | 1 crossing, 17 checks |
| four entries, four times, as batches | 36 checks |
| the same, loaded once as a program | 26 checks |
| longest region, one entry against sixteen, Tier 0 | 59 against 49 ticks |
| the same at Tier 2 | 134 against 795 ticks |
| submissions in a run | 733 at Tier 0, 758 at Tier 2 |

Every count is identical in all four configurations, which is what M9
established a count is for.

**The latency row is the milestone's other finding.** Section 13.3 says
`SYS_BATCH_MAX_ENTRIES` does not appear in interrupt latency and that
the cost of one entry does. It is true at Tier 0, where the longest
region inside a batch is one operation's own critical section, and false
at Tier 2, where the kernel lock is taken at the entry and held with
interrupts disabled until the call returns, so sixteen entries is one
region sixteen entries long. Both ports agree at each tier and the site
is the same in each pair.

That is M8's deviation costing something at last. M9 measured it and
found the latency floor unmoved; it was unmoved on a workload with no
batches, and this is the workload that shows the price.

### 13.3 What the Building Kept Finding

**Something that was correct when the count was one, left alone when the
count stopped being one.** Twice, in the same twenty lines. The status
write-back, so that an entry refused before it ran said nothing about
why. The check that a killed process leaves the kernel, which sat after
the operation and so let a killed thread run fifteen more. Turning a
scalar into a loop means asking of every line around it whether it
belonged to the operation or to the call, and the answer was not
obvious for either of them.

**A decision written down in one place, reversed in another by someone
who had not read it.** The validator tested the argument length where
the code it duplicated tested the pointer, which turned a deliberate
`SYS_ERR_INVALID_ARG` into `SYS_ERR_FAULT`. The comment explaining the
original choice was six lines from the new code.

**A new object type has a lifetime as well as a behaviour, and the
behaviour is the part that gets checked.** A prepared program was never
freed when its handle closed, so the fifth one a process ever loaded was
refused for want of room nothing was using. The stage checked what the
object did and not what became of it.

**A check that passed on no data.** The latency measurement ran before
its instrument was armed, printed two zeros, and reported that section
13.3's claim held. Both figures being zero is not a result and the check
says so now.

**And two M9 measurements that had been right by luck.** Preemption
points were counted per core, so a batch running in one thread put
sixteen of them into a figure that was supposed to be the length of a
dispatch; they are counted on the thread now. And a kernel thread
reaching the system call exit was recorded as an interrupt dispatched to
a user thread, which is not what section 13.3's second term means. Both
were correct until this milestone gave the core something else to do,
which is what "right by luck" means and why it is worth writing down.

### 13.4 What Is Still Untested

**The half of section 17 that needs a device.** Three transfers in
flight, in the submitting thread's context, at its priority, with no
worker pool. Nothing in this tree does direct memory access, the three
issuing opcodes still answer `SYS_ERR_NOT_SUPPORTED`, and the milestone
recorded that at planning time rather than building something that
looked like a test of it.

**The argument substitution**, which is both an unbuilt *Need* and the
reason the main question is unanswered.

**Section 17.5's last line**, that on abort every timeline the batch was
going to signal is force-signalled in error state. That is about issuing
ops, so there is nothing yet for a cancelled entry to have been going to
signal.

**And the reference counts.** M8 listed `p_refs`, `t_refs` and `cn_refs`
as plain increments that two cores make unsafe. M9 listed them again.
This is the third milestone to name them without fixing them, and the
fact that they are named in three retrospectives rather than fixed in
one is itself a finding about how this project has been prioritising.

*Since fixed, immediately after this milestone closed.* The count and
the decision to free are taken together under one exclusion, and the
freeing is done after it is released, because reaping an object closes
handles that release references to other objects and would arrive back
at the same lock. A check hammers one count from both cores and
requires it to come back where it started; with the plain increments it
comes back at 230 instead of 1, so the hazard three retrospectives
described was not a small one.

## 14. Milestone 11, in Retrospect

M11 was not in the plan. It came out of section 13, which closed by
naming the experiment that would settle the question M10 could only
half answer: build the argument substitution, convert one real client
to it, and measure the same way. This is that experiment, and its
answer is that substitution pays in what can be said rather than in
what can be counted, which is not the answer the section was hoping
for.

### 14.1 The Questions

**Does expressing dependencies as data beat a few more system calls?**

Answered, at last, and the answer is *not by itself*. A four-entry
chain that carries a connection in a register costs three fewer
crossings of the trap edge and three fewer validation checks than the
four calls it replaces. Those three checks are the three array reach
checks the removed calls would each have paid, so the saving is the
fixed cost of a call multiplied by one fewer than the number of
entries, and nothing per entry. That is precisely the law M10 measured
for batching *without* dependencies, arrived at again by a batch that
has them.

So substitution adds no saving of its own. What it adds is that the
batch can be written at all: the second entry names a handle that does
not exist until the first has answered, and before this milestone a
caller had no way to write that down.

**Is that worth a mechanism?** The honest comparison is not against
separate calls but against the other way of removing them. The
compound protocol request of section 25.7 saves two rendezvous and
never draws a connection from the server's pool, where the engine saves
three crossings and one array check per call removed. **For this
workload, changing the protocol beat changing the kernel.** The
engine's saving is real, small and fixed; the protocol's is larger and
grows with what a server can be asked in one message.

**Can an issuing op exist in this tree?** No, and the reason is now
precise rather than a shrug. Starting work means a device that
transfers and nothing here transfers. The route that looks open, a read
that issues against the block store, is an issuing send, which section
17.3 rules out for a structural reason: a thread with an outstanding
issuing send is not blocked on it, so the blocked client can no longer
be the queue slot.

### 14.2 The Figures

| | measured |
|---|---|
| `fs_client`, three requests as three calls and a close | 5 submissions, 15 checks, 5 rendezvous, 1 connection |
| `fs_batch`, one compound request | 4 submissions, 12 checks, 3 rendezvous, 0 connections |
| `fs_chain`, the same three in one submission | 2 submissions, 12 checks, 5 rendezvous, 1 connection |
| registers per execution | 4 |
| three completions waited on in one submission | about 60 ms |
| the same three waited on one at a time | about 121 ms |

The four counted columns are identical on both ports, at both tiers and
in every run, and they are asserted rather than printed. The two
durations describe the host and are reported.

**Read the table across rather than down.** `fs_chain` and `fs_client`
agree on every column but the first: the same rendezvous, the same
connection, the same buffers remapped. That is what says substitution
changed the shape of the call and not the work.

**The two durations show less than they appear to.** A timer does no
work, so sixty against a hundred and twenty-one is arithmetic about a
clock rather than evidence that anything ran concurrently. What they do
show is the engine's shape: several completions outstanding at once,
waited for in one submission, by one thread with no worker pool.

### 14.3 What the Building Kept Finding

**Words describing something that was not built.** This is the shape,
and it recurred in four stages out of six.

Stage one said the register file lived on the stack of the call that
runs the batch, and that this was what made it private to one
execution. Nothing in the batch path owned one; the check had called
the initialiser on a local of its own.

Stage three's program was named `fs_chain`, its comment described a
chain, and it set no link flag on anything.

Stage five said "three things in flight" of two numbers that are
arithmetic about a clock.

Stage six's cost table asserted nothing at all for a client whose name
it did not know, and would have passed for ever after a rename.

**A check written to prove one thing proving its opposite.** Stage
three wrote a check to show its chain aborts, and the check showed that
it does not: a walk for a name that does not exist *succeeds*, because
the refusal is in the server's payload and the kernel does not read
payloads. Section 12.4 requires a status field on the message for
exactly this and `sys_msg_t` did not have one. The program was safe
only because a failed walk moves no handle, so the empty register
stopped the read that the chain should have stopped. Right by luck,
which is the phrase M10 introduced and the thing this milestone found
twice.

**Two validators that had to agree and did not.** Adding register
fields to an entry made the submission path and the prepared-program
path disagree, and the second says in its own comment that it does the
same validation as the first. They share one function now.

**And half a fix is the normal outcome of a fix.** The status field was
set in two of the three servers that can refuse a request. The window
figure kept counting console regions after the run-wide figure stopped.
The mean kept the console after the worst case lost it. In each case
the second half was found by a later review rather than by the change
that made it necessary.

### 14.4 What Is Still Untested

**The issuing half of section 17**, unchanged from M10 but now with a
reason rather than an absence. There is no device that transfers, the
route through the block store is an issuing send, and `PROPOSALS.md`
carries that.

**Whether error state is the right answer to an abort.** Section 17.5's
last line is built: a batch that aborts force-signals the timelines it
was going to signal. But error state is permanent and belongs to the
timeline rather than to the batch, so an abort poisons an object other
processes may hold a signal right over. Section 9.3 rule 2 has the same
shape for a party that has died, and a party that has died cannot
signal again where a batch that failed once might. Built as the section
says and unsettled.

**A server that forgets the status field.** Nothing detects it. A
refusal that reaches only the payload is a successful send, and the
chain behind it carries on, which is the defect the field exists to
prevent reappearing one server at a time. Three set it today.

**The fault path on a substituted entry.** A fault between the check
every entry gets before any entry runs and the check the operation
makes would have written the status into the copy rather than the
caller's descriptor. Fixed, and reachable only through a race no check
here can construct.

**The other writer of a register.** Section 17.4 names two producers,
a send and the issuing ops. Only one of them exists, so the register
file has been exercised by one kind of writer and the tag discipline
has never had to refuse a handle that came from anywhere else.

---

## 15. Milestone 12, in Retrospect

M12 attempted the first of the two remedies M9 named for section 13.3's
first term: not disabling interrupts across a switch. It lowered the
figure a little, established that the figure is mostly not the switch,
and then found that the figure it had been lowering was not the one the
section is about.

### 15.1 The Questions

**Can the floor be lowered by not disabling interrupts across a
switch?**

A little, and less than it looks. Three splits were made, each argued
from what the state is rather than from what the figure does: a reply's
own work no longer shares a region with the dispatch that follows it,
and a thread waking from a rendezvous copies its message out after
restoring interrupts rather than before. None of the three moved the
figure by more than the run-to-run spread. The sites moved, which is
deterministic and therefore the solid half of the result; the durations
did not, because what remains is the rendezvous itself.

**What is the region made of, if not the switch?**

The switch is between nought and three per cent of it, measured inside
the region rather than beside it, over twenty runs. The rest is the
rendezvous work that has to be atomic with the switch: popping the
channel's receiver list, handing a buffer between address spaces,
walking a donation chain, queueing a thread. M9's account, that the
floor is the switch, was half right. The region spans a switch, which
is why no preemption point can break it. Its length is the work.

**Does M8's kernel lock put it back at Tier 2?**

No, and the mechanism settles it before the measurement does.
`klock_drop` and `klock_regain` touch the spinlock and not interrupts,
so the lock is put down across the switch and the interrupts-off region
is not. The medians agree to within nine per cent between tiers, in
both directions.

**And the question this milestone did not set out to ask.**

Whose latency is this? The longest region that spans a switch is opened
by a kernel thread in every run of every configuration, doing
rendezvous work in the checks. The longest region that begins while a
program is the current thread is four to eight times shorter. Every
figure section 13.3 has carried since M9 was the first of those, which
is the latency of a test harness.

### 15.2 The Figures

| | Tier 0 riscv64 | Tier 0 aarch64 | Tier 2 riscv64 | Tier 2 aarch64 |
|---|---|---|---|---|
| longest across a switch, kernel thread | 449 us | 559 us | 465 us | 530 us |
| longest with a program current | 83 us | 115 us | 60 us | 70 us |
| the switch inside that region | 0 to 3 per cent |
| the switch on its own, mean | 4 to 7 us on either port |
| switches in a run | 469 at Tier 0, about 4,480 at Tier 2 |

Medians of five runs each for the first two rows. Both ports and both
tiers agree on the shape of every row, which is what says these
describe the kernel rather than either machine, and every one of them
is a duration and so is reported and not asserted.

### 15.3 What the Building Kept Finding

**Measuring the wrong thing, four times, each time differently.** This
milestone is about a measurement, so the mistakes were all in the
measuring, and no two were the same mistake.

Stage one's figures were for regions that never switched. A reply
dispatches only when the client outranks the server, and the instrument
counted the regions that did not, naming the longest of them after the
path that does. On one port the published figure, 7263 ticks, was a
region with no switch in it at all against 5396 for the real one.

Stage four set the mean switch against the worst region and called the
ratio one per cent. The mean switch is larger than the mean region; the
ratio looked small only because the numerator was a mean and the
denominator a maximum. Measured inside the same instance it is one per
cent after all, so the conclusion survived and the arithmetic did not.

Stage five's review found that the whole milestone had been lowering a
figure that belongs to the kernel's own threads. Nothing in twelve
stages of measurement had asked which thread was current.

Stage six published four single runs as though they were the
measurement, one stage after stage four's review had caught the same
family of error.

**Splits that took shared state out of its exclusion.** Moving a
restore before a copy also moved a read-modify-write on the channel's
dropped-pulse flag, whose other side is set under the same exclusion. A
pulse arriving in the window would have been lost with nobody told.

**A check that asserts the negative and never the positive.** That
defect was invisible because the check for the flag required that a
loss is not reported twice and never that it is reported once. Making
the flag always read false passed it perfectly.

**And entries describing code that does not do what they say.** A
comment claimed an interrupt in the gap might dispatch to the client
itself, which the deferral is there to prevent. A stage claimed three
assertions where the report makes two.

### 15.4 What Is Still Untested

**The other remedy.** Making the switch interruptible was declined at
planning time as a port question, and is now known to be worth nought
to three per cent of the region. That is a better reason to decline it
than the one the plan gave.

**Whether the rendezvous can be made shorter or less of it atomic**,
which is what lowering the floor now means. Nothing here attempted it.

**The ordering the splits rest on.** Moving a restore before the state
it protects changes nothing any check notices; the run completes. Three
splits are argued from what the state is, and a race no check here can
construct is what would catch them being wrong.

**Whether the second row is the right bound.** It is the longest region
that began while a program was the current thread, which includes
regions a handler opened while that program ran. That is the right
question for how long the program was delayed and the wrong one for
what the program's own path costs, and only the first is measured.

**Hardware, still.** Every duration in this section describes an
emulator, and the two ports agreeing tells us about the kernel's shape
rather than about either machine's speed.

---

## 16. Milestone 13, in Retrospect

M13 set out to make a process give back what it was given, which
section 1 had said since M9 was not happening. It did that, and the
work was almost entirely in finding out what a process actually holds
and when nothing can reach it any more. The freeing itself is about
eighty lines.

### 16.1 The Questions

**What does a process cost?**

At Tier 2, 264 frames on riscv64 and 262 on aarch64: a megabyte of a
slot for a program whose image is four kilobytes, plus the tables to
reach it. `proc_load` allocates and maps the whole slot because the
archive carries the size of the image and not the size of the memory
the program needs.

**At Tier 0 it costs nothing at all**, which changed the milestone's
shape at its first stage. A flat image is copied to the address it was
linked for and runs in memory the archive already occupies, so there is
no per-process allocation and nothing about a process to reclaim. Every
frame this milestone gives back is a Tier 2 frame.

**Can the system now run indefinitely?**

A program was spawned and reaped 40000 times on a machine with 32768
frames, and the count came back to where it started. That run asked the
allocator for 10,560,000 frames on riscv64 and 10,480,000 on aarch64,
more than three hundred times what the machine has, in about seven
minutes each. Before this milestone the same loop stopped at about the
ninety-fifth iteration with no memory left. The boot check runs 200
cycles, which still asks for 52800 frames against the 32768 that exist.

**When is a process finished with?**

At the reap, and the reap is not the join. Waiting on a completion
timeline says a process has exited and closing its handles says nothing
can ask about it any more, but its last thread is *parked* at that
point rather than collected, and the collection happens at the top of
whichever core's next switch reaches it. Every measurement in this
milestone had to learn that: the first version of stage one saw a
second spawn appear to cost 8 frames instead of 264, because it read
the count in the gap.

**Was ownership already known well enough to free on?**

Yes, and that was the milestone's open question. The reference counting
M11 fixed says exactly when a process object is finished with, and the
frames follow it without anything new. What was *not* already known is
which frames in a process's address space are the process's: pages a
pager supplied are its grants', and a cohort member's slot may belong
to a replacement by the time the reap runs. Both are ownership
questions the design had answered on paper and nothing had ever had to
act on.

### 16.2 What It Cost

The freeing is `proc_free_slot`, `md_as_destroy` on each port, and a
call in the reap. What surrounds it is larger: a boundary recording
where the loader stopped allocating, a test that a cohort member still
holds its place, a lock for the frame allocator, a generation count for
address spaces, and a ledger with six rows and five new accessors
behind it.

**The frame allocator had no lock.** Every other shared structure takes
one at Tier 2, and this one relied on disabling interrupts, which is
exclusion on one core and nothing on two. It had not mattered while the
only caller that freed a frame was a boot check. Making freeing
something a reap does is what made it matter, since a reap runs on
whichever core collects the parked thread. Measured, only the boot
thread's core ever enters the allocator, 43849 times on riscv64 with
nothing on the other core, so the lock is correct and untested by
contention. The report prints the zero rather than asserting the lock
is exercised.

**A root is a frame, and freeing one made a root address ambiguous.**
The context switch decided whether to load the root register by
comparing addresses, which was a sound identity only while roots were
never freed. Skipping the load keeps translations for the space that
used to be there, at the same addresses, pointing at frames since given
to somebody else, and the allocator hands out the lowest free frame
while the checks spawn one program eleven times in a row.
`vm_as_epoch` costs two extra activations across a whole boot, 472
against 470.

**One place loses memory unrecoverably**, and it is a consequence of
section 18.2 rather than of anything here. A cohort place is free once
its occupant has exited, which is earlier than when its occupant is
reaped, so a replacement can be loaded over the previous member's
mappings while that member waits to be reaped. Its frames become
unreachable, the reap must free nothing, and the ledger counts the
event at 256 frames a time. Two such restarts happen in this boot.

### 16.3 What the Building Kept Finding

**Checks that cannot fail on the thing they are for.** Stage two's run
of eight processes measured the residue and then asserted the residue.
With the freeing removed it measured 264, found 264 of them eight times
over, and passed. The fix is to assert against a number the system does
not choose, which is the slot's size.

**Numbers read too early.** Stage one measured across the reap it had
not waited for. Stage four read the count the moment a check returned.
Stage five snapped the final count rather than waiting for it. The same
error three times, each time because the join looks like the end of a
process and is not.

**And a proxy that was not the thing.** The first fix for stage four
waited for the process pool's count to return to what it was, which
assumes the check is the only thing making processes. Under the paced
session the console has input to deal with and a process appears that
the check never started. Waiting on the figure being asserted needs no
theory about whose processes are whose.

**Guards argued rather than demonstrated.** Stage two kept the reap off
pages a pager supplied and could not make any check fail without it.
Stage four is what demonstrated it, two stages later. The habit of
writing down which protections are untested is what made that findable.

**A build error read as a result.** Tier 0 reported old figures through
several rounds of stage six because a function had been written inside
the Tier 2 guard: the link failed, the build carried on with the
previous kernel, and the run repeated what it had said before.

### 16.4 What Is Still Untested

**The cohort address space path.** A cohort's space now goes back when
its last member is reaped, and no check in this boot reaps every member
of a cohort, because their handles are held until the machine stops.
Removing the code changes not one frame of what the boot reports, which
is how that was established rather than assumed.

**The frame lock under contention.** Only the boot thread's core ever
enters the allocator. A fault serviced by a pager, or a spawn made by a
program, would enter it from wherever that program runs, and nothing in
this system does either from another core.

**Cross-core TLB maintenance, which does not exist.**
`md_tlb_flush_all` is local to the calling core, so reclaiming a cohort
member's memory while a peer runs on another core is not safe. The
checks reap processes that are in no cohort.

**Whether a slot has to be a megabyte.** Every figure in this milestone
is dominated by the loader allocating a whole slot for an image of a
few tens of kilobytes. Teaching the archive to carry the size of the
memory a program needs would change every number here, and nothing
attempted it.

**Hardware, still.** The durations in this section describe an
emulator. The counts do not, which is the whole reason the counts are
what this milestone asserts.

---

## 17. Milestone 14, in Retrospect

M14 was the milestone M13 asked for. M13 measured what a process costs
and found it was a megabyte, and the reason was one sentence in
`proc_load`: the archive carries the size of an image and not the size
of the memory a program needs. Carrying the layout instead turned out to
buy three separate things, and the entry named all three before any of
them was built, which is unusual for this project and worth noting.

### 17.1 The Questions

**Does the boundary in the link turn out to be the boundary the loader
needs?**

Yes, and it needed no negotiation: the user linker script already
page-aligned the start of writable memory, and said in a comment that
it did so to give the loader a boundary it could see. The milestone's
falsification condition was that a third region would turn out to be
needed, or that a flat binary could not express one. Neither happened.
Two offsets, where writable memory begins and where the program's
memory ends, describe every program in the archive.

**What does a process cost when it is mapped as what it is?**

**15 frames on riscv64 and 14 on aarch64, against 264 and 262.** Seven
of the fifteen are what the image needs and eight are the address space;
on aarch64, eight and six. What the machine holds when the boot ends
fell from 10732 frames to 8519, and the processes still running account
for 119 of those where they used to account for 2352.

**What did the one region cost besides memory?**

Every program's text was writable and every program's stack was
executable, on the tier whose whole purpose is that the hardware
enforces boundaries. One region means one set of permissions, and the
only set that lets a program run is all of them. Two mappings fixed it,
and the two programs that prove it had to be written: nothing in the
archive could write to an address or jump anywhere the linker had not
sent it.

**And section 15.1's eager copy?**

It had never run. Every program had an empty `.data` except `peer_b`,
which stage one discovered by the archive tool refusing to build it, and
no paged program had any. `proc_load` said in as many words that a
program with initialised writable data would break its pager path.
Giving `paged` a word of data and restoring the old condition reproduced
the failure exactly: a page fault eight bytes into the data page, after
one page had arrived.

### 17.2 What It Cost

The mapping change is three lines. The rest is where the work was.

**The build plumbing**, which the entry predicted would be most of it
and was. Two symbols in each linker script, a rule that writes them
beside the binary, a tool that reads them from there rather than from
its command line, two fields in the record, and a format version. The
one judgement worth keeping is that the numbers travel from the link
that produced them rather than from the makefile that assembles the
archive, so they cannot disagree with the image they describe.

**Three new programs**, because the checks needed a program that writes
to an address it is handed, one that jumps into memory that is not text,
and one that does nothing at all. The last was M13's, and this milestone
found the second use for it: the frame checks were spawning `spin`
eleven times to measure what a spawn costs, and `spin` computes for
eighty milliseconds.

**And a rule that keeps appearing.** Three of the six stages assert that
something faults, and the entry said in advance to watch for a check
that passes because a fault happened for the wrong reason. It caught
one: at Tier 0 a jump into a stack is permitted and the zeroes fail to
decode, so the fault is an illegal instruction and not an access
violation. A check that accepted any fault would have read that as a
protection working on a tier that has none.

### 17.3 What the Building Kept Finding

**Words describing what was not built, twice, in the same shape.** A
comment saying a function is called by the loader when nothing calls it
from there. A comment on `VM_PROGRAM` naming the archive as what would
let a program's text be read-only, which had been true and became a
description of the fix rather than of the code.

**Checks that could not fail.** A row expecting nothing held, clamping
a negative difference to zero and comparing that. A check asserting a
page is read-only to prove a record is in it, when the page is read-only
wherever the record went. Both were found by asking what the check would
do if the thing it watches were wrong.

**Stage ordering inside a function.** `proc_prot` asks `proc_slot` where
a process's memory is, and `proc_slot` answers from `p_entry`, which the
loader set after the mapping loop. Every page was mapped while the
process had no slot, the fallback permitted everything, and the stage
passed its own check by doing nothing.

**And where a check runs being part of the check.** A check placed
before a frame measurement gave its memory back in the middle of one. A
check that made the boot two seconds longer moved the shell past the
first line of a paced session, and the transcript lost a line. Neither
is about what the check computes.

### 17.4 What Is Still Untested

**A third region, if one is ever needed.** Text and rodata share the
read-only mapping, so a program wanting rodata non-executable cannot
have it. Nothing has asked.

**Whether a slot has to be a megabyte.** The question M13 raised is
answered differently rather than answered: a slot is now an address
range and not an amount of memory, so its size costs nothing. What it
still constrains is how many programs fit in the window, and nothing
has run out.

**A reference that outlives a join.** Closing a process handle after
joining does not always reap the process: something releases the last
reference later, on work the boot thread does rather than on a yield.
It was found by a wait that could not succeed and is written down here
because nothing in this milestone was about references.

**Hardware, still**, and the counts this milestone asserts are counts
for that reason.

---

## 18. Bring-Up Order Within M1

M1 is large enough to need its own sequence. Each stage produces something
testable, and a stage that fails, fails visibly.

**Stage 1. Boot to a known state.** Take the machine, set up a stack,
initialise the object pools, spin. Nothing to test but that the image runs,
which is worth confirming before anything depends on it.

**Stage 2. One thread, no scheduler.** Build the thread object and the context
switch. Switch between two threads that yield to each other explicitly. This is
where register save and restore gets debugged, in the simplest setting it will
ever be in.

**Stage 3. The scheduler.** Ready queue, priorities, preemption. Test with
several compute-bound threads at different priorities and confirm the highest
runnable one runs.

**Stage 4. Handles and the trap.** The handle table and the operation entry
point. A no-op operation is the first one worth having: it exercises the entire
entry, validation, status-write and return path with no other machinery
involved.

**Stage 5. Timelines.** Create, signal, wait, and the error state. Timelines
come before IPC because interrupts and timers both land on them, and because
they are simpler to get right in isolation.

**Stage 6. Interrupts.** Attach an interrupt to a timeline and drive it from a
real device, the timer being the obvious first one. At this point the system
responds to hardware.

**Stage 7. Timers.** Periodic and one-shot on top of stage 6.

**Stage 8. IPC and spawn.** Channels, send, receive, reply, then priority
inheritance. Process creation, handle inheritance, the startup record. The
driver process and the file read close the slice here.

**One note on order.** Timelines before IPC is the choice most likely to feel
wrong, since IPC seems more fundamental. It is not. Interrupts and timers
depend on timelines and not on message passing, so timelines first gets the
system responding to hardware several stages earlier.

---

## 19. Ordering: What Is Forced and What Is Chosen

Worth separating, because the forced edges are not negotiable and the chosen
ones are.

**Forced.** M2 after M1, since a scheduler running two programs needs the
message passing they rendezvous over. M3 after M2, since a shell that cannot
spawn a process is not a shell. M6 after M4, since a cohort without protection
domains is a naming convention. M7 after M6, since cohort recovery is the first
mechanism that cannot be demonstrated without death, and M7 not before M2,
since nothing earlier has anything to reclaim. M9 after M3, since measuring a
system with no workload measures nothing. M10 after M6, since issuing ops need
a device that does DMA.

**Chosen, and reversible.** M3 before M4. The argument is that the MMU
milestone's real test is running the same software on both tiers, which needs
software. The counter-argument is that porting less code to Tier 2 is easier,
which would put M4 first. If M3 grows larger than expected, invert them.

Also chosen: M7 after M6 rather than between M4 and M5. Death could be built
as soon as faults exist, and the reason not to is that M5 and M6 gain nothing
from it while paying for it. If either turns out to need a process to die and
be replaced before it can be demonstrated, M7 moves ahead of it and nothing
else changes.

**Independent.** M5 is pure userland and libc and blocks nothing, so it can
move anywhere after M3. M8 is almost entirely kernel and shares little with M5,
so the two can proceed in parallel if there is ever reason to.

---

## 20. What Would Change This Plan

Each of these is a plausible outcome, not a worry. The plan is arranged so that
they surface early and cheaply.

**Timelines do not earn their place (M1).** Remove the object, keep blocking
receive, and section 9 collapses. This is the cheapest thing on the list to
discover and the most disruptive to the document, since fences run through
interrupts, timers, exit and the submission engine.

**Compound requests do not pay (M2).** *Answered: they do.* Three
rendezvous become one and the connection pool goes from one allocation to
none, the second being the structural saving. Section 25.7 stands, with an
entry-index field and a close entry removed from it that building it found
to be wrong. Had it gone the other way, 25.7 would have been deleted and
section 12.7 with it.

**One thread per source is unaffordable (M3).** Wait-sets come forward from
`PROPOSALS.md`. The queue model, not the array model, and only after evaluating
whether a timeline enqueuing a coalesced pulse on a channel is sufficient.

**The interface differs between tiers (M4).** The thesis is wrong. Part I is
rewritten and the project needs a different reason to exist.

**Namespaces are unworkable (M5).** Either the mount table moves, or the design
concedes an ambient root. Sections 20 and 25 both change.

**Cohort faults are intolerable (M7).** Cohorts drop to a naming convention and
zero-copy goes through ordinary grants, at the cost the design was trying to
avoid.

**The submission engine does not pay (M10).** It is deleted. Section 23 already
defines the calling convention around a single operation for exactly this
reason, so nothing else moves.

**Substitution does not pay (M11).** *Answered: not in counts.* A chain
that carries a connection in a register saves the fixed cost of a call
multiplied by one fewer than the number of entries, and nothing per
entry, which is the same law M10 measured for batching without
dependencies. What it buys is that the batch can be written at all. The
register file stays, because the cost of expressing a dependency turned
out to be nothing; had it been more than nothing, section 17.4 would
have gone and section 23 is arranged so that removing it costs nothing
else.

---

## 21. Deliberately Not Scheduled

Named here so that they stay named rather than drifting in.

- A GDB remote stub over the debug UART of section 22.4. Named there as the
  companion to the debug console, and deliberately unspecified, because a
  stub written against a real debugging session is worth more than one
  designed in advance. It arrives with the board that needs it.
- Everything in `PROPOSALS.md`, until its own trigger fires.
- Conformance testing, certification, and any statement about what an
  implementation must demonstrate to be Nyctos.
- Hardware ports beyond QEMU `virt`. Real boards bring firmware handover,
  interrupt controller variation, clock and PHY bring-up, and cache maintenance
  for non-coherent DMA. None of it is interesting until the system works on the
  emulator.
- Writing any of this up as teaching material, and the per-process isolation
  aim that section 1.4 of `design.md` describes. Both are long-term reasons
  the system is built the way it is, and both constrain the code today. The
  isolation aim in particular adds no work, because it is to be reached with
  mechanisms already designed; if it ever appears to need a new one, the aim
  is re-examined rather than the design extended. Neither is a deliverable.
