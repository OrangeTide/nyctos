---
title: Nyctos
subtitle: Roadmap
revision: "2"
---

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

What does not exist: multicore, the submission engine, and anything at
all to do with death beyond reaping. Pool slots come back, but nothing
is cancelled, no address space is ever torn down, no cohort is ever
restarted, and a rendezvous still carries no deadline.

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

**2. Cancellation.** Section 10.5: releasing a thread from whatever it is
blocked on, without the caller knowing which mechanism blocked it.

- The `t_unwait` hook already exists, forced into being by deadlines in
  M1 stage 7, and already serves channels and timelines.
- A cancelled sender that has already lent a buffer gets it back before
  it runs again. A thread released with its pages still in another
  process faults on its own memory, which is worse than the hang it was
  cancelled out of.

*Check.* A thread blocked in each of the ways this system can block, in
turn, cancelled and observed to run again with `SYS_ERR_INTERRUPTED`:
receive, reply, a timeline wait, and a send that queued. A reply-blocked
client cancelled mid-call has its buffer back, checked by writing to it
immediately afterwards, and the buffer remapping count says the pages
moved rather than being abandoned.

**3. Pulses.** Section 13.1: a fixed-size queued notification with a
region reserved for the system's own.

- The reservation is correctness. A flood of user pulses that displaced a
  death notice would leak a dead client's resources for good.
- The overflow flag a receiver observes, so that a receiver knows it
  missed something rather than inferring it.

*Check.* A queue filled with user pulses still delivers a system pulse,
and the sender of the one that did not fit is told. Removing the
reservation makes the death notice the thing that is dropped, which is
the check for why the reservation exists rather than for whether the
queue works.

**4. Death notification.** Section 12.5 in both directions.

- A client blocked on a dead server is released with
  `SYS_ERR_OWNER_DEAD`, because it is not at a receive point and cannot
  collect anything.
- A server holding an unanswered request from a dead client gets a
  `SYS_PULSE_DEAD_OWNER`, because it is at one, and its later reply is
  refused as stale rather than delivered to whoever holds the id now.

*Check.* The filesystem server killed mid-read releases its client with
`SYS_ERR_OWNER_DEAD` rather than hanging it, and the client's memory is
its own again. A client killed mid-call leaves the server able to
discover it, finish, and have its reply refused; the connection record
the server was holding goes back to the pool, which is the observation
`PROPOSALS.md` gates connection death notification on. Both directions
have to be tried with a buffer outstanding, because that is the case M4
and M6 made possible and neither had to unwind.

**5. Kill and restart.** A supervisor waiting on a child's completion
timeline and relaunching it, bounded and escalating per section 24.3.

- The kernel holds no restart policy, per section 18.3. What is built
  here is the mechanism a supervisor uses and one supervisor that uses
  it.
- Escalation is what a bound is for: a child that dies immediately every
  time is a configuration mistake, and a supervisor that relaunched it
  for ever would hide one.

*Check.* The filesystem server is killed, restarted, and the client's
next walk and read succeed against the replacement, with the client
never knowing which one answered. A server that dies on every start is
given up on rather than restarted for ever, and the give-up is reported
rather than silent.

**6. Cohort recovery.** The half of M6 that was deferred, and the case
that forced this milestone to sit here.

- A fault in any member tears down every member together and a
  supervisor restarts them, per 18.2 and 18.3.
- The member table's pointers are cleared as the members go, so a
  restart can take the places back.
- The slots leave a live address space, which is where M6's TLB
  insurance is claimed.

*Check.* The halt M6 prints is replaced by a teardown and a restart, and
the storage stack serves a file again afterwards through processes that
are not the ones that started. A client blocked on the dying filesystem
is released rather than hung. And the peer that did not fault is gone
too, which is the whole content of 18.2 and the thing worth checking
explicitly: a survivor left running would be the unsound recovery the
section forbids.

*Done when* killing the filesystem server mid-read releases the blocked
client with an owner-dead error rather than hanging it, a supervisor
restarts the server, the client's next walk and read succeed against the
replacement, a fault in one cohort member restarts the whole cohort, and
the resource report's high-water reflects what is alive rather than what
has ever run.

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

*Needs.* Per-core ready queues with no global scheduler lock (11.4). Explicit
migration rather than automatic balancing. Cross-core priority donation via
inter-processor interrupt.

*Done when* threads run pinned across cores, a donation chain crosses a core
boundary and is honoured, and no scheduling decision takes a shared lock.

*Would falsify.* Whether partitioned scheduling with no load balancing is
usable, or whether the absence of automatic placement is intolerable in
practice. The design chose it for analysability, which is a real-time argument;
if real time is one subsystem rather than the thesis, the argument is weaker
than it was when it was made.

*Size.* Medium to large.

### M9. Real Time

**Deadlines, the tickless timer, and the first measurements.**

The priority mechanisms are present from M1. What is missing is treating the
result as a real-time system, which means measuring it. This is where "no
figures are claimed" stops being true.

*Needs.* Absolute deadlines everywhere a timeout appears (14.3, 14.4). A
one-shot timer reprogrammed to the earliest pending event, with no tick (14.1).
A bound on the donation chain. Introspection sufficient to read pool
high-water marks (22).

*Done when* interrupt latency, IPC round trip and donation latency are measured
on both tiers and published together with the configuration that produced them,
and the Nth activation of a periodic timer lands at `first + N * period`
despite a deliberately stalled task.

*Would falsify.* Every latency intuition in the design, and the per-tier
interrupt latency argument in 13.3 specifically. Also whether a tickless timer
is affordable on a part where reprogramming is a slow peripheral write.

*Size.* Medium. Mostly instrumentation and discipline rather than new
mechanism.

### M10. The Submission Engine

**Section 17: the count exceeds one.**

Deliberately last. The engine needs issuing ops to be interesting, issuing ops
need a DMA-capable device, and that wants the driver work in M6 underneath it.
Building it earlier against blocking ops alone would exercise the machinery
without testing the idea.

*Needs.* Entry arrays with per-entry status. Typed registers with tags.
`LINK` chaining with abort semantics. Issuing ops returning timelines. Prepared
programs validated once and executed many times. Preemption points at entry
boundaries.

*Done when* one batch issues several transfers, collects their timelines and
waits on each, with three transfers in flight, the submitting thread's own
context executing all of it, and donation unbroken throughout. And a prepared
program re-executes without revalidation.

*Would falsify.* The most novel idea in the design. Whether expressing
dependencies as data actually beats a few more system calls, and whether the
branchless form stays sufficient once real code uses it. If it does not pay,
it is removed, and section 23 is already arranged so that removing it costs
nothing else.

*Size.* Medium.

---

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

## 10. Bring-Up Order Within M1

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

## 11. Ordering: What Is Forced and What Is Chosen

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

## 12. What Would Change This Plan

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

---

## 13. Deliberately Not Scheduled

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
