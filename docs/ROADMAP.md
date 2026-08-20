---
title: Nyctos
subtitle: Roadmap
revision: "2"
---

# Roadmap

This document says what exists, what is being built, and in what order.
`design.md` says what the system is meant to be. `PROPOSALS.md` holds ideas
that are not being built.

Milestones 1 and 2 are done. What each settled is recorded in sections 4
and 5 below, and section 4 of `design.md` carries the design decisions
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

This shares its build system and its architecture support with the design,
and little else. There is no IPC and no MMU. Only the
real-time band of section 11.2 exists: the proportional tree below the
floor is unbuilt, so the floor sits at zero and the whole priority range is
real-time. Section 11.2 names that as the degenerate case, and it is. The object pools exist but the objects themselves are still empty
structures, filled in by the stage that gives each type its behaviour.

**The banner demo is gone.** `/bin/motd`, the host tool that built its RAM
disk, the in-kernel VFS and file table, and the loader that ran it have all
been deleted. They were how the tree proved a system call worked before
there was anything else, and the vertical slice replaces that path rather
than extending it. Nothing in the kernel contradicts the design now.

The C library and the userland linker scripts stay. Nothing links against
them yet, and they are kept building so that they do not rot before the
stage that spawns the first process needs them.

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

*Exit and join, which is not death.* A process that exits advances a
completion timeline, the handle stays valid afterwards and carries the
status, and a joiner waits on it (section 10.4). This is the normal path and
it belongs here, with the shell that is the first thing to need it. Nothing
is reaped, no slot is reused, and nothing is killed; M7 is still where all of
that lives.

*Done when* an interactive shell runs, spawns a program as a separate process,
and reports its exit status.

*Would falsify.* **Whether one thread per event source is affordable.** A shell
and a console service both want to wait on several things at once, and section
19.3 forces two threads and no shared state where one would have done. This is
the first real pressure on the decision to have no wait-set, and the first
honest evidence for or against the trigger written into `PROPOSALS.md`.

*Size.* Medium, and mostly userland.

### M4. The MMU

**Tier 2: the memory contract enforced rather than trusted.**

This is the thesis. Until Tier 2 exists, "the same interface with or without an
MMU" is a claim about one tier, which is not a claim at all. It comes after M3
so that there is real software to run on both.

*Needs.* Page tables and TLB maintenance in the port layer (section 27). The
move contract enforced by unmapping (6.1). The fault taxonomy and the fatal-by-
default rule (16). Faults as messages the faulting thread sends (15.3). A root
pager, demand-paged program text, and memory locking on the paging path (15).

*Done when* the M3 system runs on Tier 2 from the same source, and a program
that touches a granted buffer between send and reply faults on Tier 2 and does
not on Tier 0.

*Would falsify.* **The central claim of the whole design.** If the interface
has to differ between tiers, section 2.1 is wrong and Part I needs rewriting.
Separately, whether modelling a fault as IPC from the faulting thread really
reuses the reply path, the blocking invariant and donation without extending
any of them, which is the single most valuable borrowed idea in the document.

*Size.* Large.

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
