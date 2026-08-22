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
between them.

What does not exist: the MMU and everything resting on it, driver cohorts,
multicore, per-process namespaces, the submission engine, and anything at
all to do with death. Nothing is reaped, nothing is cancelled, and a
rendezvous carries no deadline, so a server that stops answering hangs its
client rather than failing it.

The one figure this milestone owed is what having no wait-set costs. It is
one thread, the console's second, at eight and a half kilobytes with both
its stacks. A system with N devices that must stay responsive while
serving pays N of them, which is linear and, at this size, cheap.
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

*Size.* Large, and larger than M3. Two of the stages are ports of things
that already work, which is the kind of work that produces no new
behaviour and takes the longest to be sure of.

### M5. Namespaces

**The mount table in libc, and namespace composition at spawn.**

*Needs.* Path resolution as ordinary library code walking a local table.
Handle-relative naming throughout (20.3). Named directory handles handed over
at launch and consumed like `envp` (20.4). Union directories.

*Done when* two processes see different filesystem views assembled from the
same underlying handles, with no kernel involvement and no ambient root
anywhere.

*Would falsify.* The second provisional mechanism. Whether a system with no
absolute paths is usable by ordinary programs, or whether the complexity simply
moves into libc and gets worse there. If this fails, section 20 changes
substantially and section 25 with it.

*Size.* Medium. Pure userland and libc, so it blocks nothing and can move.

### M6. Driver Cohorts

**A shared protection domain, and the zero-copy path it buys.**

Cannot come before M4. Without protection domains a cohort is a naming
convention that demonstrates nothing.

*Needs.* Cohort membership fixed at build time. Zero-copy buffer handoff by
address within a cohort. Scatter-gather with lazy mapping (18.4). Cohort
scheduling nodes (11.5).

*Deferred to M7.* Cohort fault recovery (18.2). Tearing down and restarting
every member together is the one mechanism in the plan that cannot be
demonstrated without death, and it is the reason M7 sits immediately after
this milestone rather than later.

*Done when* a filesystem and a block driver in one cohort exchange a buffer as
an address rather than a grant, with no mapping cost per layer, and a fault in
either halts the system with the recovery path stubbed and named.

*Would falsify.* Whether zero-copy within a shared domain is worth a shared
domain at all, which is the half of the trade this milestone can answer alone.
Whether the fault-domain cost is tolerable is answered in M7, when a fault
actually recovers rather than halting.

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
that cannot be demonstrated without death. M6 is therefore scheduled for
its zero-copy half only, and its recovery half is part of this milestone.
Everything earlier can carry a `TODO` in its exit path and lose nothing.

*Three things it needs that were not listed before.* Milestone 1 left them
out and they are each load-bearing.

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

*Exit and join already exist*, from M3. What is left is everything that
happens when the exit was not the program's own idea.

*Stages.*

1. **Reaping.** Return thread and process slots to their pools, and check
   that an identifier held across the reuse is refused rather than reaching
   the new occupant.
2. **Cancellation.** Section 10.5, prompt, releasing a thread from whatever
   it is blocked on without knowing which mechanism blocked it. This should
   be cheap: the hook that makes it possible was forced into existence by
   deadlines in stage 7 and already serves channels.
3. **Pulses.** Section 13.1, including the reserved system region and the
   overflow flag a receiver observes.
4. **Death notification.** Section 12.5 in both directions: a blocked
   client released with `SYS_ERR_OWNER_DEAD`, a server holding an
   unanswered request given a pulse, and its later reply refused as stale.
5. **Kill and restart.** A supervisor waiting on a child's completion
   timeline and relaunching it, with restart bounded and escalating per
   section 24.3.
6. **Cohort recovery.** The half of M6 that was deferred: a fault in one
   member tears down and restarts every member together (18.2). This is
   the case that forced the milestone to sit here rather than later.

*Done when* killing the filesystem server mid-read releases the blocked
client with an owner-dead error rather than hanging it, a supervisor
restarts the server, the client's next walk and read succeed against the
replacement, and a fault in one cohort member restarts the whole cohort.

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

*Size.* Large. The original estimate of medium predated knowing that
pulses, reaping and donation unwinding were all inside it, and cohort
recovery has since joined them.

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

## 6. Bring-Up Order Within M1

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

## 7. Ordering: What Is Forced and What Is Chosen

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

## 8. What Would Change This Plan

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

## 9. Deliberately Not Scheduled

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
