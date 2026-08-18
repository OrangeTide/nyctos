---
title: Nyctos
subtitle: Roadmap
revision: "1"
---

# Roadmap

This document says what exists, what is being built next, and in what order.
`design.md` says what the system is meant to be. `PROPOSALS.md` holds ideas that
are not being built yet.

The distinction matters because the design document describes a system that has
not been built. Nothing in `design.md` has been implemented or measured, and
until the milestone below is reached, none of it has been tested against
reality.

---

## 1. What Exists Today

A cross-compiled kernel that boots under QEMU on aarch64 and riscv64, drops to
unprivileged mode, and runs a single userland program that prints a banner and
reads a file.

- Boot to unprivileged mode on both architectures.
- A system call trap path in both directions.
- A project-owned libc and C runtime startup object.
- A read-only RAM disk built at compile time by a host tool.
- A kernel-internal VFS with one filesystem behind it.

This shares its build system and its architecture support with the design, and
almost nothing else. There is no scheduler, no IPC, no handle table, no
timelines, and no MMU. Userland is one process, and it is the only process.

**Three files contradict the design and are known to be temporary.**
`sys/kern/vfs.c`, `sys/kern/ramfs.c` and `sys/kern/kern_file.c` put the
filesystem inside the kernel. The design makes the filesystem a protocol served
by an ordinary process. They stay until milestone 1 replaces them, because
something has to serve files before any driver process exists, and they should
not be mistaken for the intended shape.

---

## 2. Milestone 1: The Vertical Slice

**One thread, one channel, one driver, one file read.**

The thinnest path that exercises handles, synchronous IPC and the filesystem
protocol together. It is chosen because it is the smallest thing that could
falsify the design rather than the largest thing that could demonstrate it.

A client process opens a file through a handle it was given at spawn, reads
from it, and prints the result. The file is served by a separate driver process
over the filesystem protocol. Nothing in the path is inside the kernel except
the message passing itself.

### 2.1 What This Requires

- A handle table with rights, backed by a static pool.
- Channels, and synchronous send, receive and reply.
- Priority inheritance on that path, built in from the start rather than
  retrofitted.
- Timelines, because the driver's interrupt path needs them immediately
  afterwards and building them later means rewriting the driver.
- Process spawn with handle inheritance.
- Enough scheduler to run two processes.
- The filesystem protocol operations the read path uses, and no others.

### 2.2 What This Deliberately Does Not Require

- The MMU. Milestone 1 is flat memory.
- Driver cohorts. Without protection domains a cohort is a naming convention.
- SMP. Single core.
- Demand paging, pagers, or memory locking.
- Namespaces and a mount table. The client is handed a directory handle at
  spawn, which is the minimal case the namespace design generalises.
- Introspection.
- The submission engine. Section 23 of `design.md` fixes a calling convention
  for one operation at a time, and the slice uses only that. The engine is the
  count-greater-than-one case and is deliberately built so that it can arrive
  later without disturbing anything.
- Any of `PROPOSALS.md`.

### 2.3 What It Would Falsify

The point of the slice is to find out which of these is wrong.

- Whether synchronous send, receive and reply with priority inheritance is
  actually as small as the design assumes.
- Whether timelines earn their place as a separate object, or whether blocking
  in receive is sufficient and the object is ceremony.
- Whether handle-relative naming with no ambient root is workable in practice
  or merely defensible on paper.
- Whether a filesystem protocol with the offset in the message, rather than in
  the server, survives a real client.

---

## 3. Bring-Up Order

Each stage produces something testable, and a stage that fails, fails visibly.
Stages 1 through 8 constitute milestone 1.

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

**Stage 9. Death.** Cancellation, process kill, completion timelines, dead-owner
notification. Deliberately last, because tearing down a system that already
works is far easier to reason about than building teardown alongside
construction.

**Two notes on order.** Timelines before IPC is the choice most likely to feel
wrong, since IPC seems more fundamental. It is not: interrupts and timers depend
on timelines and not on IPC, so timelines first gets the system responding to
hardware several stages earlier. And death last is deliberate, because teardown
paths are where the subtle bugs live.

---

## 4. After The Slice

In rough order, and not committed to. What the slice teaches should be allowed
to change this list.

**Console input and a shell.** The first genuinely interactive system, and the
first time more than two processes exist at once.

**More userland.** The design claims a complete system rather than a kernel, and
that claim is unsupported until there are utilities to point at.

**The MMU.** The second protection tier, and the first point at which the memory
contract is enforced rather than trusted. Everything about faults, pagers and
program loading arrives here.

**Driver cohorts.** Only meaningful once protection domains exist.

**Namespaces and the mount table in libc.** Generalising the directory handle
handed over at spawn into per-process namespace composition.

**SMP.** Partitioned scheduling with explicit migration, and cross-core priority
donation.

**Real-time.** Absolute deadlines, the tickless timer, and the response-time
properties the priority mechanisms were built for. The mechanisms are present
from the start; treating them as a real-time system is a later concern.

**The submission engine.** The novel part of the design, and the part most
worth finding out about. It needs issuing ops to be interesting, which needs a
DMA-capable device, so it wants real driver work underneath it before it means
anything. Building it early against blocking ops alone would exercise the
machinery without testing the idea.

---

## 5. Out Of Scope

Named here so that they stay named rather than drifting back in.

- Everything in `PROPOSALS.md`.
- Conformance testing, certification, and any language about what an
  implementation must demonstrate to be Nyctos.
- Published latency measurements and worst-case execution time analysis. These
  become interesting when there is a system to measure, and claiming them now
  would be claiming someone else's results.
- Hardware ports beyond QEMU `virt`.
- Writing any of this up as teaching material. That is the long-term reason the
  system is built the way it is, and it constrains the code today: small
  functions, a complete system rather than a kernel alone, and a refusal of
  scope creep. It is not a deliverable, and it will not be one until the system
  works.
