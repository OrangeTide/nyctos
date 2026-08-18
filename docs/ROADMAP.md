---
title: Nyctos
subtitle: Roadmap
revision: "2"
---

# Roadmap

This document says what exists, what is being built, and in what order.
`design.md` says what the system is meant to be. `PROPOSALS.md` holds ideas
that are not being built.

Nothing in `design.md` has been implemented or measured. Until milestone 1 is
reached, none of it has been tested against anything.

**There are no dates here.** There is no evidence on which to base one. Each
milestone carries a rough size, and sizes are relative to each other and to
nothing else.

---

## 1. What Exists Today

A cross-compiled kernel that boots under QEMU on aarch64 and riscv64,
initialises its object pools, reports them, and powers the machine off.

- Boot to a known state on both architectures.
- Static object pools with generation-checked identifiers. Stage 1 of
  milestone 1.
- Console output with a small formatter, `panic`, and a halt path that ends
  the machine rather than spinning.
- A project-owned libc and C runtime startup object.
- A system call trap path, an unprivileged-mode entry, a read-only RAM disk
  and a kernel-internal VFS, all left over from the banner demo that
  preceded this work.

This shares its build system and its architecture support with the design,
and almost nothing else. There is no scheduler, no IPC, no handle table, and
no MMU. The object pools exist but the objects themselves are still empty
structures, filled in by the stage that gives each type its behaviour.

**The banner demo is no longer on the boot path.** `/bin/motd`, the loader
that ran it, and the in-kernel filesystem behind it are still in the tree and
still compile, but nothing reaches them. They were how the tree proved a
system call worked before there was anything else; the vertical slice
replaces that path rather than extending it, so they are dead weight to be
removed rather than a demo to keep working.

**Four files contradict the design and are known to be temporary.**
`sys/kern/vfs.c`, `sys/kern/ramfs.c`, `sys/kern/kern_file.c` and
`sys/kern/kern_exec.c` put the filesystem and the program loader inside the
kernel. The design makes both ordinary processes. They should not be
mistaken for the intended shape.
## 2. How This Plan Is Organised

Four rules shape the ordering, and they are worth stating because they explain
choices that otherwise look arbitrary.

**Every milestone ends with something that runs.** Not a subsystem that
compiles, and not a layer waiting for the layer above it. A milestone that
cannot be demonstrated cannot be shown to be wrong.

**Each milestone names what it would falsify.** The design is a set of guesses
and some of them are bad. The point of building in this order is to reach the
bad ones early, so the milestone descriptions below say what result would count
as the design being wrong rather than the implementation being unfinished.

**The three provisional mechanisms are scheduled where they can actually be
tested.** Timelines are exercised in milestone 1, per-process namespaces in
milestone 5, and the submission engine in milestone 9. Each is placed at the
first point where the question about it can be answered, not at the first point
where it could be written.

**Teardown comes early, and measurement comes late.** Death paths are where the
subtle bugs live and are far easier to add to a system that works than to build
alongside one. Measurement needs a system and a real workload, and a number
taken before both exist is a number about nothing.

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

*Done when* the client prints the file's contents, and the three kernel
filesystem files are deleted.

*Would falsify.* Whether send, receive and reply with donation is as small as
assumed. **Whether timelines earn a separate object at all**, or whether
blocking in receive is sufficient and the object is ceremony. Whether
handle-relative naming with no ambient root is workable rather than merely
defensible. Whether a protocol carrying the offset in the message survives a
real client.

*Size.* Large. It is most of a kernel.

### M2. Death

**Cancellation, killing, and restart.**

The design leans on death everywhere: cohort recovery (18.2), the supervision
tree (24.3), and reclaiming server state (25). None of it has been exercised.
This comes second because teardown is much easier to reason about in a system
that already works.

*Needs.* Cancellation (10.5), process kill, completion timelines carrying exit
status (10.4), dead-owner notification, and a parent that waits on a child's
completion timeline and relaunches it.

*Done when* killing the driver mid-read releases the blocked client with an
owner-dead error rather than hanging it, and a supervisor restarts the driver
so that the client's next open succeeds.

*Would falsify.* Whether "the disappearance of the only party who could satisfy
a wait is itself an event" is sufficient in practice. Whether a restarted
server leaks per-connection records, which is the observation `PROPOSALS.md`
gates connection death notification on.

*Size.* Medium.

### M3. A System You Can Talk To

**Console input, a shell, and a few utilities as separate processes.**

The design claims a complete system rather than a kernel. That claim is
unsupported until there are utilities to point at. This milestone also produces
the workload every later milestone is tested against, which is why it comes
before the MMU rather than after it.

*Needs.* The read side of the UART as a driver process. A console service. A
shell that spawns a utility, waits on its completion timeline, and reports
status. Enough libc to write the utilities.

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

**A shared protection domain, and the recovery unit that comes with it.**

Cannot come before M4. Without protection domains a cohort is a naming
convention that demonstrates nothing.

*Needs.* Cohort membership fixed at build time. Zero-copy buffer handoff by
address within a cohort. Scatter-gather with lazy mapping (18.4). Cohort fault
recovery tearing down and restarting every member together (18.2). Cohort
scheduling nodes (11.5).

*Done when* a filesystem and a block driver in one cohort exchange a buffer as
an address rather than a grant, and a fault in either tears down and restarts
both.

*Would falsify.* Whether the fault-domain trade is tolerable once it actually
happens. A bug in a minor driver taking down the storage stack is easy to
accept on paper and may not be acceptable in practice.

*Size.* Medium.

### M7. Multicore

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

### M8. Real Time

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

### M9. The Submission Engine

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

## 4. Bring-Up Order Within M1

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

## 5. Ordering: What Is Forced and What Is Chosen

Worth separating, because the forced edges are not negotiable and the chosen
ones are.

**Forced.** M2 after M1, since there is nothing to tear down before that. M6
after M4, since a cohort without protection domains is a naming convention. M8
after M3, since measuring a system with no workload measures nothing. M9 after
M6, since issuing ops need a device that does DMA.

**Chosen, and reversible.** M3 before M4. The argument is that the MMU
milestone's real test is running the same software on both tiers, which needs
software. The counter-argument is that porting less code to Tier 2 is easier,
which would put M4 first. If M3 grows larger than expected, invert them.

**Independent.** M5 is pure userland and libc and blocks nothing, so it can
move anywhere after M3. M7 is almost entirely kernel and shares little with M5,
so the two can proceed in parallel if there is ever reason to.

---

## 6. What Would Change This Plan

Each of these is a plausible outcome, not a worry. The plan is arranged so that
they surface early and cheaply.

**Timelines do not earn their place (M1).** Remove the object, keep blocking
receive, and section 9 collapses. This is the cheapest thing on the list to
discover and the most disruptive to the document, since fences run through
interrupts, timers, exit and the submission engine.

**One thread per source is unaffordable (M3).** Wait-sets come forward from
`PROPOSALS.md`. The queue model, not the array model, and only after evaluating
whether a timeline enqueuing a coalesced pulse on a channel is sufficient.

**The interface differs between tiers (M4).** The thesis is wrong. Part I is
rewritten and the project needs a different reason to exist.

**Namespaces are unworkable (M5).** Either the mount table moves, or the design
concedes an ambient root. Sections 20 and 25 both change.

**Cohort faults are intolerable (M6).** Cohorts drop to a naming convention and
zero-copy goes through ordinary grants, at the cost the design was trying to
avoid.

**The submission engine does not pay (M9).** It is deleted. Section 23 already
defines the calling convention around a single operation for exactly this
reason, so nothing else moves.

---

## 7. Deliberately Not Scheduled

Named here so that they stay named rather than drifting in.

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
