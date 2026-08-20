---
title: Nyctos
subtitle: Design
tagline: |
  A microkernel whose interface does not depend on
  whether the hardware has a memory management unit
revision: "5"
logo: logo.svg
---

# Part I — Overview

This part describes what Nyctos is and why it is arranged the way it is.

---

## 1. Introduction

### 1.1 The Problem

Small systems are built twice.

Below a certain size you get a small kernel with no memory protection: one address space, drivers linked into the application, and a scheduler measured in kilobytes. Above it you get a general-purpose kernel with processes, virtual memory and isolation. The line between them is drawn by hardware, by whether the part has a memory management unit, and software does not cross it.

A product family that spans the line maintains two systems, and moving a component from one to the other is a rewrite rather than a port. The decision about which system a component belongs to is therefore made early, on incomplete information, and is expensive to revisit.

The usual response is to treat the small system as a cut-down version of the large one, sharing a name and a build system and very little else. The application-level interface differs, so the code differs, so the two diverge until only the branding is common.

### 1.2 What Nyctos Is

A microkernel whose application-level interface does not depend on whether the hardware has a memory management unit.

Flat memory and a full MMU are treated as two levels of *enforcement* of one set of rules, not as two systems. A program written against the interface runs on either, and what changes is whether the hardware can catch it breaking the rules. The rule that makes this work is a rule about how to choose rules, and it is stated in section 2.1.

Everything else follows from holding that commitment while keeping the kernel small enough to read.

### 1.3 What Is Claimed, and What Is Not

Part of this design has been built and part has not, and which is which
governs how the rest should be read.

**Roughly half the kernel exists**, and one thing has been built end to
end. A client process, handed a single connection when it was created,
reaches a file through it and reads the file from a server that is a
different process. Static pools, threads, the scheduler, handle tables,
the operation entry point, timelines, interrupts, timers, message passing
and process creation all run on two architectures from one source.

What does not exist is the memory management unit and everything resting
on it, driver cohorts, multicore, per-process namespaces, and the
submission engine. No user code has run, so the trap is still unexercised
from the outside. `ROADMAP.md` says where the line is and what building
the first half corrected.

**Nothing has been measured.** Every statement about cost, size or latency
is still an intention. The one number obtained so far is a footprint rather
than a speed, and it is reported in the roadmap because it is a fact about
a configuration and not about the design.

**Two properties are structural rather than empirical.** Kernel memory is
statically bounded: there is no kernel heap at either tier, every object
comes from a build-time pool, and consumption is therefore a property of
the configuration rather than of application behaviour. And a submitted
batch has a statically bounded execution time inside the kernel, because
the operation array is linear, finite, and carries no branches; its
wall-clock completion is not bounded, because some operations block, and
the operations that may block are enumerated in one place and nowhere else.
Neither claim waits on an implementation, and neither says anything about
how long anything takes.

**Of the three mechanisms marked provisional, one has been settled.**
Timelines earn a separate object and do not deserve the exclusive claim
section 9 made for them, which that section now says. Per-process
namespaces (section 20) and the submission engine (section 17) are exactly
as unproven as when they were written.

**What building it has already corrected is marked where it happened.**
Sections 9.2, 9.3, 11.6, 13.2 and 14.1 carry notes of that kind: an
ambiguity that only became visible when something had to resolve it, a
requirement that was implied rather than stated, or a sentence that was
wrong. Those notes are the most useful part of this document, because they
are the only parts of it that have been tested.

---

### 1.4 Where This Is Going

Two long-term aims shape the design without appearing in it as features.
Neither is scheduled, and neither adds a mechanism.

**A system that can be read end to end**, kernel and userland utilities
alike, in the tradition of the Lions commentary and xv6. This is why
functions are small, why the whole system matters rather than the kernel
alone, and why scope creep is refused rather than accommodated.

**Per-process environment isolation**, in the spirit of Qubes OS but at a
fraction of the weight. This one is worth spelling out, because it reads
like a feature request and is not one. Qubes separates work into domains and
makes every crossing between them explicit and reviewable. The mechanisms
that produce that property are already here, and are here for other reasons:

- No ambient authority. A process reaches exactly what its parent placed in
  its handle table, and the kernel grants nothing on request (section 7).
- No global filesystem root and no absolute paths, so a process's view of
  the world is composed for it at launch rather than discovered by it
  (sections 20, 20.4).
- The memory move contract, so a buffer handed across a boundary is not
  shared but transferred, enforced by hardware at Tier 2 (section 6.1).
- Cohorts, which make a protection and failure domain an explicit build-time
  decision rather than an accident of linkage (section 18).
- Static configuration, so which processes exist and what each may reach is
  fixed when the system is built and can be reviewed as data (section 24.2).
- Introspection that cannot discover, so a process is observable only by
  something already holding a handle to it (section 2.9).

**What this aim does not promise.** There is no hypervisor here, no virtual
machines, no GUI or device isolation behind an IOMMU, and no disposable
domains. Nyctos isolates processes, not machines, and a domain is a process
or a cohort rather than a guest operating system. Anything wanting the
stronger property should use a system built for it.

The constraint on this aim is the useful part: **it is to be reached with
what is already designed.** If it ever seems to require a mechanism that is
not here, that is a reason to re-examine the aim, not a licence to extend
the design.

---

## 2. The Design

Six ideas, and the cost of holding them.

### 2.1 One Contract, Two Enforcements

The tiers differ in what they can enforce and never in what things mean. The rule that makes this work is a rule about how to choose rules:

> Choose the contract that the stronger tier enforces in hardware and the weaker tier merely trusts.

Applied to memory, this gives move semantics: when a buffer is sent, the sender loses access to it until reply. The tier with an MMU enforces that by unmapping. The tier with neither does not enforce it at all, and a program that violates the rule will not fault.

A correct program therefore behaves identically on both, and the only thing the weaker tier loses is the ability to catch an incorrect one. That has a practical consequence worth more than it first appears: the same source can be developed on hardware that enforces and deployed on hardware that does not, so a class of bug that is invisible on the target is findable off it.

The alternative, a contract permitting shared access with a convention against touching, is unenforceable on every tier and is therefore not a contract at all.

### 2.2 Everything Crosses the Same Boundary

A page fault, a driver completion, a timer expiry and an application request all become either a message or a timeline advance. There is no privileged path that bypasses the general mechanisms.

This is the property that keeps the kernel small enough to hold in mind. A reader who understands message passing and timelines understands the whole system rather than a part of it, and a mechanism that would have needed its own delivery path instead reuses one that already exists and is already debugged.

### 2.3 One Thing To Wait On

A thread waits on exactly one kind of object, whatever it is waiting for.

Driver completion, interrupt delivery, timer expiry and thread exit are all advances of a monotonic counter called a timeline. An interrupt handler advances a timeline and returns; a timer advances one on schedule; a thread's exit advances its own. A driver blocked on a transfer and a task blocked on its next activation are doing the same thing.

The payoff is that the blocking relation in the system is a **chain rather than a graph**. A thread blocks on one object at a time, so priority donation walks in one direction with no branching, deadlock detection is that same walk with a bound on it, and cancellation can release a thread from whatever it is waiting on without knowing which mechanism blocked it.

**This is the design's most provisional idea.** The argument above is a good one and it has not been tested. A driver that blocks in receive and never touches a timeline is a real possibility, in which case the object is ceremony and should go. The first implementation is arranged to find out.

### 2.4 Priority Follows Work

A request executes at the priority of whoever is waiting for it.

A server handling a request runs at its client's priority for the duration. A thread taking a page fault has that fault serviced at *its* priority, because the fault is modelled as a message the faulting thread sends rather than an event delivered to a pager. The general shape is that wherever a mechanism could execute work somewhere other than where it was requested, it does not, and concurrency comes from hardware rather than from kernel threads.

This is the property a real-time system needs, and it is why several mechanisms here look the way they do rather than the way they usually look. It is not the reason the system exists. Priority inheritance is present in the message-passing path from the beginning, because retrofitting it into a working system is harder than building with it, but the system is not presented as a real-time operating system and makes no timing guarantees. Section 21 collects the priority mechanisms in one place.

### 2.5 Dependencies as Data

A thread can submit an ordered batch of operations in one call. The submission language has no branches, no loops and no bytecode. It is a flat array of entries.

That is sufficient because ordering constraints are expressed as **fences rather than control flow**. An operation that would have needed a conditional instead waits on a timeline. The dependency becomes data, and the language that would have needed to express it does not.

Removing branches removes the need for an in-kernel verifier, which would otherwise be the largest security-critical component in the system and catastrophic to get wrong on a tier with no hardware protection. It also makes the cost of a submission the sum of its entries, computable without running it.

This is the newest idea here and the one most likely to be wrong. It is kept strictly separable from the rest for that reason: section 23 defines a calling convention for a single operation, and section 17 extends it to many. Nothing else in the design requires the extension to exist.

### 2.6 An Explicit Trade: Driver Cohorts

Related drivers can be grouped into a cohort that shares one protection domain, so that a filesystem and the block driver beneath it exchange scatter-gather lists without crossing a boundary.

The cost is stated rather than minimised: **the cohort is the unit of recovery.** A member that faults may have corrupted any peer arbitrarily, and nothing can establish that it did not, so all of them are torn down and restarted together. A bug in a minor driver takes down the storage stack.

That is the whole content of the feature. It is offered because zero-copy I/O paths are worth real throughput, and it is bounded by making cohort membership a build-time decision that deserves attention rather than a convenient way to group related code. It is also meaningless without protection domains, so it does nothing at all on the flat-memory tier.

### 2.7 Statically Configured, Everywhere

Object counts are fixed when the system is built, on the application processor as much as on the microcontroller. The tiers differ in protection hardware, not in how dynamic they are.

This removes a large class of error paths, since allocation failure moves from being reachable on nearly every call to being a configuration error visible at integration time, and it makes the kernel's memory footprint computable from build settings. It costs the ability to grow: a system that exhausts a pool is reconfigured and rebuilt.

### 2.8 The Shape of the System

The kernel provides thread contexts, a scheduler, synchronous message passing, timelines, and a handle table. Nothing else is inside it. File systems, network stacks, device drivers, pagers, loaders and supervisors are ordinary processes, and the filesystem namespace is assembled in the C library.

**Authority flows downward from a single root.** The kernel grants nothing on request. A process reaches only what its parent placed in its handle table, and the root process holds everything at boot. There is no ambient authority anywhere, including no global filesystem root.

**The system is meant to be a set of replaceable parts.** Nyctos is intended to arrive as a working system and not only as a kernel. The loader, the pager, the supervisor, the filesystem, the drivers and the C library that assembles the namespace are named roles with defined interfaces, and each is an ordinary process reaching the others through message passing and timelines and through nothing else. Substituting one should keep the rest, because the parts depend on the interfaces between them and not on each other's internals. This is why the policy inside those roles is left open, restart policy in section 19.3 being the clearest case.

```{=typst}
#figure(include "fig/architecture.typ", caption: [Nyctos system architecture.])
```

### 2.9 What This Costs

Every item here is a consequence of something above, and a reader deciding whether the design suits them should weigh these first.

**The total writable working set must fit in RAM.** Mapping exists to load read-only program and library text. There is no copy-on-write and no swap, because without dirty tracking an evicted writable page cannot be safely written back. This is the wrong answer for anything that wants to overcommit memory.

**A fault in one cohort member destroys the cohort.** Discussed above. The alternative is unsound rather than merely slower.

**Exhausting a pool means a rebuild.** There is no runtime growth to fall back on.

**A server waiting on several unrelated sources needs a thread for each.** There is no multi-wait primitive. On the flat-memory tier, where RAM is measured in kilobytes, a stack per event source is a real cost.

**POSIX software needs work.** There is no `mmap` in the usual sense, no ambient filesystem root, and no global namespace. A compatibility layer can supply the familiar shapes over the native interface, and it is a layer rather than a thin shim.

**The weaker tier cannot catch its own violations.** The flat-memory tier trusts the memory contract entirely. A program that breaks it there produces no fault and no diagnostic, which is why the same program should be exercised on a tier that enforces.

**Replaceable parts mean interfaces that are expensive to change.** A component can only be substituted against an interface stable enough for someone else to have written to. Those boundaries must be fixed early and then held, and moving one afterwards costs a coordinated change. That cost is the price of the arrangement in section 2.8 rather than an accident of it.

**Introspection cannot discover.** A debugger can inspect only what it holds a handle to, so inspection follows the supervision tree. There is no way to attach to an arbitrary running process that was not arranged to be observable. This is the capability model applied consistently, and it is inconvenient exactly when debugging is hardest.
---

## 3. Inspiration, References, and Credits

Almost nothing in this design is new. It is an attempt to select well among existing mechanisms and to make the resulting combination coherent. This section records where each idea came from, so that credit sits with the people who did the work and so that a reader who disagrees with a choice can go read the original argument.

Two contributions are load-bearing enough to name up front. **Jochen Liedtke's** insistence that a microkernel should contain only what cannot be implemented outside it, and his demonstration that IPC performance is a design problem rather than an implementation problem, is why this design has a small kernel and puts fault handling and namespaces outside it. **Andrew Valencia's VSTa** is the least-cited system here and one of the most useful; it had already resolved several questions this design was still treating as open, and section 20 exists because of it.

### 3.1 Direct Lineage

**QNX Neutrino** (Dan Hildebrand, Quantum Software Systems) is the source of the overall shape: synchronous send/receive/reply as the single communication primitive, pulses as a separate non-blocking notification, the split between channel creation and connection, and priority inheritance applied to message passing. The observation that a message-passing kernel can be small enough to analyze while supporting a full POSIX personality above it is QNX's.

- Hildebrand, [*An Architectural Overview of QNX*](https://cseweb.ucsd.edu/~voelker/cse221/papers/qnx-paper92.pdf), USENIX Workshop on Micro-kernels and Other Kernel Architectures, 1992.
- [QNX Neutrino system architecture](https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/about.html)

**Plan 9 from Bell Labs** (Rob Pike, Dave Presotto, Ken Thompson, Howard Trickey, Phil Winterbottom) contributes per-process namespace composition, union directories, and the discipline of exposing services through a file-like interface. The insight this design leans on hardest is that a name is a convenience and not an authority, which is what makes section 20 safe. **Inferno** contributes the narrower reading of what mapping is for, adopted in section 15.1.

- Pike et al., [*The Use of Name Spaces in Plan 9*](https://9p.io/sys/doc/names.pdf), ACM SIGOPS European Workshop, 1992.
- [Plan 9 documents, volume 2](https://9p.io/sys/doc/index.html)

**L4 and L4Ka::Pistachio** (Jochen Liedtke and successors) contribute the page fault mechanism in section 15.3, the single most valuable idea borrowed. Modelling a fault as a message sent *by the faulting thread* rather than as an upcall into it is what lets demand paging reuse the reply path, the blocking invariant, and priority inheritance without extending any of them. The kernel interface page is the ancestor of the system page, and the map and grant distinction in L4 is the ancestor of the move contract.

- Liedtke, [*Improving IPC by Kernel Design*](https://cgi.cse.unsw.edu.au/~cs9242/18/papers/Liedtke_93.pdf), SOSP, 1993.
- Liedtke, [*On μ-Kernel Construction*](https://cgi.cse.unsw.edu.au/~cs9242/18/papers/Liedtke_95.pdf), SOSP, 1995.
- Elphinstone and Heiser, [*From L3 to seL4: What Have We Learnt in 20 Years of L4 Microkernels?*](https://sigops.org/s/conferences/sosp/2013/papers/p133-elphinstone.pdf), SOSP, 2013.
- [L4Re IPC concepts](https://l4re.org/doc/l4re_concepts_ipc.html)

**seL4** (Gerwin Klein, Kevin Elphinstone, Gernot Heiser, and the Trustworthy Systems group) contributes the memory model in section 8. The property adopted is not retype itself but its consequence, that kernel memory becomes a property of configuration rather than workload. seL4 is also the reason this document states interrupt latency per tier rather than in general: its WCET work quantified the cost of the interrupts-disabled approach honestly, including conceding what it cannot do, and that honesty is worth imitating even where no measurement is being claimed.

- Klein et al., [*seL4: Formal Verification of an OS Kernel*](https://plsyssec.github.io/cse227-spring25/papers/sel4.pdf), SOSP, 2009.
- Blackham et al., [*Timing Analysis of a Protected Operating System Kernel*](https://abhikrc.com/pdf/rtss11-os.pdf), RTSS, 2011.
- Blackham, Shi and Heiser, [*Improving Interrupt Response Time in a Verifiable Protected Microkernel*](https://dl.acm.org/doi/10.1145/2168836.2168869), EuroSys, 2012.
- [WCET analysis of seL4](https://trustworthy.systems/projects/OLD/wcet) and [untyped memory tutorial](https://docs.sel4.systems/Tutorials/untyped.html)

**VSTa** (Andrew Valencia) is the direct source of five decisions. The mount table belongs in the C library (section 20). A scheduling tree in which related threads share a node gives much of the benefit of gang scheduling without gang machinery (section 11.5). Processes should declare their own reentrancy discipline (section 10.3). Anything on the paging path must be memory-locked, or a storage driver deadlocks demand-paging itself from the swap device it serves (section 15.4). And system state should be exposed as readable text (section 22). VSTa's retrospective also independently justifies the small-message inline fast path, having found its page-mapping path overkill for small buffers.

- Valencia, [*An Overview of the VSTa Microkernel*](http://www.vsta.org/documentation/papers/microkernel.html)
- [VSTa source](https://github.com/JamesLinus/vsta)

**Fuchsia and Zircon** (Google) contribute the handle model: per-process handle tables, per-handle rights with attenuation-only duplication, and transfer by move inside messages. The arrangement in section 10.4, where an exited object's handle stays valid and carries its exit status, is Zircon's and avoids inventing a zombie concept. Zircon's event pairs, asserting peer-closed when the far side's handles are all closed, are generalized into timeline abandonment. Zircon also contributes a negative result discussed in section 3.5.

- [Zircon kernel concepts](https://fuchsia.dev/fuchsia-src/concepts/kernel/concepts) and [signals](https://fuchsia.dev/fuchsia-src/concepts/kernel/signals)
- [Zircon pager object](https://fuchsia.dev/fuchsia-src/reference/kernel_objects/pager)
- [RFC-0226: Zircon Pager Writeback](https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0226_zircon_pager_writeback), the reference for the writeback item in `PROPOSALS.md`.

**LK, the Little Kernel** (Travis Geiselbrecht) is the model for Tier 0. The bitmap ready queue with count-leading-zeros lookup, wait queues as the single blocking primitive, and the proposition that a useful preemptive kernel fits in a few thousand lines on a part with no MMU all come from LK. It is also the reference for the interrupts-enabled, fine-grained-locking alternative adopted at Tier 2.

- [littlekernel/lk](https://github.com/littlekernel/lk) and its [kernel API documentation](https://github.com/littlekernel/lk/wiki/Kernel-APIs-And-Primitives)

**Erlang/OTP** (Joe Armstrong, Robert Virding, Mike Williams) contributes the supervision model of section 24.3. The parts adopted are restart intensity, a bound on restarts within a window, and escalation to the parent supervisor when that bound is exceeded, rather than the endless respawn that Unix `init` established as the default and that every later Unix retrofitted a limit onto. The distinction between a restart group and the protection domain it sits in is also OTP's, arrived at there as the difference between `one_for_one` and `rest_for_one`.

- Armstrong, [*Making Reliable Distributed Systems in the Presence of Software Errors*](https://erlang.org/download/armstrong_thesis_2003.pdf), PhD thesis, 2003.

**ARINC 653** is the precedent for per-child recovery policy as build-time data (section 24.2). Its health monitoring tables map an error, at process, partition or module level, onto a fixed recovery action chosen when the system is integrated, with no runtime policy engine anywhere. That is the same answer this design reaches from the static configuration constraint, and it is worth knowing that a certified avionics standard got there first.

**Genode** (Norman Feske) is the reference for a root process that routes services between children it describes declaratively, and for the observation that boot modules are best served by the smallest possible read-only service rather than understood by the kernel. Section 24.4 takes the second and declines the first, since a static system can compute its routing at build time rather than describing it in a configuration the root process must parse.

- Feske, [*Genode Operating System Framework*](https://genode.org/documentation/genode-foundations/index), foundations manual.

### 3.2 Where Each Mechanism Came From

| Mechanism | Section | Source |
| ---------------------------------------- | --------- | ------------------------ |
| Send/receive/reply rendezvous | 12.1 | QNX |
| Pulses as a separate primitive | 13.1 | QNX |
| Channel and connection split | 12.1 | QNX, VSTa |
| Descriptor as a connection to the server | 27.2 | QNX resource managers |
| Priority inheritance on IPC | 12.3 | QNX, and Sha et al. for the protocol |
| Page fault as IPC from the faulting thread | 15.3 | L4 |
| System page mapped read-only per process | 6.2 | L4 kernel interface page |
| Move semantics on transfer | 6.1, 7.4 | L4 grant, Zircon handle transfer |
| No kernel heap, statically bounded objects | 8 | seL4 |
| BootInfo-shaped handoff to a root task | 8.1 | seL4 |
| Preemption points as the latency lever | 11.3, 17.7 | seL4 |
| Submission chaining with LINK | 17.5 | io_uring |
| Fences as submitted dependencies | 17.2 | GPU pushbuffer practice, Android sync_file |
| Mount table in libc | 20 | VSTa |
| Cohort scheduling nodes | 11.5 | VSTa scheduling tree |
| Per-process reentrancy modes | 10.3 | VSTa |
| Memory locking on the paging path | 15.4 | VSTa |
| Lazy on-demand mapping of sg segments | 18.4 | VSTa |
| Text attribute introspection | 22 | VSTa stat, Linux sysfs |
| Small-message inline fast path | 12.4 | VSTa retrospective, L4 register IPC |
| Read-only mapping for shared libraries | 15.1 | Plan 9, Inferno |
| Filesystem access as a protocol, not a layer | 27 | Plan 9, 9P |
| Per-process namespace, union directories | 20 | Plan 9 |
| Per-handle rights, attenuation only | 7.2 | Zircon |
| Handle stays valid after exit, carries status | 10.4 | Zircon |
| Peer-closed as timeline abandonment | 9.3 | Zircon event pairs |
| Bitmap ready queue, CLZ lookup | 11.2 | LK |
| Interrupts enabled with fine-grained locks | 13.3 | LK, Zircon |
| Monotonic timeline over binary fence | 9.1 | Vulkan timeline semaphores |
| Owner-in-word PI lock protocol | 21.1 | Linux futex |
| Partitioned scheduling with migration | 11.4 | Real-time scheduling literature, see 3.3 |
| Recovery action per child, fixed at build | 26.2 | ARINC 653 health monitoring |
| Restart intensity and escalation to the parent | 26.3 | Erlang/OTP supervisors |
| Boot modules behind a minimal read-only service | 26.4 | Genode |

### 3.3 Foundational Work

**Priority inheritance.** The protocol in sections 11.5 and 20 is the basic priority inheritance protocol. Sha, Rajkumar and Lehoczky formalized it, proved the blocking bounds, and introduced the priority ceiling protocol as the stronger alternative. `SYS_PRIO_CHAIN_MAX` exists because transitive inheritance without a bound has no blocking guarantee.

- Sha, Rajkumar and Lehoczky, [*Priority Inheritance Protocols: An Approach to Real-Time Synchronization*](https://www.math.unipd.it/~tullio/RTS/2009/SRL-1990.pdf), IEEE Transactions on Computers 39(9), 1990.

**Multiprocessor real-time scheduling.** The choice of partitioned over global scheduling in section 11.4 is made on analyzability grounds, and the tradeoff is not this design's discovery. Davis and Burns survey the whole space, including why partitioned analysis decomposes per core.

- Davis and Burns, [*A Survey of Hard Real-Time Scheduling for Multiprocessor Systems*](https://cpen432.github.io/resources/P7-multiprocessor-survey.pdf), ACM Computing Surveys 43(4), 2011.

**Gang scheduling.** Ousterhout introduced co-scheduling of related threads and the analysis of why partial scheduling of a coupled set wastes work.

- Ousterhout, [*Scheduling Techniques for Concurrent Systems*](https://www.semanticscholar.org/paper/Scheduling-Techniques-for-Concurrent-Systems-Ousterhout/014f2fde05dc7c6f0b71c13f98e4a0d0c4969e25), ICDCS, 1982.

**Fast userspace locking.** The futex design, and specifically the convention that an uncontended lock never enters the kernel, underlies section 21.1. The priority-inheriting variant with the owner encoded in the futex word is Linux's.

- Franke, Russell and Kirkwood, [*Fuss, Futexes and Furwocks: Fast Userlevel Locking in Linux*](https://www.kernel.org/doc/ols/2002/ols2002-pages-479-495.pdf), Ottawa Linux Symposium, 2002.
- [futex(2) manual page](https://man7.org/linux/man-pages/man2/futex.2.html), for `FUTEX_LOCK_PI` and the robust-list protocol.

**Capability systems.** The handle model is a capability system in the Dennis and Van Horn sense, where possession of an unforgeable reference is itself the authority and there is no ambient permission to check against. seL4 and Zircon are the practical references.

### 3.4 Graphics and I/O Lineage

The submission engine and timelines owe more to graphics and storage stacks than to operating systems research.

**Vulkan timeline semaphores** are the direct model for section 9.1. The properties adopted are the monotonic 64-bit counter, one object per pipeline rather than one per transfer, and the legality of waiting on a value before anything has promised to signal it, which removes the wait-before-signal race binary fences impose.

- [Vulkan Timeline Semaphores](https://www.khronos.org/blog/vulkan-timeline-semaphores), Khronos Group.
- [Timeline semaphore sample and rationale](https://docs.vulkan.org/samples/latest/samples/extensions/timeline_semaphore/README.html)

**Android `sync_file` and the Linux dma-fence framework** established fences as transferable, refcounted, error-carrying completion objects shared between drivers and userspace. The error-propagation rule in section 9.3 is theirs. Merge is examined in section 3.5 and not adopted.

- [Linux sync_file documentation](https://docs.kernel.org/driver-api/sync_file.html)
- [Linux dma-buf and dma-fence documentation](https://docs.kernel.org/driver-api/dma-buf.html)

**GPU pushbuffers** are the structural model for section 17: a linear, branchless, in-order instruction stream in which ordering constraints are expressed as fence waits rather than control flow.

**io_uring** contributes chain semantics. Its execution model is deliberately inverted here, for the reason in section 3.5.

- Axboe, [*Efficient I/O with io_uring*](https://web.archive.org/web/2023/https://kernel.dk/io_uring.pdf). The original at `kernel.dk` is no longer served; this is the Internet Archive copy.
- [The rapid growth of io_uring](https://lwn.net/Articles/810414/), LWN.
- [Lord of the io_uring](https://unixism.net/loti/what_is_io_uring.html), for `IOSQE_IO_LINK` and `IOSQE_IO_DRAIN` specifically.

### 3.5 Examined and Not Adopted

Recording rejected designs is as useful as recording adopted ones, because the reasons constrain future revisions. In every case the original is a reasonable design for its own context, and the mismatch is with this system's requirements rather than with the work itself.

**Mach's external pager.** Mach established that memory objects can be managed by user programs through a message interface, the ancestor of every userspace pager including this one. What is not adopted is its asynchrony, which severs the priority relationship between the faulting thread and the pager servicing it. Section 15.3 takes the synchronous formulation from L4 instead.

- Young et al., [*The Duality of Memory and Communication in the Implementation of a Multiprocessor Operating System*](https://www.cs.princeton.edu/courses/archive/fall08/cos597B/papers/mach.pdf), SOSP, 1987.

**Fuchsia's asynchronous pager.** The modern version of the same shape, delivering page requests as packets to a port. Rejected for the same priority reason, and because it requires an object this design does not have.

**seccomp-BPF and an eBPF-style verifier.** Examined in section 17.2 and rejected because confinement here comes from the handle table rather than program text, so the security benefit is illusory, while the verifier would become the largest security-critical component in the kernel and would be catastrophic on a tier with no MMU.

- [seccomp BPF documentation](https://docs.kernel.org/userspace-api/seccomp_filter.html)
- [BPF verifier documentation](https://docs.kernel.org/bpf/verifier.html), for the scale of what was avoided.

**Android's `sync_file` merge.** Examined in section 9.4. Merge is an adapter for a single-wait ioctl API, and neither motivating constraint applies here. Dropping it also removed the only construct that would have reintroduced multi-target priority donation.

**Zircon's `zx_object_wait_many`.** Inherited as a negative result rather than rejected. It caps at 64 objects and Zircon's documentation directs users to ports beyond that. Fuchsia built the array form, found it did not scale, and moved to a queue.

- [zx_object_wait_many](https://fuchsia.dev/reference/syscalls/object_wait_many)

**io_uring's asynchronous execution.** The chaining semantics are adopted and the execution model is not. io_uring runs submissions on kernel worker threads, correct for a throughput-oriented general-purpose kernel and wrong here, because it breaks the connection between a requesting thread's priority and the priority at which its work runs.

**Global multiprocessor scheduling.** Better average responsiveness and automatic load balancing, at the cost of a contended lock on every scheduling decision and substantially harder worst-case analysis.

**Untyped memory and retype, from seL4.** Adopted in consequence, not in mechanism. Section 8 gets the statically bounded kernel through build-time pools. Because Tier 2 is also statically configured, retype is not deferred but genuinely unnecessary.

**POSIX `mmap` semantics.** Copy-on-write, anonymous mappings, and writable file-backed mappings are all absent, following Plan 9 and Inferno. Section 15.1 explains what this removes: transparent dirty tracking, swap, and a write fault that can block on a plain store instruction.

### 3.6 Further Reading

Systems consulted for comparison that did not directly contribute a mechanism, but worth reading before revisiting any decision here.

- [Genode OS Framework](https://genode.org/documentation/genode-foundations/index), for parent-child resource trading and capability delegation. An alternative answer to section 8.1.
- [Barrelfish](http://www.barrelfish.org/), for capability derivation on multicore and the multikernel model.
- [HelenOS](https://www.helenos.org/), for building an asynchronous framework above synchronous IPC, relevant if one-thread-per-source proves too costly.
- [Redox](https://doc.redox-os.org/book/), for a modern URL-scheme take on Plan 9 namespaces.
- [QNX adaptive partitioning](https://www.qnx.com/developers/docs/7.1/index.html#com.qnx.doc.adaptivepartitioning.userguide/topic/about.html), the commercial realization of the proportional-share idea in section 11.5.

### 3.7 Status of This Document

This is a design document rather than an implementation report, but it is no longer a description of something unbuilt. Section 1.3 says which half exists. Nothing in it has been measured, so every statement about performance, size or latency remains an intention or a citation of someone else’s measurement, never a result of this work.

Where building a mechanism showed the description of it to be ambiguous, incomplete or wrong, the section carrying it says so rather than being quietly corrected. Those notes are worth more than the surrounding prose: everything else here is reasoning, and they are the only parts that have met a machine.

Errors of attribution in this section are mine, not the cited authors'. Corrections are welcome.

---

# Part II — The System

This part describes the system in detail: what each mechanism is, how it behaves, and why it was chosen over the alternatives. It is a design, not a standard. Nothing in it has been built.

---

## 4. Scope

This part describes Nyctos in enough detail to be implemented. Where a decision is genuinely open, it says so rather than inventing an answer.

Sections 5 through 22 give the semantics. Section 23 gives the binding between those semantics and the machine, and section 26 gives the C interface. Section 25 gives the filesystem protocol, which matters more than its length suggests, because the substitutability claimed in section 2.8 rests on it.

**Two sections assemble rather than define.** Section 19 puts the mechanisms of earlier sections into the shape of a driver, and section 24 does the same for the root process. Neither introduces anything new. They exist because the shape of a driver and the shape of the first process are the clearest evidence for or against the claims in section 2.

Part III suggests what a port supplies underneath all of this. `ROADMAP.md` gives the order to build it in, and `PROPOSALS.md` holds what is deliberately left out.

**Language.** This document says *must* where a choice would break something stated elsewhere, and the reason is always given nearby. It says *may* where the choice is genuinely free. It avoids conformance language entirely: there is no test suite, no certification, and nothing here has earned the right to tell an implementer they are wrong.

Four constraints shape everything that follows, and they are stated here because a reader who does not hold them will find later decisions arbitrary.

**The system is statically configured at both tiers.** Object counts are fixed when the system is built. This is as true of a multicore application processor as of a microcontroller; the tiers differ in protection hardware, not in how dynamic they are. There is no kernel heap and no runtime growth.

**One API spans two protection models.** The same application-level interface runs on flat memory with no protection unit and on a full memory management unit. The tiers differ in what they *enforce*, never in what they *mean*.

**A thread blocks on exactly one object at a time.** This is relied on by reply addressing, by the priority inheritance chain, by the fault path, and by cancellation. It is stated as an invariant in section 5 and assumed throughout.

**Priority follows work across every boundary.** A request executes at the priority of whoever is waiting on it, through message passing, through page faults, and through locks. Mechanisms that would break this connection are rejected even where they would be faster.

### 4.1 Decision Log

| Area | Decision |
| ---------------------- | ------------------------------------------------------------------ |
| Syscall form | One operation descriptor per trap. No syscall numbers. |
| Batch language | Linear op array. No branches, no bytecode VM, no verifier. |
| Batch entry points | Program objects plus a direct-invoke shim for single ops. |
| Op flavors | Blocking ops and issuing ops. Issuing ops return a fence. |
| Chaining | Per-entry `LINK` flag: abort-on-error grouping. Chains, not graphs. |
| Graph edges | Expressed with fences, which cross batch and process boundaries. |
| Fence primitive | Monotonic timelines. Binary fences are timelines of depth 1. |
| Fence merge | Not present. Sequential waits. `WAIT_MULTI` is future work. |
| Fence errors | Abort or timeout force-signals pending fences in error state. |
| Fence donation | Waiters donate to the holder of a signal-righted handle. |
| Handles | Uniform allocation, 64-bit, index plus generation, with rights. |
| Handle rights | Rights mask on every handle. Duplication may only attenuate. |
| Vocabulary | `sys_proc_id_t` and `sys_thread_id_t`. `task_id_t` is deleted. |
| Blocking | A thread blocks on exactly one thing at a time. Invariant. |
| Reply addressing | By client thread id. No rcvid or transaction table. Any server thread may reply. |
| Connections | Minted by the server and given away. A client cannot connect to a channel. |
| Blocked states | SEND-blocked and REPLY-blocked are distinct and reported separately. |
| Reply status | `sys_err_t` in the message, outside the payload, so a chain can abort on it. |
| Batching | Two forms: a kernel batch collapses traps, a compound request collapses rendezvous. |
| Multi-wait | One thread per source. Wait-sets remain deferred. |
| SMP | Partitioned scheduling with explicit migration. |
| Co-scheduling | Cohort scheduling nodes, not gang admission. |
| Buffer contract | Move semantics, identical on both tiers. |
| Bulk payload | A buffer field in the message, rewritten at the rendezvous. Grants are for memory outliving a call. |
| Compound requests | Inline sequence, implicit linear chaining, abort at the first failure. |
| Cohort faults | The cohort is the recovery unit. |
| Cohort restart | Parent process via a death timeline. Policy is not in the kernel. |
| Handle transfer | Inline for small counts, count plus pointer for large. Move. |
| Pulses | Retained, distinct from fences. Reserved slots for system pulses. |
| Page faults | Fault IPC sent by the faulting thread to its pager. Tier 2, read-only. |
| Paging scope | Shared library text. No copy-on-write, no swap. |
| Loading | XIP from flash. Optional compile-time RAM loader. |
| Kernel memory | Static per-tier object pools. No kernel heap. Permanent. |
| Identifier layout | One machine word: index low, generation high, split sized to the pools per word size. |
| Mount table | In the C library, not the kernel. |
| Naming | Handle-relative only. No global namespace, no absolute paths, no ambient root. |
| Namespace handoff | Named directory handles passed at spawn, consumed by libc like `envp`. |
| Interrupts | Disabled in kernel on Tier 0; enabled on Tier 2. |
| Timers | Advance a timeline. No timer pulse. |
| Timeouts | Absolute deadlines. Relative helpers live in libc. |
| Exit and join | Exit advances a completion timeline; status read from the handle. |
| Cancellation | Prompt, plus a death pulse to any server holding the request. |
| Introspection | Text key-value attributes, declared with macros. |
| Debug console | A handleless write, compiled out by default and announced when not. |
| Root process | Compiled-in policy table. Hierarchical supervision, then a watchdog. |
| Boot archive | Aligned member table, no compression. Walked by root, served by romfs. |
| Boot handover | A port stage builds `sys_bootinfo_t`. No device tree in the kernel. |
| Filesystem | A protocol, not a VFS server. The mount table stays in libc. |
| File handles | A file handle is a connection. Offsets travel in the request. |

---

## 5. Vocabulary

The term "task" is not used anywhere in this document, because every real-time system defines it differently and the ambiguity is not worth inheriting. Where another system's "task" is meant, this document says process or thread and means exactly one of them.

**Process.** An address space, a handle table, and one or more threads. The unit of protection at Tier 2, and of resource ownership at both tiers. Operated on through a handle; `sys_proc_id_t` is an identity for introspection and reporting, not an operand.

**Thread.** A schedulable context within a process. Named by `sys_thread_id_t`. A process may have many threads. This is not negotiable and the rest of the design accommodates it.

**The blocking invariant.** *A thread is blocked on at most one object at any instant.* Every blocking primitive takes exactly one target. The reply mechanism, the priority inheritance chain, the fault path, and the absence of a transaction table all depend on this, so it is stated here rather than left implicit.

**Handle.** A per-process reference to a kernel object, carrying a rights mask. Named by `sys_handle_t`.

**Optional outputs may be NULL.** Any parameter this document describes as an output may be passed as `NULL` by a caller that does not want the value, and the kernel omits that write. This is stated once here rather than repeated per call, and it applies to every `out_` parameter in section 26. Inputs are never optional.

**Ids are not handles, and the distinction is deliberate.** A thread is named two ways in this API, and they mean different things. A `sys_handle_t` on a thread is *authority*: whoever holds it can set priority, migrate, or cancel. A `sys_thread_id_t` is a *reply token* and confers nothing. `sys_msg_recv()` returns the client's id rather than a handle to it, so a server acquires no power over its clients merely by serving them, which would otherwise be a privilege escalation available to every service in the system. Everywhere else a thread is named, it is named by handle.

**Cohort.** A set of driver processes sharing one protection domain. See section 18.

---

## 6. The Two Protection Tiers

| Tier | Hardware | Memory Model | Loading | Enforcement |
| -------- | ------------------------ | ------------------------ | ------------------ | ------------------------------ |
| **Tier 0** | Flat memory, no MPU or MMU | Shared global address space, PIC/PIE required | XIP from flash | Cooperative. Handles enforce modularity, not security. |
| **Tier 2** | Full MMU | Virtual address mapping | Demand-paged text | Strict isolation, remapping for zero-copy. |

### 6.1 The Uniform Memory Contract

A single API across two protection models is only meaningful if it states what a sender may do with a buffer after sending it. Without that rule portable code cannot be written, because on Tier 0 the receiver observes the sender's later writes and on Tier 2 it may not.

**The rule is move semantics, and it is identical on every tier.** It governs both forms in which memory crosses a boundary: the buffer field of a message (section 12.4), which is the ordinary case and needs no object, and a grant handle, which is what long-lived delegated authority over memory looks like: device registers, a page supplied by a pager, a scatter-gather target. The distinction is lifetime, not semantics. A payload lives for one call; a grant outlives the call that carried it.

> When memory is sent, the sender loses the right to read or write it until the corresponding reply is received. When the reply arrives, the right returns to the sender.

The tiers differ only in *enforcement*, never in *semantics*. Tier 2 enforces by unmapping from the sender and mapping into the receiver. Tier 0 does not enforce at all; the memory remains reachable and a violating program will not fault.

This generalizes into the design rule for the whole system. **Choose the contract that the stronger tier enforces in hardware and the weaker tier merely trusts.** A correct program then behaves identically on both, and the only thing Tier 0 loses is the ability to catch an incorrect one. A contract chosen the other way around, such as shared access with a convention against touching, is unenforceable everywhere and is therefore not a contract at all.

```{=typst}
#figure(include "fig/tiers.typ", caption: [The same violation under both protection models. The contract is identical; the tiers differ only in whether the hardware can detect a breach of it.])
```

The figure shows a program breaking the rule rather than obeying it, because **a correct program cannot illustrate the difference.** One that respects the contract produces the same result on every tier, which is the entire point. Only a violation distinguishes them, and then only by whether anything notices.

This is what makes Tier 0 a useful development target rather than merely a constrained one. The same source runs on both, so a contract violation can be found on hardware that enforces and then deployed to hardware that does not.

A Tier 0 debug build should include software checks for grant access outside the owning window, because that is the class of bug Tier 0 cannot catch and Tier 2 catches for free. No API in this document exposes a raw pointer that crosses a process boundary.

**Reply-path caveat.** The move rule above governs the forward path. The reply path is asymmetric, because a server that mapped its own memory into a client could not tell when the client had finished reading it, and so could never safely reclaim it. Reply payloads larger than the inline area therefore use the buffer the *client* supplied in the original send, returning ownership to a party that already knows the lifetime. **A server does not hand back memory of its own.**

### 6.2 The System Page

Kernel status, hardware topology, pool limits, and the timebase are exported through a page mapped read-only into every process. `SYS_SYSPAGE_ENTRY()` dereferences through the current process descriptor rather than a fixed virtual address, so Tier 2 isolation is preserved.

The 64-bit `uptime_ticks` field is protected by a seqlock and must be read through `sys_syspage_uptime()`, never by direct field access, because a 32-bit target cannot read it atomically and will occasionally tear.

That accessor is more load-bearing than it appears. Absolute deadlines (section 14.3) require a cheap clock read on every timeout construction, and reading the shared page costs no syscall. Without it, absolute deadlines would not be affordable.

---

## 7. Objects, Handles, and Rights

Every kernel object is referenced through a per-process handle table. Channels, grants, timelines, timers, batch programs, processes, threads, and interrupt attachments are all handles.

### 7.1 Representation

**An identifier is one machine word: a table index in the low bits and a generation above it**, incremented when a slot is reused. The generation costs nothing and buys use-after-close detection, since a stale identifier names a valid index whose generation no longer matches and is rejected with `SYS_ERR_STALE`. This is what makes reply-by-thread-id safe against a client dying and its id being recycled before the server replies.

**The split is sized to the pools, so it differs by word size.**

| Word size | Index | Generation | Slots per pool |
| --------- | ----- | ---------- | -------------- |
| 64-bit | 20 | 44 | 1,048,576 |
| 32-bit | 10 | 22 | 1,024 |

Twenty bits of index on a part whose entire system holds a few dozen objects would spend half the word describing a range no configuration of that part could reach. Those bits are worth more as generation, which is where they defend against something that can actually happen. A thousand slots per pool is still an order of magnitude beyond what a design with `max_threads` in the tens will configure, and section 8 makes that number a build-time decision that a static assertion checks rather than a limit anything discovers at runtime.

**Neither split is close to the hazard.** Aliasing requires one slot to be recycled through the entire generation range while a stale identifier naming it is still held, which is four million reuses in the narrower case.

**Neither split is an even division, and on a 64-bit build that is the whole point.** Two places in this document compress an identity into 32 bits: the pulse value field (section 10.5) and the priority-inheriting futex word (section 21.1). On a 64-bit build those are truncations, and the 20/44 split is what makes them carry the index **and generation bits alongside it**, so both are self-validating and neither can mistake a recycled slot for the original. With a 32-bit index neither could, and each would have needed its own answer to the same problem.

The two truncations differ slightly. `SYS_ID_TAG32` keeps 12 generation bits and is what a pulse carries. `SYS_ID_TAG31` keeps 11, because the futex word must reserve its top bit for the waiters flag and cannot widen to 64 bits, since Tier 0 targets have no 64-bit atomic instruction.

A false match then requires thousands of recycles of one slot inside the window between a value being written and being read, which is bounded and absurd. The authoritative generation remains the full width in the identifier itself, so the truncation costs nothing outside those two windows.

**On a 32-bit build both compressions get stronger rather than weaker.** An identifier is already 32 bits, so `SYS_ID_TAG32` is the identifier and no information is lost at all; `SYS_ID_TAG31` gives up one bit to the waiters flag and still carries twenty-one generation bits against the eleven a 64-bit build has. The part with the least memory is the one whose compressed identities are the most exact, which is the opposite of the usual direction and follows from sizing the split to the pools.

**Handles are opaque to a program.** The split is the kernel's, and the only operations userland performs on a handle are passing it and comparing it to `SYS_HANDLE_INVALID`. Nothing above the kernel may extract an index, and a program that does is reading a layout no part of this interface promises.

### 7.2 Rights

Every handle carries a rights mask, checked on use. **Duplication may only attenuate**: `sys_handle_dup()` cannot produce a handle with rights the source lacked.

Rights are uniform across object types rather than special-cased per type. A timeline is one object whose handles may carry `SYS_RIGHT_WAIT`, `SYS_RIGHT_SIGNAL`, or both, exactly as a channel's handles may carry send or receive rights. Nothing in the system requires two distinct object types where one object with two rights will do.

The benefit is least-privilege handoff everywhere. A driver can pass a channel handle without `SYS_RIGHT_TRANSFER` to prevent onward delegation, or a grant with `SYS_RIGHT_READ` only.

### 7.3 Allocation Policy

The kernel allocates handles arbitrarily, from per-CPU free lists, with no ordering guarantee.

POSIX requires that `open` return the lowest available descriptor, which shell redirection and `dup2` depend on. That is met **in libc, not in the kernel**, by a shim table mapping POSIX descriptor numbers to kernel handles. Lowest-first allocation is a serializing bitmap scan on a hot path, which is the worst policy for partitioned SMP, and it makes handle values predictable. Confining it to the compatibility layer keeps the kernel ABI uniform.

### 7.4 Transfer

Handles move between processes in IPC messages, by **move**, matching the memory contract. Transfer requires `SYS_RIGHT_TRANSFER`.

Because handles are per-process table indices, the kernel rewrites their values during the rendezvous. The message structure places handles at a known offset rather than embedding them in an opaque payload, so the kernel never parses user data to find them. Small counts travel inline, large counts through a count and pointer, mirroring the pattern already used for the payload itself.

```{=typst}
#figure(include "fig/handles.typ", caption: [Handle transfer by move. The value changes across the boundary, the sender's slot is invalidated, and both handles named the same object throughout.])
```

**A handle value is meaningful only to the process holding it.** Two processes may hold the same integer and reach different objects, or hold different integers and reach the same one. Nothing may be inferred about an object from the number that names it, and a handle value is therefore not a system-wide identity and must never be logged, compared across processes, or used as a key.

**Attenuation happens before transfer, not during it.** A transfer carries the handle's rights unchanged. To give a peer less authority than you hold, duplicate with a reduced mask first and transfer the duplicate, which is why the sender in the figure ends with its original intact and a weaker handle gone.

**The move invalidates the source slot.** Once transferred, the sender's handle is not merely unused but unusable: its slot is freed and its generation advances, so presenting the old value returns `SYS_ERR_STALE` rather than reaching the object. A sender that needs continued access keeps a separate handle, as `0x0003` does in the figure.

---

## 8. Kernel Memory: Static Object Pools

**There is no kernel heap, at any tier.** Every object type has a pool whose size is fixed at build time, published read-only in the system page so userspace can see the limits it works against.

The property being bought is that **kernel memory consumption is a property of the configuration rather than of the workload.** A system's kernel footprint can be computed from its build settings without running it, and cannot be driven upward by application behaviour.

Two things improve. `SYS_ERR_NO_MEMORY` moves from being reachable on nearly every syscall to being a configuration error visible at integration time, removing a large class of hard-to-test error paths. And on Tier 0, where total RAM may be a few kilobytes, a kernel that cannot grow is the only kind worth having.

Because Tier 2 is also statically configured, this is the permanent model rather than an interim one. Untyped memory and retype are not on the roadmap. The cost is that pool sizing is a real integration task and a system that exhausts a pool must be reconfigured and rebuilt, which is the normal way of working for the systems this targets. Section 22 exists partly so that pool utilization can be observed and sized honestly.

**Objects owned by another object do not draw from a pool.** A completion timeline (section 10.4) is part of its thread or process rather than an entry in the timeline pool, and a channel's pulse queue is part of the channel. The rule matters because the alternative is worse in both directions: charging them to the general pool means every sane-looking configuration silently under-provisions by `max_threads + max_processes`, while allocating them lazily means `sys_object_completion()` can fail with `SYS_ERR_NO_MEMORY` and supervision becomes unreliable exactly when the system is under pressure. Embedding makes the accounting exact and the call infallible.

### 8.1 Boot and Initial Capability Distribution

The kernel starts one root process and hands it a `sys_bootinfo_t` describing the physical memory map, the pool configuration, and an array of initial handles.

The root process holds every capability at handoff, including the root pager channel and all interrupt attachment rights, and distributes them by spawning children with explicit inherit arrays. There is no ambient authority: a process reaches only what its parent placed in its handle table.

---

## 9. Timelines and Fences

The timeline is the system's principal synchronization object. Driver completion, interrupt delivery, timer expiry, and thread exit are all timeline advances, so a thread waiting for work to finish waits on one kind of object whatever the work was.

**It is not the only one, and an earlier draft of this sentence said it was.** A thread blocked in `sys_msg_send()` is waiting for a reply rather than for a counter to reach a value, so a channel is the second thing a thread can block on. Building the two showed the difference to be real: a reply carries data back to one specific waiter, where a signal releases everyone a value satisfies and carries nothing. Forcing the first into the second would have meant inventing a value for every request and a timeline per client.

What survives is the invariant the claim existed to protect, which is section 5 rather than this one: a thread is blocked on **at most one object**, whatever kind. That is what reply addressing, the priority chain and cancellation all rest on, and none of them needs the object to be of a single type. The practical cost of a second kind was one function pointer per thread, saying how to take it off whatever it is on.

### 9.1 The Primitive

A timeline is a monotonically increasing 64-bit counter. A waiter blocks until the counter reaches or passes a named value. A binary fence is a timeline of depth one, provided as a userspace idiom rather than a second kernel object.

Timelines rather than one-shot binary fences, for three reasons. There is one object per pipeline instead of one per transfer, removing an allocation from the hot path, which matters more given static pools. Waiting on a value not yet signalled is legal and well defined, removing the wait-before-signal race that binary fences force you to design around. And a sequence of dependencies from one producer collapses into a single wait.

Signalling a value at or below the current value is an error. Monotonicity is what makes a late waiter safe.

```{=typst}
#figure(include "fig/timeline.typ", caption: [Timeline semantics. Waiter A blocks and is released when the counter reaches its value. Waiter B asks for a value already passed and returns immediately, which is why a late waiter is safe. Waiter C is blocked on a value that never arrives and is released by the failure. Waiter D arrives after the failure and receives the error at once.])
```

The four waiters in the figure exhaust the cases an implementation must handle. A and B are the ordinary paths, and the difference between them is the property that makes the primitive usable without a registration protocol: **there is no window in which a waiter can miss a value.** C and D are the failure paths from section 9.3, and the distinction between them matters, because the error state is not an event that waiters must be present to observe. It is a property the timeline retains, so a waiter arriving afterwards is satisfied just as immediately as one that was already blocked.

### 9.2 Priority Donation

A thread blocked on a timeline donates its priority to the holder of a signal-righted handle, exactly as an IPC client donates to a server.

**Kernel-signalled timelines have no donation target.** Timers and interrupt attachments are advanced by the kernel itself, so donation is a no-op for them. This is stated rather than left to inference, because a reader who has internalized the donation rule will otherwise wonder what a timer inherits.

**Which holder is not stated, and needs to be.** A timeline may have any
number of signal-righted handles, held by any number of processes, and
nothing here says which of them a waiter donates to. Section 9.4 assumes
the answer is exactly one when it argues that dropping merge buys
constant-time single-target donation, but nothing constrains a timeline to
one signaler. Donation is unimplemented, so this has cost nothing yet; it
has to be answered before it is built, and the likely answer is that
donation belongs to whatever a timeline is bound to rather than to whoever
holds a handle on it.

### 9.3 Error Propagation and Abandonment

A timeline can enter an error state, satisfying every outstanding and future wait immediately with an error rather than success. Four events force it:

1. **Batch abort.** Every timeline the batch was going to signal is force-signalled in error state.
2. **Last signal right closed** without having reached the value waiters are blocked on.
3. **Signaler death**, which closes its handles and triggers rule 2.
4. **Timer destruction**, which is rule 2 applied to a kernel-held signal right.

Without these, a consumer blocks forever on a timeline whose producer died, converting a producer bug into a system hang. The general principle is that **the disappearance of the only party who could satisfy a wait is itself an event**, and must be delivered as one rather than left to a timeout.

Wait deadlines still exist, but as a backstop against hardware that never completes, not as the primary failure mechanism.

**A value already reached is not affected by the error state.** The rule
above says every outstanding and future wait is satisfied with an error,
and taken literally that would include a wait on a value the timeline had
already passed. It does not. Monotonicity is what makes a late waiter safe,
and that property is more load-bearing than this one: a value that was
reached stays reached, whatever later became of the timeline. An
implementation therefore checks the value before the error state, and the
two orderings are distinguishable, so this is a rule rather than a detail.


### 9.4 There Is No Merge Operation

Fence systems built on one-shot binary fences generally provide a merge operation, combining several fences into one that signals when all of them have. Nyctos does not, because the two conditions that make merge necessary are both absent here.

Merge exists to adapt many fences to a wait interface that accepts only one, and to collapse a producer's sequence of one-shot fences into a single object. Timelines already collapse a producer's sequence natively, and a wait here is an op in an array rather than a single call, so three dependencies are three consecutive wait ops. **The batch array is the merge.**

Dropping it gains constant-time single-target donation, since a thread is blocked on exactly one timeline and donates to exactly one holder rather than walking a member list on every priority change. It also gains precise error attribution, since the op that fails is the one that errored and its status field names it.

It costs a transferable compound handle, and it costs fixed batch shape, since a varying dependency count means a varying number of wait ops and a prebuilt submission must be rebuilt.

**Reserved for a future revision: `SYS_OP_FENCE_WAIT_MULTI`.** If fixed batch shape proves to matter in practice, the extension is merge as an *op* rather than as an object: one entry carrying a short inline array of handles. That restores fixed-shape batches with no new object type, keeps per-handle error status, and limits donation to a walk of a bounded inline list only while blocked. Such a list must stay small, on the order of four to eight handles. It is a convenience for the common narrow join, not a general multi-wait facility, and sizing it as one would reintroduce the donation cost the absence of merge was chosen to avoid.

---

## 10. Processes and Threads

Section 5 gives the one-line definitions. This section says how a process and a thread come into existence, what a thread costs, and what state each carries. Scheduling is section 11; loading a program image is section 15.

### 10.1 Processes

A process is created by `sys_proc_spawn()`, which is the only way one exists. There is no fork. A process is constructed from a description rather than cloned from a parent, so nothing is inherited implicitly: every handle the child holds, and every region of its address space, was named in the spawn call.

**Nothing is inherited.** The child receives exactly the handle array and the namespace array the parent passed, placed in its table by position (section 20.4). It does not receive the parent's other handles, its mappings, or its identity. A parent that wants a child to have something must say so, and the spawn call is the complete list of what the child can reach. This is what makes the absence of an ambient root in section 20 hold in practice rather than in principle.

The absence of fork is not a simplification for its own sake. Fork duplicates an address space, a handle table and a thread's continuation in one operation, which means it must decide what happens to every handle, every mapping and every other thread in the parent. Those decisions are the copy-on-write machinery, the close-on-exec flag and the "fork in a threaded program is undefined" clause that the interface is then stuck with. Constructing the child instead of copying the parent removes all three.

**A process is a resource owner, not a schedulable thing.** It owns an address space, a handle table, a reentrancy mode, and its threads. It is never dispatched. Everything the scheduler acts on is a thread, which is why section 11 speaks only of threads and why priority is a thread attribute rather than a process one.

**The spawn is atomic and its failure is total.** A spawn either produces a process with every handle installed and its first thread ready to run, or it produces nothing and consumes no pool slot. There is no half-built process observable through introspection and no partially populated handle table, because a spawn that could fail halfway would make the pool accounting in section 8 depend on where it failed.

### 10.2 Threads

A thread is created by `sys_thread_create()` against a process handle, which requires `SYS_RIGHT_MANAGE` on the target. A process can therefore create threads in another process if it has been given the authority to, which is how a supervisor recovers a wedged server without tearing it down.

**The creator supplies the stack.** The kernel does not allocate stacks. The caller passes a region it has already mapped in the target address space, along with the entry point and one argument word. This follows from section 8: a kernel with no heap cannot allocate a stack, and a kernel that carved stacks from a static pool would be choosing a stack size on behalf of every program in the system.

The consequence is worth stating plainly, because it is the sharp edge of the decision. **Stack overflow is a fault in the faulting thread, not a kernel error.** The kernel knows the region it was given and nothing about how the thread uses it. A guard region below the stack turns an overflow into a memory fault at the guard rather than a silent write into whatever is mapped below, and a port on Tier 2 should map one by default. On Tier 0 there is no MMU to fault with, and the choice is between a guard band checked by a stack-limit register where the part has one and no detection at all. This is the clearest instance of the rule in section 6: the same interface, two enforcement strengths, and the weaker cannot catch its own violation.

**Thread-local storage is a userspace convention.** The kernel reserves no per-thread word, provides no TLS register, and does not know that thread-local storage exists. A thread gets its entry argument, and a runtime that wants a thread pointer either uses the architecture's dedicated register directly or stores it at a known offset in the stack region it allocated. Putting TLS in the kernel would mean choosing an ABI, and the ABI belongs to the language runtime.

A thread carries a priority, a core affinity, a blocked-on pointer, a completion timeline, and its saved context. It carries no name, no signal mask, no working directory, and no per-thread handle table. Everything else a thread appears to have belongs to its process.

**Threads are drawn from the static pool.** Creation fails with `SYS_ERR_NO_RESOURCE` when the pool is exhausted, and there is no growth path. A design that creates threads dynamically in response to load must therefore state its bound, which is the intended effect: an unbounded thread pool is an unbounded kernel memory requirement, and section 8 does not allow one.

### 10.3 Process Reentrancy Mode

A process declares at spawn whether it is single-threaded, **serially reentrant** (multiple threads, but only one executing in the address space at a time), or fully parallel.

Serial reentrancy is valuable for cohorts specifically. A cohort whose members are serially reentrant needs no internal locking between them, which removes the main source of complexity that shared-protection-domain code otherwise acquires, at the cost of no intra-cohort parallelism. For a storage cohort on a two-core part that is usually the right trade.

### 10.4 Exit, Join, and Status

**Exit advances a completion timeline.** Every thread and process has one, obtained from its handle with `sys_object_completion()`. Joining is therefore an ordinary timeline wait and introduces no new blocking primitive.

The completion timeline is **embedded in the thread or process object**, not drawn from the timeline pool, per section 8. The call cannot fail for want of a pool slot.

**The returned handle carries `SYS_RIGHT_WAIT` only, and the kernel holds the sole signal right.** This is not a default worth getting wrong. If a process could obtain a signal-righted handle to its own completion timeline it could fake its own exit, and a supervisor (section 18.3) would restart it while it was still running, or the RAM loader (section 15.2) would reclaim memory out from under it. The whole supervision model rests on a death signal that only the kernel can produce.

A timeline value is a monotonic counter and cannot carry an exit code, so the code is read separately with `sys_thread_status()` or `sys_proc_status()`. The handle remains valid after exit until closed, which is where the exit code lives. This avoids a zombie concept distinct from the handle: a process that has exited but whose handle is still held is simply an object whose completion timeline has advanced.

**The completion timeline outlives the object.** When the last handle to a thread or process closes, the object is reaped and its resources released, but any completion-timeline handle still held remains valid and still reports the exit.

This is not the same as keeping the object alive, and it costs nothing to arrange. Once reaped, the timeline can never change again, so its final value and error state are complete information. They are frozen into the handle-table slot of each holder rather than allocated anywhere: the slot already exists and is already accounted to that process, so the detachment cannot fail. A supervisor holding only the completion timeline therefore always receives the death it is waiting for, which is what the whole supervision model in section 18.3 depends on, and section 8's guarantee that `sys_object_completion()` is infallible survives intact.

### 10.5 Cancellation

`sys_thread_cancel()` unblocks a thread, causing its pending blocking call to return `SYS_ERR_INTERRUPTED`. Without it, `SYS_ERR_INTERRUPTED` would be a code nothing generates, and cohort restart would be unable to break children out of pending rendezvous.

Cancellation is **prompt**, and the interesting case is a client cancelled after a server has already received its message but before replying.

1. The client unblocks immediately and returns `SYS_ERR_INTERRUPTED`.
2. The server additionally receives `SYS_PULSE_DEAD_OWNER`, whose value is `SYS_ID_TAG32` of the cancelled client's thread id, so it can release whatever it allocated without waiting to discover the failure.
3. If the server replies anyway, the reply returns `SYS_ERR_STALE`.

The pulse in step 2 is what makes prompt cancellation safe. Without it a server only learns at reply time, and a server that ignores a failed reply leaks the request's resources permanently. Deferring cancellation until the server replies would avoid the issue but would make cancellation unbounded when a server is wedged, which defeats the purpose during restart.

**Why the pulse carries a tag rather than a bare index.** A pulse value is 32 bits, so on a 64-bit build the identity must be compressed; on a 32-bit build it fits whole and the paragraph below describes a hazard that build does not have. A bare index is not sufficient: the client is cancelled, the kernel enqueues the pulse, the slot is recycled to a new thread which sends to the same server, and the server matches the late pulse against the new request and releases the wrong one. That is the ordinary ABA problem. `SYS_ID_TAG32` carries the index plus 12 bits of generation, so a false match would require 4096 recycles of that slot between enqueue and dequeue. The server matches by comparing `SYS_ID_TAG32(client_id)` against the pulse value, using the id it already holds. This is why the 64-bit split leaves generation bits below bit 32, per section 7.1.

**Process termination cancels every thread.** `sys_proc_kill()` promptly unblocks all threads of the target process exactly as if each had been cancelled individually, including delivering the `DEAD_OWNER` pulse to every server holding one of their requests. This is the path cohort restart actually takes, and it has to work without per-thread handles: a supervisor holds a handle to the cohort's processes and no handles at all to the threads inside them.

```{=typst}
#figure(include "fig/cancel.typ", caption: [Prompt cancellation involves three parties: the client unblocks at once, and the server is told rather than discovering it at reply time.])
```

A thread cancelled mid-batch stops at the next entry boundary, which is already a preemption point. Remaining entries are marked `SYS_ERR_CANCELED` and pending fences are force-signalled in error state, per section 9.3.

### 10.6 States

```{=typst}
#figure(include "fig/threadstate.typ", caption: [Thread state transitions. Every way into and out of BLOCKED is listed, and EXITED is a state the thread rests in rather than the point at which it is reaped.])
```

Two properties of this diagram are consequences of decisions made elsewhere, and both are easy to miss.

**BLOCKED is one state, not a family of them.** Because a thread is blocked on at most one object, there is no need for separate blocked-on-send, blocked-on-receive and blocked-on-timeline states, and every entry and exit fits on a single pair of edges. A design permitting a thread to wait on several things at once would need a substate per combination, and the transitions would no longer be enumerable.

**The list of unblock reasons is complete, and that completeness is load-bearing.** Cancellation and timeline failure must be able to release a thread from whatever it is waiting on, without knowing which mechanism blocked it. That is only possible because there is one blocked state with a known set of exits. If the set were open, a new blocking primitive could introduce a thread that cancellation cannot reach, which would make cohort restart unreliable in exactly the situation it exists for.

`EXITED` is also not the end of the object's life. The thread has stopped and its exit code is readable, its completion timeline has advanced to release anyone joining, and the object itself is released only when the last handle to it closes.

---

## 11. The Scheduler

Sections 9 and 12 say what makes a thread runnable. This section says which runnable thread runs. It is stated separately because the response time claims in section 13.3 are claims about this section, and because a reader implementing the kernel needs the dispatch decision in one place rather than distributed across the mechanisms that trigger it.

### 11.1 Priorities

Priorities run from `0` (lowest) to `SYS_PRIO_MAX`. The default configuration provides 256 levels on Tier 2 and 32 on Tier 0, where a 32-bit ready bitmap makes the highest-priority lookup a single count-leading-zeros instruction.

Within one priority level the policy is FIFO. Round-robin is available per-thread as an opt-in with an explicit quantum, because round-robin at the same priority introduces timing variance that analysis must account for and should be a decision rather than a default.

### 11.2 The Dispatch Decision

Each core holds a ready queue in two parts, and the decision between them is fixed rather than tunable.

1. **The real-time band.** Threads at or above `SYS_PRIO_RT_MIN` are held in a bitmap-indexed array of FIFO queues, one per priority. The highest set bit in the bitmap names the queue to take from. On Tier 0 the bitmap is one word, which is why section 11.1 suggests 32 levels there and why the lookup is a single count-leading-zeros instruction.
2. **The proportional band.** Everything below `SYS_PRIO_RT_MIN` is scheduled from the tree of section 11.5.

**The real-time band is consulted first, and unconditionally.** If any thread at or above `SYS_PRIO_RT_MIN` is runnable, the proportional tree is not walked at all. This is what lets response time analysis ignore the tree entirely: no amount of activity below the real-time floor can delay a real-time thread beyond the blocking terms already accounted for. The cost is equally plain, and is the reason the floor exists as a configured constant rather than a convention. A busy real-time thread starves everything below it, and a system that puts too much above the floor has no scheduler left, only a priority list.

**A scheduling decision is O(1) in the real-time band and O(depth) in the tree.** Selecting from the bitmap is one instruction plus one dereference and does not depend on the number of runnable threads. Selecting from the tree walks from the root to a leaf, so its cost is bounded by the configured tree depth, which is a build-time constant. Neither cost depends on the number of threads in the system, which is what makes the figures in section 13.3 statable at all.

**The effective priority is the donated one.** Where a thread has received priority through the chain of section 9.2 or section 12.3, the scheduler sees the donated value and nothing else. There is no separate base-priority queue and no re-sorting pass: donation moves the thread between queues at the moment it is donated to, so the dispatch decision never has to know that donation exists. This is what keeps donation off the dispatch path, where its cost would be paid by every scheduling decision rather than by the ones that involve it.

**Scheduling decisions happen at named points, and the list is closed.** A decision is taken when a thread blocks, when a thread is made runnable, when a thread's effective priority changes, when a running thread's quantum expires, and on return from an interrupt that did any of those. Nothing else reaches the scheduler. The list is closed for the same reason the unblock list in section 10.6 is closed: an open set of entry points is a set that cannot be enumerated when accounting for worst-case latency.

### 11.3 Preemption

**The kernel is preemptible except inside named regions, and every one of those regions is bounded by construction rather than by inspection.** This is the load-bearing property behind the interrupt latency figures in section 13.3, and it is stated as an obligation on the implementation rather than as a description of one.

Three regions are non-preemptible, and there are no others.

**The interrupt entry path**, from the vector to the point where the ISR has advanced its timeline and returned, per section 13.2. This region contains no loop over an unbounded structure. It is short by construction because the ISR runs no driver logic.

**A single submission entry**, per section 17.7. An entry is the unit of execution in a batch, and a batch may hold many, so the batch as a whole is preemptible at every entry boundary while an individual entry is not. `SYS_BATCH_MAX_ENTRIES` therefore does not appear in interrupt latency at all; the cost of one entry does.

**The handle table and pool manipulations** that must be atomic with respect to another core, which are bounded by the fixed structure sizes of section 8 and hold no lock a userspace thread can be waiting on.

Everything else, including the whole of the IPC path and the whole of the scheduling decision, is preemptible. A thread that becomes runnable at a higher priority than the one currently running takes the core at the next preemption point, and the distance to that point is the latency figure.

**Preemption is not the same as the round-robin quantum.** Round-robin, which is opt-in per thread under section 11.1, only decides which thread of *equal* priority runs next when the quantum expires. A higher-priority thread becoming runnable preempts immediately and does not wait for any quantum. Conflating the two is how a system acquires a scheduling latency equal to its tick period, and section 14.1 is written specifically so that no such period exists.

### 11.4 Multicore: Partitioned With Explicit Migration

Each core has its own run queue and makes its own scheduling decisions. A thread has affinity to exactly one core at a time. There is no global run queue and no global scheduler lock.

This is chosen for analyzability. Response time analysis on a partitioned system decomposes into an independent per-core problem. Global scheduling gives better average responsiveness and automatic balancing, but puts a contended lock on every scheduling decision and makes worst-case analysis substantially harder.

Load balancing is explicit. `sys_thread_migrate()` moves a thread to a named core, so migration cost is visible in the code rather than hidden in a balancer.

**Cross-core priority inheritance.** When donation crosses a core boundary, the donating core updates the target's inherited priority and sends an inter-processor interrupt to the target's core, which re-evaluates its run queue on receipt. Donation latency is bounded by IPI delivery plus one scheduling decision, and that term must appear in any response time calculation involving cross-core donation. Keeping a client and its server on the same core removes the term entirely, which makes core placement part of system configuration rather than an afterthought.

### 11.5 Cohort Scheduling Nodes

Below the real-time band, threads are scheduled from a tree whose internal nodes hold proportional shares of the CPU. Time relinquished by a thread is offered first to other threads under the same node, and escapes to the wider system only when that node has nothing runnable.

**A cohort is a scheduling node.** Since a cohort is exactly a set of tightly coupled processes handing work to one another, this keeps a storage request moving through the filesystem and block drivers rather than losing the core between hops. It approximates the benefit of co-scheduling a related set without requiring that they be dispatched simultaneously across cores.

Two limits are stated plainly, because the approximation is easily overread.

**This is not gang scheduling.** Members are not co-scheduled across cores at the same instant, and a member on another core may still be delayed. What the node buys is that yielded time stays within the group, not that the group runs together.

**It does not apply to real-time threads.** Proportional-share allocation is incompatible with strict fixed-priority response time analysis, so it governs only threads below `SYS_PRIO_RT_MIN`. At or above that priority, scheduling is by priority alone, and a cohort containing real-time threads gets no proportional behaviour for them.

### 11.6 The Idle Behaviour

When a core has nothing runnable it enters an idle state, and what that state is belongs to the port rather than to this document. What is specified is that idle is not a thread. There is no idle thread occupying a priority level, no idle process visible to introspection, and no thread id that means "nothing". A core is either running a thread or it is idle, and the two are distinguishable in the introspection attributes of section 22.2.

This matters for one reason beyond tidiness. An idle thread at priority zero would be a runnable thread, and the proportional-share tree of section 11.5 would have to account for it: every share calculation would carry a term for a thread that exists only to do nothing. Making idle a state of the core rather than a thread keeps the tree describing real work.

**It matters for a second reason, found by building it.** While a core is
idle there must be no current thread either, not merely no idle thread. The
interrupt that ends the idle may make the very thread whose blocking caused
it runnable again, and if that thread is still recorded as the one running,
it is being made runnable while running. Narrowing the check that catches
that is not a fix: it leaves the scheduler comparing priorities against a
thread that is not running and queueing it. A core that is idle is running
nothing, and the implementation has to mean it.

**Waking from idle is on the interrupt path, not the scheduling path.** A core with nothing to run has programmed its timer to the next deadline it knows about, per section 14.1, and is woken either by that timer or by a device interrupt or an IPI. The wake cost is therefore an interrupt entry cost, and it belongs in the figures of section 13.3 rather than in a separate accounting.

---

## 12. Synchronous IPC

Every request in the system crosses this boundary. A client asking a filesystem to read a file, a driver reporting a transfer, a faulting thread asking its pager for a page: all of them are these three operations. Section 2.2 says everything crosses the same boundary, and this is that boundary.

It is therefore specified here in full rather than sketched. A microkernel is mostly the interface in this section, and a reader who has to guess at the rendezvous has to guess at everything built on it.

### 12.1 Rendezvous

Three operations, defined by what blocks.

| Operation | Blocks until |
| --------------------- | ------------------------------------------------ |
| `sys_msg_send()` | a server has received the message and replied to it |
| `sys_msg_recv()` | a message or a pulse is available |
| `sys_msg_reply()` | never; it does not block |

**There is no message queue, and this is the central fact of the design.** A message is copied from the sender's buffer to the receiver's at the moment both are present, and at no other time. Nothing is buffered anywhere in between. The blocked senders *are* the queue, which is why the kernel needs no message pool, no per-channel storage sized at build time, and no policy for what to do when that storage fills.

Three properties follow, and each is load-bearing elsewhere.

**A sender is a thread the kernel can see, name, prioritise, and release.** A buffered message is anonymous data; a blocked sender is a scheduling entity. Priority ordering of the queue (below), donation (section 12.3), and release on server death (section 12.5) are all only possible because the queue holds threads.

**Flow control needs no mechanism.** A client cannot outrun a server, because the client is stopped for the duration of every request it makes. A system where a fast client can pile work on a slow server needs a policy for the overflow; there is no overflow here to have a policy about.

**The cost of a request is paid by whoever makes it.** The server's work runs at the client's priority, in a rendezvous the client is blocked in, so a client cannot push cost onto a server without accounting for it.

#### Channels and connections

**A channel is created by a server and never by a client.** `sys_chan_create()` returns a handle carrying `SYS_RIGHT_RECV`, and holding that right is what it means to be the server for a channel. A channel is owned by the process that created it, and that ownership is what section 12.5 keys death off.

**A connection is minted by the server too, and given away.** `sys_chan_connect()` requires `SYS_RIGHT_RECV` on the channel, which a client by construction does not hold. The server names a connection with an opaque 64-bit **cookie**, which the kernel carries with every message that arrives on it and never interprets. The resulting handle is then moved to a client, either in a reply or in the startup namespace of section 20.4.

**An earlier version of this section had the client connect and the server accept or reject, and that was wrong on its own terms.** Connecting requires naming the channel to connect to, and section 20.3 has removed every ambient name from the system. A client that could name a channel it was not given would be holding authority nobody granted it, which is the thing the handle model exists to prevent. Server-minted connections need no accept step, no rejection path, and no identity check at connect time, because a connection only ever exists in a client's table because a server put it there.

**The cookie is where a server keeps its per-client state, and it is also where identity lives.** There is no kernel-supplied "which process is calling" field, and there should not be. A server that cares who is calling recorded it in the cookie when it minted the connection, which is the only moment at which it actually knew. This is the same argument as section 25.2: the connection is the state.

**A message may also be sent on a bare channel handle carrying `SYS_RIGHT_SEND`.** The cookie delivered is zero, which is reserved to mean "no connection". This is the shape root uses when it creates a channel and hands both ends out at boot (section 24.5), before any connection exists to mint.

#### The three blocked states

A thread involved in IPC is in one of three states, and the distinction between the first two is not cosmetic.

| State | Meaning | Released by |
| ------------------ | -------------------------------------------------- | ---------------------- |
| **SEND-blocked** | queued on the channel; no server has taken it yet | a server receiving |
| **REPLY-blocked** | a server holds the message and is working on it | that server replying |
| **RECEIVE-blocked** | a server waiting with no message available | a client sending |

**SEND-blocked means the request has not been seen; REPLY-blocked means it has.** A client accumulating time SEND-blocked is waiting on server capacity, and the answer is more receiving threads. A client accumulating time REPLY-blocked is waiting on the work itself, and more threads will not help. Introspection (section 22) must report the two separately or it reports nothing useful about a saturated server. The distinction also decides what a timeout means, below, and what a dying client owes its server, in section 12.5.

Both states satisfy the blocking invariant of section 5: the thread is blocked on the channel, on exactly one object, and the transition from one to the other happens inside the kernel with no window in which it is on neither.

#### The sequence

Whichever side arrives first, the same rendezvous occurs.

**Server first.** The server calls `sys_msg_recv()` and finds no sender, so it becomes RECEIVE-blocked. A client calls `sys_msg_send()`, finds a waiting receiver, copies its message into that receiver's buffer, passes the cookie and its own thread id, donates its priority, makes the server runnable, and becomes REPLY-blocked. It was never SEND-blocked.

**Client first.** The client calls `sys_msg_send()`, finds no receiver, and is inserted into the channel's sender queue, SEND-blocked. A server calls `sys_msg_recv()`, takes the highest-priority sender, copies that sender's message into its own buffer, takes the cookie and the sender's id, and accepts the donation. The client moves from SEND-blocked to REPLY-blocked without running.

In both cases the server then does its work and calls `sys_msg_reply()`, which copies the reply into the client's buffer, moves any handle the reply carries into the client's table, drops the donated priority, and makes the client runnable. The reply does not block, so a server never waits on a client.

**Receive queues are ordered by sender priority, highest first, and never by arrival.** FIFO ordering here is a priority inversion that no amount of donation can repair, because the inversion is in the queue rather than in the execution: donation raises the thread that is running, and a low-priority request ahead of a high-priority one in a FIFO queue is not running, it is next. Ties are broken by arrival order, so equal-priority clients are served fairly.

**Threads blocked in receive are in no particular order.** They are interchangeable by construction, since a receiving thread has no message and no client, so any of them will do and the cheapest choice is correct.

#### Timeouts and cancellation

`sys_msg_send()` takes an absolute deadline, per section 14.3. Expiry, and cancellation under section 10.5, act on the two blocked states differently, and the difference has to be stated because the server's view differs.

**A SEND-blocked client is removed from the queue and the server never learns it existed.** Nothing was delivered, nothing was allocated on its behalf, and the client returns `SYS_ERR_TIMEOUT` or `SYS_ERR_CANCELED`. This case is clean.

**A REPLY-blocked client abandons work already in progress.** The client is released with the same error, but the server is still holding the message and will reply to a thread that is no longer waiting. That reply returns `SYS_ERR_STALE` (section 12.2), which is how the server discovers the outcome. Any resources the server committed to the request are released on that path, in the same place it would release them on a normal reply.

**A server that must not have its work discarded should not be given a deadline to discard it.** This is a statement about how clients are written rather than a mechanism: a request with a side effect on the server, and a client-side deadline shorter than the operation, is a request whose completion the client will not learn about. Deadlines belong on operations a client can meaningfully abandon.

### 12.2 Reply Addressing

`sys_msg_recv()` returns the client's `sys_thread_id_t`, and `sys_msg_reply()` takes it.

This works without any transaction identifier because of the blocking invariant in section 5. A thread blocks on at most one thing, so a blocked sender is uniquely identified by its thread id and there is nothing for an rcvid to disambiguate. A server may hold a client's thread id and reply later, out of order with respect to other clients, because the blocked client thread is itself the queue slot.

**A reply may be made by a thread other than the one that received.** The id is a token, not a lock, so a server may receive on one thread and reply from another. This is what makes a receive-and-dispatch structure possible without the kernel knowing anything about worker threads, and it is the whole of the kernel's support for section 12.6.

**The id confers no authority over the client**, per section 5. It permits exactly one thing, which is completing the request the client is blocked in. A server acquires no power over its clients by serving them, which would otherwise be an escalation available to every service in the system.

The hazard this exposes is recycling: the client dies, its id is reused, and the server replies into the wrong thread. The generation counter closes it. A reply is refused with `SYS_ERR_STALE` when the id names a slot whose generation has moved on, and equally when it names a live thread that is not REPLY-blocked on this server. Both are the same check, which is that the thread the id names is not the thread that sent this request.

**A reply that carries a handle can partly fail.** The handle move is subject to the receiving table having room, so the reply may deliver its payload and fail to deliver its handle. The client is released either way, with the error, because leaving it blocked on a reply that has already been sent is worse than telling both sides the transfer failed.

### 12.3 Priority Inheritance

When a thread sends to a channel, the receiving thread runs at `max(its own priority, the client's priority)` for the duration of the request and reverts on reply.

**Donation begins at the rendezvous and not at the send.** A SEND-blocked client donates to nobody, because there is nobody serving it yet, and there is no thread whose promotion would help. What serves that client is the queue being priority-ordered, so that it is taken first. The two mechanisms cover the two states, and neither covers the other's.

Donation is transitive. If the server blocks sending to a second server, the donated priority propagates along the chain. Chain length is bounded by `SYS_PRIO_CHAIN_MAX`, and a send that would exceed the bound fails with `SYS_ERR_DEADLOCK`. Section 21 covers composition with the other priority mechanisms.

**Transitivity needs no walk, and the bound is therefore unenforced.** A server sends onward at its own effective priority, which is already the donated value, so each hop carries the donation forward by itself: a client at 25 reaching a server at 10 that calls a server at 5 leaves all three at 25 with nothing traversing a chain. A walk would only be needed to raise a thread that is *already* blocked onward, and donation happens at the rendezvous, when the target has just stopped being blocked.

The consequence is that `SYS_PRIO_CHAIN_MAX` and `SYS_ERR_DEADLOCK` describe a mechanism that is not there, and a cycle of servers would deadlock undetected. Closing it needs a link from a blocked sender to whoever is serving it, which is the walk this paragraph assumes; the alternative is conceding that per-send donation is the whole mechanism and that deadlock detection belongs somewhere else.

```{=typst}
#figure(include "fig/rendezvous.typ", caption: [A rendezvous, with the server running at the client's priority for the duration of the request.])
```

### 12.4 Message Structure

A message carries a small inline payload, an optional memory grant for bulk data, and zero or more handles. The same structure is used for a request and for a reply, so a server filling in a reply is doing what a client did to send.

The inline payload exists so that small requests need no grant at all. Grant setup means address-space manipulation on Tier 2, which is disproportionate for a keystroke, a serial byte, or a short status word. Below `SYS_IPC_INLINE_BYTES` the payload is copied directly and no grant is constructed.

`inline_data` is **a memory copy, not a register transfer.** The distinction matters because a register-based fast path and a copied struct field have different costs and different size limits, and conflating them leads to interfaces that promise register speed for payloads far too large to fit in registers. Whether a port additionally carries the first few words in CPU registers is an implementation choice that does not change the interface or its semantics.

#### The reply carries a status the kernel can read

**A reply has a `status` field of type `sys_err_t`, outside the opaque payload.** Section 25.6 already requires that servers report failure as `sys_err_t` and invent no codes of their own, so this costs a protocol nothing it was not already doing. What it buys is that the kernel can tell a successful *transport* from a successful *request*, which the payload alone never allowed.

Two things depend on it. A tracing tool can report failed requests without understanding any protocol. And section 17.5's abort-on-failure becomes implementable for entries that send: without this field, a `FS_OP_WALK` for a file that does not exist is a completely successful `SYS_OP_IPC_SEND` as far as the kernel can see, so a linked chain would carry on and send a read on a connection that was never returned.

The kernel copies the field and attaches no meaning to it beyond zero and non-zero. It does not learn the protocol, in exactly the way it does not learn the cookie.

#### Sizes, and the one case that truncates

**The inline payload cannot truncate.** It is a fixed-size array in a fixed-size structure, copied whole, with `inline_len` saying how much of it is meaningful. A sender giving an `inline_len` above `SYS_IPC_INLINE_BYTES` is rejected with `SYS_ERR_INVALID_ARG` before anything is copied, rather than silently shortened.

**A grant is a handle to the sender's memory, and its size is data.** A server asked to fill a buffer smaller than the request it was given completes as much as fits and reports a short count in the protocol. That is a protocol outcome, not a kernel error, and no operation in this section returns a length the caller did not ask for.

**The one truncating case is the handle array.** On receive, `handle_capacity` is the caller's array size and `handle_count` is written with the count the message actually carries. A short array yields `SYS_ERR_TRUNCATED` and **moves nothing**, so the message can be received again with a larger array and no handle is stranded in between.

#### What moves, and when

**Handles in a request move into the receiving process at receive. Handles in a reply move into the client at reply.** In both cases the value changes across the boundary, per section 7.4, because a handle means something only in the table holding it. The source slot is invalidated rather than merely unused, so presenting the old value afterwards is refused rather than reaching the object. Moving requires `SYS_RIGHT_TRANSFER` on the handle.

**A grant is moved for the duration of the call and returns on reply.** By section 6.1 the sender may not touch the underlying memory between send and reply, and on Tier 2 that rule is enforced by unmapping rather than trusted. This is the uniform memory contract, and the rendezvous is the place it is most used.

### 12.5 When a Peer Dies

A rendezvous has two sides and they fail differently, because they are waiting on different things.

**A client blocked in `sys_msg_send()` is released with `SYS_ERR_OWNER_DEAD`** when the process owning the channel terminates. This applies in both blocked states: a SEND-blocked client whose server never existed long enough to receive, and a REPLY-blocked client whose server died holding its request, are released the same way. This is an error return, not a notification, and the distinction is forced rather than stylistic: the client is not receiving on that channel, it is waiting for a reply, so nothing delivered *to* the channel can reach it. A mechanism that only posted a pulse would leave every blocked client hanging on a dead server.

**A server holding an unanswered request learns through `SYS_PULSE_DEAD_OWNER`**, per section 10.5, because a server *is* at a receive point and can collect it. Its later reply, if it makes one anyway, returns `SYS_ERR_STALE`. Only a REPLY-blocked client's death needs announcing; a SEND-blocked client that dies is removed from a queue the server never looked at.

Both directions are therefore covered, and neither depends on a timeout. This matters most during cohort restart (section 18.2), where a supervisor is deliberately destroying processes that may have clients blocked on them and requires those clients to be released promptly rather than at the expiry of whatever deadline each happened to pass.

**A server learns nothing when a client merely closes a connection.** Destroying the connection object is a local operation in the client's table and tells the server nothing, so per-connection state is reclaimed on an explicit protocol close (section 25.2) and not before. `PROPOSALS.md` describes what changing that would cost, and section 25.7 describes the shape that avoids needing to.

### 12.6 The Shape of a Server

The kernel provides no server framework, and this section is what a server has to do without one.

**A server is a loop around `sys_msg_recv()`.** It receives, switches on the operation, and replies. The cookie tells it which of its objects the request is about; the client id tells it who to reply to; the payload or the grant carries the argument. Section 19.3 gives the same loop for a driver, and there is no difference between the two.

**Concurrency is receiving threads, and the count is a configuration decision.** Several threads may be blocked in `sys_msg_recv()` on one channel, and each rendezvous pairs one client with one of them. A server that wants to serve *n* requests at once has *n* receiving threads. There is no kernel worker pool, and there could not be one that preserved section 2.4, since a kernel thread serving a request is a thread whose priority is not the client's.

**A server that blocks onward stops serving on that thread, and the donation follows.** A filesystem receiving a read and sending to a block driver is REPLY-blocked on the driver, running at the client's donated priority, and carries that priority to the driver by section 12.3. This is the intended shape and needs no asynchrony. What it costs is a thread per outstanding request, which is the sizing decision above stated a second way.

**Receive and reply may be split across threads**, per section 12.2, so a server may instead receive on one thread and hand the client id to a worker. Nothing in the kernel distinguishes the two structures. Which is right depends on whether the server's work is bounded and short, in which case serving inline is simpler and faster, or long and variable, in which case a dispatcher keeps the channel drained.

**A server must not send to its own channel.** It would be SEND-blocked on a channel only it can receive on, with no thread left to receive. Section 10.3's process reentrancy mode is the related setting, and the general rule is that the connection graph among servers must be acyclic, since section 12.3 does not detect a cycle.

### 12.7 One Message, Several Operations

Nothing in this section says a message carries one operation, and for a request whose steps are known in advance it should not.

**The kernel does not know how many operations a message contains, and must not.** A message is a payload and a buffer, and what a server finds inside is between the client and that server. A batched request is therefore not a kernel feature at all: it is a protocol whose request structure happens to be a sequence. Section 25.7 defines the one for filesystems.

**The saving is rendezvous, and rendezvous is the expensive part.** Three requests to one server cost three rendezvous and six context switches. The same three operations in one message cost one rendezvous and two, and the server executes the whole sequence with its caches warm and without yielding to the client's scheduler between steps. On Tier 2 it also costs one grant setup rather than three.

**A batched request must abort at the first failure**, and the server is the one that aborts it, because the server is the only party that knows what the operations mean. The reply says which operation failed and why. A client that wanted the remaining operations attempted anyway is describing a different request and should send a different one.

**Intermediate results stay inside the server.** An operation in the sequence refers to what an earlier operation in the same sequence produced, by index rather than by handle. Nothing is moved back to the client for a step it did not ask to keep, which means a sequence that opens, reads and closes never puts a durable object in either table. Section 25.7 shows what that saves, which turns out to be more than the context switches.

**This is not the submission engine of section 17, and the two solve different costs.** A section 17 batch is executed by the kernel in the client's thread and collapses *traps*: three sends in one batch are still three rendezvous. A batched protocol request is executed by the server and collapses *rendezvous*. They compose, since a section 17 entry may send a message that itself carries a sequence, and neither depends on the other existing. Where only one is available, the protocol-level one is the one that pays for a request to a single server.

---

## 13. Pulses and Interrupts

Fences and pulses are both notification mechanisms, kept separate because they carry different things.

**Fences carry data-flow completion.** They have a producer, a consumer, a defined value, and a natural donation target. They answer "is this ready."

**Pulses carry out-of-band events.** Owner death, cancellation, and asynchronous signals have no buffer, no producer a waiter should donate to, and often no thread waiting when they occur. They answer "something happened." Expressing owner death as an errored fence would require inventing a producer for an event that has none.

### 13.1 Pulses

A pulse is a fixed-size, non-blocking, queued notification: a code and a value, with no payload and no reply. Sending never blocks, which makes it safe from contexts where blocking is not permitted.

Pulses are cheap. Eight bytes, no rendezvous, no address-space work, a bounded enqueue and possibly making a receiver runnable. They are the least expensive thing in this section.

What needs specifying is not cost but **capacity**. Under static pools a channel's pulse queue has fixed depth, so a policy is required when it fills. The queue is divided:

* **User pulses** occupy the general region. When it is full, the oldest is dropped and an overflow flag is set on the channel, which the receiver observes on its next receive.
* **System pulses** (`SYS_PULSE_DEAD_OWNER`) occupy a reserved region that user pulses cannot displace.

The reservation is required for correctness, not politeness. A dropped death notification leaks the resources of a dead client permanently, and a process that floods a channel with user pulses must not be able to cause that.

Pulses are delivered to a channel and collected by `sys_msg_recv()`, which reports through a flag whether it received a message or a pulse, so a server has one receive point for both.

### 13.2 The Interrupt Path

An interrupt service routine advances a timeline and returns. That is the whole of it.

```{=typst}
#figure(include "fig/interrupt.typ", caption: [The interrupt path: an ISR advances a timeline and returns.])
```

`sys_irq_attach()` binds an interrupt to a timeline handle carrying `SYS_RIGHT_SIGNAL`. Because interrupt delivery, DMA completion, timer expiry and pipeline stages are all timeline advances, a driver waits on one kind of object regardless of what it waits for.

**There is no opcode for it, which is an omission rather than a decision.**
Section 23.1 has every operation enter through one descriptor carrying an
opcode, and the list in section 26 does not include this one. Either the
list is incomplete or attaching an interrupt is not an operation, and the
first is far likelier. Until that is settled the binding is kernel
internal, which is tolerable only because nothing outside the kernel can
attach an interrupt yet.

**The handler must finish with the device before it advances anything.**
Advancing a timeline can make a waiter runnable and switch to it from
inside the handler, so a device left asserting is re-entered the moment
that thread enables interrupts, and the nested handler overwrites the
return state of the one still in progress. This is not a property of any
particular controller: it follows from the handler being allowed to
reschedule, and both ports had to learn it.

The ISR runs no driver logic, takes no lock a thread can hold, and performs no allocation.

### 13.3 Interrupt Latency, Stated Per Tier

**Worst-case interrupt latency is defined as the longest interrupts-disabled region plus the time to dispatch an interrupt to a user thread.** This definition is used because both terms are measurable on a given port, so the claim can be checked rather than asserted. Nothing here has been measured, and no figure is offered.

There is a genuine tension between analyzability and absolute latency, and this document resolves it differently per tier rather than pretending one answer serves both.

Disabling interrupts inside the kernel and bounding latency purely by preemption point placement gives a sound and completely analyzable result, but it imposes a floor: response cannot be faster than the longest disabled region, which puts latencies on the order of tens of cycles out of reach. Enabling interrupts with fine-grained internal locking removes that floor but makes sound worst-case analysis substantially harder.

* **Tier 0: interrupts are disabled inside the kernel**, and latency is bounded by preemption point placement. Cores are simple, often uniprocessor, and the kernel is small enough that the longest disabled region can be inspected exhaustively. This buys a kernel with no internal locking at all.
* **Tier 2: interrupts are enabled, with fine-grained locking.** Multiprocessing forces internal locking regardless, so the marginal cost is small, and removing the floor matters more on hardware fast enough for it to be the binding constraint.

Latency is therefore tier-specific and is a measured number per port rather than a general property of the architecture. This document states no such number, because none has been obtained.

Preemption points sit at batch entry boundaries, at message copy chunk boundaries, and at page table walk boundaries.

---

## 14. Time and Timers

### 14.1 There Is No Tick

**The system has no periodic timer interrupt.** The hardware timer is programmed one-shot to the earliest event the core knows about, which is the nearest of the next expiring timer, the next absolute deadline being waited on, and the quantum expiry of the running thread if it is a round-robin thread under section 11.1. When that interrupt is taken, the next event is computed and the timer is reprogrammed. A core with nothing to run and nothing pending programs no timer at all.

This follows from absolute deadlines rather than being an independent choice. A tick exists to give periodic code somewhere to run and to age a quantum, and section 14.3 has already removed the first use: waiting until an absolute instant is a property of the wait, not a poll that a tick has to service. What remains is quantum ageing, which is opt-in and per-thread, and one-shot programming serves it exactly.

Three things are bought, and they are the reasons the choice is made rather than pleasant consequences of it.

**Timer resolution is not quantised to a tick period.** A deadline is met to the resolution of the timer hardware. With a tick, every deadline is rounded up to the next tick boundary, and half a tick period is added to the worst case of every timed operation in the system. That term would appear in the figures of section 13.3 and would dominate them on any part with a conventional tick rate.

**Scheduling latency does not include a tick period.** A thread made runnable by an interrupt preempts at the next preemption point of section 11.3, which is a bounded distance measured in instructions. It does not wait for a tick. This is the distinction section 11.3 insists on, and a tick would erase it.

**An idle core is genuinely idle.** No interrupt arrives to tell a core that nothing has happened. On a part where the wake cost is significant this is the difference between an idle core costing nothing and an idle core costing a wake per tick, and on a battery-powered Tier 0 part it is the difference that matters most.

**One consequence is an implementation obligation rather than a property.**
If the hardware is armed for the earliest pending event, then every path
that adds, removes or advances a pending event has to recompute it, and
missing one leaves the timer armed for something that has passed or not
armed at all. That is easy to say and easy to get wrong in pieces, so it is
worth one place that decides what to arm and no other caller touching the
hardware.

The cost is equally specific. **Every scheduling decision that changes the earliest pending event must reprogram the timer**, which on some parts is a slow write to a peripheral. That write is on the dispatch path and must be counted in the figures of section 13.3. A port whose timer is genuinely expensive to reprogram may find a tick cheaper, and that is a real trade, but it is a divergence to be measured and stated rather than assumed: the latency statements in this document are made without a tick period in them.

**The timer is a scheduling mechanism, not a clock.** Reading the current time does not involve it at all: uptime is read from the system page with no system call, per section 6.2, and the two are related only in that both derive from the same monotonic source. Nothing in section 14.5 can perturb either, which is what makes clock adjustment safe for real-time behaviour.


### 14.2 Timers Advance Timelines

A timer binds a timeline and advances it at each deadline. There is no timer pulse.

For a **one-shot** timer the timeline advances once at its deadline.

For a **periodic** timer the counter value is the activation count, which gives overrun detection for free. A task that intended to wait for activation `N+1` and finds the counter already at `N+3` has missed two activations and knows it without any additional mechanism.

Timer destruction closes the kernel's signal right, which force-signals the timeline in error state under section 9.3 rule 2. No new rule is needed.

**Phase is absolute.**

> The Nth activation of a periodic timer occurs at `first + N * period`, and the counter always holds the number of activations that should have occurred by the current time.

Phase is measured from the timer's origin, never from the previous delivery. Scheduling delay, interrupt latency and a late waiter do not shift subsequent activations.

This is what makes the counter meaningful. Value N corresponds to time `first + N * period` exactly, so a task can convert between activation number and time arithmetically without reading a clock. Overrun is then a precise measurement rather than an estimate: a task that wanted `N+1` and resumes at `N+3` missed two activations and knows when each should have occurred. Independent periodic tasks also hold their phase relationships permanently, so two tasks of equal period started together stay aligned, and tasks whose periods are related keep that relation.

The alternative, scheduling each activation one period after the previous *delivery*, accumulates the delivery jitter into the phase. A timer averaging even a small lateness runs measurably slow, the counter ceases to correspond to any particular time, and overrun becomes undetectable because nothing was strictly missed. That would remove the property this construction exists to provide.

**Catching up is a single counter update.** After a stall the kernel sets the counter to the value the current time implies. There is no backlog of missed signals to deliver and no queue to drain, which is the failure mode that makes catch-up expensive where each activation is a separate event.

**Whether to catch up or to skip is the task's choice, per iteration.** The counter carries enough information to decide either way, so the kernel offers no policy:

```c
/* Process every activation, catching up after a stall. */
for (uint64_t n = 1; ; n++)
    sys_timeline_wait(timer_tl, n, SYS_DEADLINE_NEVER);

/* Skip missed activations and resynchronize to the current period. */
for (;;) {
    uint64_t now;
    sys_timeline_query(timer_tl, &now, NULL);
    sys_timeline_wait(timer_tl, now + 1, SYS_DEADLINE_NEVER);
}
```

### 14.3 Absolute Deadlines

Every blocking call takes an **absolute deadline** on the system timebase, not a relative duration.

Relative timeouts drift. A wait that is interrupted and restarted with the same relative value extends its own deadline by however long it had already waited, so a periodic task accumulates phase error proportional to its own scheduling jitter, which is precisely the error real-time code exists to avoid. Absolute deadlines have no such failure mode and make restart-after-interruption exact.

The cost is a clock read when constructing a deadline, and section 6.2 is what makes that affordable: `sys_syspage_uptime()` reads the shared page with no syscall. Relative helpers live in libc.

Two sentinels are defined: `SYS_DEADLINE_NOW` polls without blocking, and `SYS_DEADLINE_NEVER` blocks indefinitely.

### 14.4 Timers and Deadlines Are for Different Things

The two mechanisms look similar and are not interchangeable, so the distinction is stated directly.

**Timers are for activation.** A periodic task waits on its timer's timeline and passes no deadline at all.

**Deadlines are for failure detection.** A deadline on a send or a fence wait expresses how long the caller is willing to wait before treating the operation as failed.

A periodic task that also wants a watchdog uses both: it waits on the timer timeline for activation, and passes deadlines on the operations it performs within each period.

### 14.5 Calendar Time

Everything above is monotonic. Uptime never steps, never runs backwards, and is unaffected by any correction to the system's idea of the date. All scheduling, all deadlines and all timers are defined against it, and nothing in the kernel depends on calendar time.

Calendar time is nonetheless needed by applications, and is provided without a syscall and without a kernel clock driver. The system page carries a **UTC offset** written by a userspace time service:

```
calendar time = uptime_ticks / ticks_per_sec + utc_offset
```

The kernel neither computes nor validates the offset. It owns no real-time clock driver, which stays outside the kernel like every other driver, and it does no timezone or leap-second handling. Its entire role is to hold a value that one authorized service writes and every process can read cheaply.

Three properties follow, and the third is the reason for the arrangement.

**Reading the date costs no IPC.** A process computes it from the system page it already has mapped, the same way it reads uptime.

**Adjusting the clock is one write.** A time service that has synchronized against an external source updates the offset. There is no need to walk running timers or wake anything.

**Clock adjustment cannot disturb real-time behaviour.** Because timers and deadlines are defined against uptime and never against calendar time, a correction of any size, forwards or backwards, cannot cause a periodic task to miss an activation, fire twice, or block for an unexpected duration. Systems that schedule against a settable clock have to defend against exactly that, and here it is structurally impossible.

The offset is protected by the same seqlock as `uptime_ticks` and must be read through `sys_syspage_utc_offset()`, since it is a 64-bit value that a 32-bit target cannot read atomically.

---

## 15. Program Loading and Demand Paging

### 15.1 What Mapping Is For

Mapping exists to bring in **read-only shared library and program text**. It is not a general-purpose memory mapping facility, and in particular it does not provide the semantics of POSIX `mmap`.

Three things follow, and each removes rather than adds machinery.

* **No copy-on-write.** Writable segments are copied eagerly at load. A write fault against a demand-paged region is a program error, not a request to a pager.
* **No swap.** Without dirty tracking the kernel cannot know whether an evicted writable page needs writing back, so writable memory is never reclaimed. Clean pages remain reclaimable because they can be re-read.
* **The pager only ever supplies read-only pages.** The fault message's access field is `READ` or `EXEC`, never `WRITE`.

This is the right position for a real-time system independently of the simplification, because paging out a real-time task's working set is unacceptable regardless. The consequence to state plainly is that the total writable working set of the system must fit in RAM. Demand paging is a mechanism for loading code lazily, not for overcommitting memory.

### 15.2 Loading on Tier 0

There is no MMU and therefore no demand paging. **Text and rodata execute in place from flash**, and only writable data is copied to RAM at load. This is standard practice on parts that have far more flash than RAM, and Tier 0 already mandates position-independent code, so the requirement costs nothing extra.

A **RAM loader** is available as a compile-time feature for images that must run from RAM, whether for speed or because they arrive at runtime rather than living in flash.

The two paths differ in teardown, which is the part worth specifying. An XIP process owns no text memory, so its teardown reclaims only its data segment, stacks, and kernel objects. A RAM-loaded process holds a region owned by the **loader service**, which must reclaim it. The loader therefore waits on the process's completion timeline from section 10.4 and releases the region when it advances. Reclaim is a userspace responsibility, and the mechanism it uses is the one already specified for supervision.

### 15.3 Faults Are Messages the Faulting Thread Sends

Tier 2 only. On Tier 0 the paging calls return `SYS_ERR_NOT_SUPPORTED`.

Every thread has a designated **pager**, held as a channel handle and inherited at spawn unless overridden.

On a page fault the kernel synthesizes a message that appears to have been **sent by the faulting thread** to its pager, and places the faulting thread in the ordinary blocked-sending state. The pager receives it with a normal `sys_msg_recv()` and replies with a grant covering the faulting address. The reply resumes the thread, which retries the faulting instruction.

```{=typst}
#figure(include "fig/pagefault.typ", caption: [A page fault is a message the faulting thread sends to its pager.])
```

Modelling the fault as a message the faulting thread *sends*, rather than as an upcall delivered *into* a pager, is what allows **the rest of this document to apply unchanged.** Reply-by-thread-id works because the faulting thread is an ordinary blocked sender. The blocking invariant holds because it is blocked on exactly one object. Handle transfer delivers the grant. Cancellation reaches it like any other blocked thread.

Most importantly, **priority inheritance applies without modification.** A high-priority thread's page fault is serviced at high priority, because the faulting thread donates to its pager exactly as any client donates to any server. In a real-time system this property is worth a great deal, and here it is a consequence of the formulation rather than a mechanism that had to be added.

The alternative formulation, delivering page requests asynchronously to a queue that a pager services at its own priority, is rejected. It severs the priority relationship between the faulting thread and the work that unblocks it, so a high-priority thread's fault would be serviced at whatever priority the pager happened to run at.

### 15.4 The Pager Chain Must Terminate

A pager can itself fault, which makes the pager relation a chain. If that chain has a cycle, or ends at a process that can fault, the system deadlocks: the classic form is a storage driver that faults on its own text and must reach the storage device it is itself responsible for driving in order to resolve the fault.

Three rules follow.

1. **The pager chain is finite and terminates in a memory-locked process with no pager.** The root pager, established at boot, is that terminator.
2. **Every process on the paging path is memory-locked**, including pagers, storage cohorts, and any driver a pager depends on. `sys_mem_lock()` requires a right held only by processes the root task designates.
3. **A memory-locked process that faults is a fatal error**, not a recoverable one. It indicates a configuration mistake, and failing loudly at integration time is better than deadlocking in the field.

The kernel enforces rule 1 at `sys_thread_set_pager()` by walking the chain and rejecting a cycle or an unterminated chain with `SYS_ERR_DEADLOCK`.

---

## 16. Faults

Section 15.3 makes a page fault a message the faulting thread sends. That is the recoverable case and it is the exception. This section says what the other faults are, what happens to them, and why the default is death.

### 16.1 The Kinds

A fault is a synchronous exception raised by the hardware while a thread is executing, attributable to that thread and to the instruction it was executing. The kinds are enumerated because an implementation has to map every exception its hardware can raise onto one of them, and because a kind that is not in this list has nowhere to go.

| Kind | Raised by | Recoverable |
| --- | --- | --- |
| `SYS_FAULT_PAGE` | A reference to a mapped-but-not-present address | Yes, by the pager of section 15.3 |
| `SYS_FAULT_ACCESS` | A reference to an unmapped address, or one the thread lacks rights to | No |
| `SYS_FAULT_ALIGN` | A reference the hardware requires to be aligned and is not | No |
| `SYS_FAULT_INSTRUCTION` | An undefined, privileged or otherwise illegal instruction | No |
| `SYS_FAULT_ARITH` | An arithmetic exception the hardware raises, such as integer division by zero | No |
| `SYS_FAULT_DEBUG` | A breakpoint or watchpoint | No |
| `SYS_FAULT_EXTERNAL` | A bus error or external abort | No |

"Recoverable" here means recoverable *by the mechanism this document provides*. `SYS_FAULT_PAGE` has a pager that can supply the missing grant and a defined retry. The others have no such mechanism, which is a statement about the kernel rather than about the fault: a handler may still resolve one, but it must do so by changing the thread's state rather than by asking the kernel to retry.

**`SYS_FAULT_EXTERNAL` may be imprecise.** On most parts an external abort can be reported after the instruction that caused it has retired, so the reported program counter is not the one to blame and the thread's registers are not a coherent snapshot of anything. It is enumerated separately for exactly this reason: a handler must treat it as a report that the address space is no longer trustworthy, not as a fault it can analyse. This is the one kind for which resumption is not merely unsupported but meaningless.

**Stack overflow is not a kind.** It arrives as `SYS_FAULT_ACCESS` against the guard region below the stack, per section 10.2. There is no separate kind because the kernel does not know which region is a stack: it was given a region by the creator and nothing more. A handler that wants to distinguish overflow from any other access fault compares the faulting address against the stack bounds it chose.

### 16.2 Fatal by Default

**A fault terminates the faulting thread.** Its exit status records the fault kind, the faulting address, and the program counter, readable through `sys_thread_status()`. Its completion timeline advances, exactly as if it had exited, so everything already built on that timeline works without addition: a joiner is released, a supervisor holding the completion timeline is woken per section 18.3, and any server holding one of its requests receives `SYS_PULSE_DEAD_OWNER` per section 10.5.

This is the default because it is the correct behaviour for almost every program in this system, and because the alternative defaults are worse in ways that are easy to miss.

**Terminating the process instead would be too coarse.** A server whose worker thread faults on one malformed request should lose the request, not the connection state of every other client. Terminating the thread and letting the supervisor decide keeps the policy where the knowledge is.

**Suspending the thread instead would be too generous.** A faulted thread that is merely stopped still holds its stack, its handles, its pool slot, and whatever locks it was holding, and it holds them until something notices. In a statically bounded system that is a slow leak with no upper bound on how long it lasts. Death releases the resources and advances the timeline that tells somebody about it.

**A faulting thread that holds donated priority loses it on death**, and the chain of section 21 unwinds exactly as it does for any other exit. There is no special case: this is the reason exit and fault share a path.

**A fault taken by the last thread of a process is still a thread fault.** The process is left with no threads, which is a legitimate state and is not itself an error. It is reaped when the last handle to it closes, per section 10.4. A supervisor that wants a process to die when its last thread does must arrange it, and holding the process's completion timeline is how.

### 16.3 The Handler, Which Is Opt-In

A process may nominate a **fault handler** at spawn, as a channel handle. A process that nominates none, which is the common case, gets section 16.2 and nothing else.

Where a handler is installed, a fault becomes a message the faulting thread sends to it, in the same shape and with the same semantics as the page fault of section 15.3. The faulting thread is an ordinary blocked sender; the handler receives with `sys_msg_recv()`; the blocking invariant holds; cancellation reaches the thread as it reaches any other. Nothing new is introduced, which is the whole reason the page fault was modelled this way in the first place.

The handler has two replies available.

**Reply to resume.** The thread retries the faulting instruction. This is only useful where the handler has changed something that makes the retry succeed, which for `SYS_FAULT_PAGE` means supplying a grant and for everything else means altering the thread's registers through its handle. A resume reply on `SYS_FAULT_EXTERNAL` returns `SYS_ERR_INVALID`, because there is nothing coherent to resume to.

**Reply to terminate, or do not reply at all.** The thread dies as in section 16.2. Declining to reply is not a resource leak: the handler holds the thread blocked, and cancelling it or killing the process resolves it.

Three restrictions exist, and each removes a way the mechanism could otherwise fail exactly when it is needed.

**A fault in the fault handler is fatal, always.** The handler's own threads get section 16.2 regardless of what the process nominated. Without this, a handler that faults sends itself a message and the system has a livelock in its recovery path.

**A fault while the handler is unreachable is fatal.** If the handler channel is closed or its receiving process is gone, the thread dies rather than blocking forever on a send that nobody will receive. A handler that has itself died is precisely the moment the default has to reassert itself.

**The handler is nominated at spawn and cannot be changed.** A process cannot install a fault handler on itself later, and cannot change the one it was given. This follows from the same reasoning as the completion timeline in section 10.4: a process that could install its own fault handler could catch and hide the faults its supervisor exists to observe.

The handler exists for two identified users and is not a general facility. A **debugger** needs `SYS_FAULT_DEBUG` and needs to inspect and resume. A **language runtime** needs to turn a hardware fault into whatever its language calls an exception, which means unwinding to a known point rather than retrying. Both need the same mechanism, and neither is served by the kernel deciding anything about their policy.

### 16.4 Faults and the Tiers

The split of section 6 is nowhere more visible than here, and the difference is one of detection rather than of interface.

**Tier 2** has an MMU. Every kind in section 16.1 is detected, `SYS_FAULT_PAGE` is recoverable, and a guard region makes stack overflow an access fault at a known address.

**Tier 0** has neither. `SYS_FAULT_INSTRUCTION`, `SYS_FAULT_ALIGN` and `SYS_FAULT_ARITH` are still raised by the core and still reported. Memory faults are largely not detectable, and a stray write corrupts whatever it reaches. The interface does not change and the fault kinds do not change; what changes is that the weaker tier cannot catch its own violations, which section 6 says plainly and which is the price of the same source running across both.

**A port must map every exception its hardware raises onto a kind in section 16.1, and must state the mapping.** An exception with no mapping is an exception whose behaviour is undefined, and section 27 requires the table.

### 16.5 Faults Inside a Cohort

Section 18.2 states that a fault in one cohort member destroys the cohort. That rule takes precedence over everything in this section, and the ordering matters.

**A cohort member's fault handler, if it has one, runs first.** If the handler resumes the thread, no cohort fault has occurred and section 18.2 does not apply. If the thread dies, the cohort dies with it.

This is not a softening of section 18.2. Cohort members share a protection domain, so a member that has faulted may already have corrupted its peers, and the cohort teardown exists because that corruption cannot be bounded. What the ordering buys is the case where the fault was contained and provably so, which a language runtime unwinding its own stack can establish and the kernel cannot. A cohort that installs a fault handler is asserting that it can make that judgement, and section 18.2 is what happens when it declines to.

---

## 17. The Submission Engine

### 17.1 What It Is For

**This is the least conventional mechanism in the system, and it is deliberately kept separable.** Sections 5 through 16 describe a complete system in which every operation is submitted one at a time, and none of them depends on a submission being longer than one entry. This section is what happens when the count in section 23.1 exceeds one.

The separability is worth preserving for two reasons. The mechanism is novel enough to be worth building and worth being wrong about, so it should be removable without disturbing anything else. And it is the hardest thing here to explain to someone still learning what a system call is, which makes it the wrong place to start and a poor thing to have woven through the rest.

The engine lets one thread submit a dependency-ordered set of operations that the kernel executes in that thread's context, at that thread's priority, with no worker pool anywhere.

The absence of a worker pool is the point. Asynchronous submission rings execute work on kernel threads, severing the connection between the requesting thread's priority and the priority at which its work runs. That is acceptable in a throughput-oriented general-purpose kernel and unacceptable here.

### 17.2 Structure

A batch is a linear array of entries with no branches, no loops, and no bytecode.

**There is deliberately no verified bytecode.** A richer submission language, with branches and an in-kernel verifier, would allow a batch to make decisions and to double as a syscall filter. It is rejected for three reasons.

Confinement here does not come from program text. A process is confined by which handles its table contains, and a submitted program that operates on a handle supplied as an argument reaches anything the caller already holds. Filtering the program would therefore add no authority the handle table does not already control.

What a verifier would genuinely provide is memory safety of the submitted program and a static bound on its cost. A branchless linear array provides both by construction: its cost is the sum of its entries, and validation is per-entry argument checking with no reachability analysis.

The verifier itself would become the largest security-critical component in the kernel. On Tier 0, where there is no hardware protection to fall back on, a defect in it is total compromise. Removing branches removes the need for it entirely.

Dependencies that would have required branches are expressed as fences instead. **The dependency becomes data rather than code.** That substitution is what makes the branchless form sufficient rather than merely simpler.

### 17.3 Op Flavors

**Blocking ops** complete before the next entry executes: `IPC_SEND`, `IPC_RECV`, `FENCE_WAIT`. These are the only entries that can block, and any wall-clock analysis concerns these and nothing else.

**Issuing ops** start work and return immediately, writing a timeline handle to an output register: `DMA_ISSUE`, and the issuing forms of read and write.

Issuing ops are where concurrency comes from. In an in-context engine there is one thread, so entries execute strictly in order and the kernel provides no parallelism. Concurrency comes from hardware. A batch issues three transfers, collects three timelines, then waits on each; three transfers are in flight simultaneously, the thread's own context executed all of it, and priority inheritance was never broken.

**There is no issuing form of send, so this concurrency does not reach servers.** `SYS_OP_IPC_SEND` blocks, which means a batch cannot start two requests to a filesystem and wait on both. The concurrency above is hardware concurrency and applies to devices. A client wanting two file reads at once uses two threads, which is the same answer section 19.3 gives a driver and the same cost `PROPOSALS.md` names wait-sets for.

An issuing send is not obviously incoherent, since the reply would land in the entry's argument structure, which was validated once at submission and is exactly the fixed buffer a reply needs. What it would break is the description in section 12.2 rather than the mechanism: a thread with an outstanding issuing send is not blocked on it, so the blocked client thread can no longer be the queue slot, and something else has to hold the reply's destination. That is a new object or a new thread state, and it is not a small change. `PROPOSALS.md` carries it.

```{=typst}
#figure(include "fig/batch.typ", caption: [Issuing ops put three transfers in flight from one thread, with no worker pool and no broken priority relationship.])
```

### 17.4 Registers

The engine provides `SYS_BATCH_REGS` typed registers, private to one execution and zeroed at entry. Each holds either a handle or a scalar and is tagged with which. An entry reading a register as a handle when it contains a scalar fails with `SYS_ERR_INVALID_ARG` rather than fabricating a handle from an integer.

The tagging is load-bearing. Without it a batch could construct a handle value arithmetically, defeating the handle table's unforgeability.

**A reply's moved handle is written to `reg_out`.** `SYS_OP_IPC_SEND` is the second writer of a register, alongside the issuing ops, and without it a batch cannot use the result of a request it made. The case this exists for is a walk returning a connection that the next entry reads as its `target`. A reply carrying no handle leaves the register empty, and the dependent entry then fails on the tag check above rather than on a fabricated value, which is the failure mode wanted.

### 17.5 Chaining and Error Semantics

Each entry carries a `LINK` flag. A chain of linked entries runs sequentially; the first failure cancels the remainder of that chain with `SYS_ERR_CANCELED`; the chain ends at the first entry without the flag. Several independent chains may occupy one batch.

Two properties of `LINK` are easily assumed and both are false here.

**Chains are not graphs.** `LINK` produces a set of disjoint linear paths. It cannot express a join, where one op depends on two others, or a fork, where two ops depend on one. Those are expressed with fences instead: a fork is one op signalling a timeline that two later chains wait on, and a join is several producers advancing a timeline that a consumer waits on at the final value. Fences also cross batch and process boundaries, which `LINK` cannot, so the two mechanisms together span a strictly larger space than chaining alone.

**Independent chains do not run concurrently.** The engine executes in one thread, so entries run strictly in submission order whether or not they are linked. `LINK` means abort-on-error grouping and nothing else. Concurrency comes only from issuing ops, per section 17.3.

On abort, every timeline the batch was going to signal is force-signalled in error state.

**That set is always determinable, and the branchless form is why.** The kernel must identify timelines a batch would have signalled without executing the remaining entries. An entry naming its target literally is visible by inspection. An entry taking its target from a register can only have got it from an earlier entry in the same batch, because registers are zeroed at entry and written only by an entry that has already run. If that earlier entry ran, the kernel holds the value; if it did not, the register is empty and no timeline exists for anything to be waiting on. Both cases are covered, with no execution required.

A language with branches would lose this. Whether a given signal is reachable would depend on values not yet computed, so the kernel could not enumerate the set without running the batch, and the guarantee could not be made at all.

**A failure reported by a server aborts the chain too, and this needs the status field of section 12.4.** Without it, an entry that sends is successful whenever the message was delivered, whatever the server said, so a chain of walk, read and close would carry on past a walk that found nothing and send a read on a connection that was never returned. With it, the kernel copies the reply's `status` into the entry's status field and treats non-zero as failure, learning nothing about what the value means. `LINK` covers protocol failures and transport failures the same way, which is the only behaviour a client would predict.

**An aborted chain can leave state behind in another process, and the engine does not clean it up.** If a walk succeeds and the read that follows fails, the close never runs. The connection exists, its handle sits in a register that is zeroed when the batch ends, and the client never learns the value. The server still holds the record, and section 25.2 reclaims that record on an explicit close and on nothing else, so a slot in a statically sized pool is leaked in a process that did nothing wrong.

The engine cannot fix this by closing register-held handles at exit, because destroying a connection object tells the server nothing (section 12.5). The choices are a deferred-entry flag, meaning "run even if the chain aborted, provided the register holds a value", which stays inside the branchless argument because the predicate is whether a register was written; or connection death notification, which `PROPOSALS.md` costs out; or arranging not to have the problem, which is what section 25.7 does by keeping the intermediate node inside the server. **The third is why a compound protocol request is the better tool for a batched open, and this is stated here rather than there because it is a limit of the engine.**

### 17.6 Status Reporting

The kernel writes each entry's result into a status field in the entry itself.

Results are written into the entry array rather than to per-entry user-supplied pointers. The entry array is validated once when the batch is submitted, so a status write needs no further checking, whereas an arbitrary pointer per entry would require validating a user address for every write in the middle of execution. On Tier 0 there is no meaningful validation to perform against such a pointer at all. Keeping status in the entry also keeps each result adjacent to the op that produced it.

### 17.7 Entry Points and Preemption

Batches may be submitted as a prepared program object, validated once and re-executed many times, or through a **direct-invoke shim** for a single op.

The shim matters more than it appears. It means `sys_msg_send()` is a one-entry batch, so **every operation has exactly one implementation** rather than a batch path and a separate syscall path that must be kept in agreement. It also keeps trivial calls cheap, and keeps single operations legible to tracing tools that a prepared program would obscure.

`sys_batch_exec` bounds `count` at `SYS_BATCH_MAX_ENTRIES` and rejects a larger submission with `SYS_ERR_INVALID_ARG` before executing any entry. **Preemption points sit at entry boundaries**, so submitting a batch never extends the kernel's non-preemptible section beyond the cost of one entry. On Tier 0 this is the primary determinant of interrupt latency.

---



## 18. Driver Cohorts

Related drivers are grouped into a cohort sharing one protection domain, trading fault isolation between members for zero-copy data paths.

A cohort is simultaneously **one protection domain, one recovery unit, and one scheduling node**, and on the paging path it is also memory-locked.

```{=typst}
#figure(include "fig/cohort.typ", caption: [Members of a cohort exchange scatter-gather lists without crossing a protection boundary.])
```

### 18.1 Membership

Cohort membership is fixed at system configuration time and cannot change at runtime. This is a security property, not an implementation convenience: joining a cohort grants access to every other member's memory, so a runtime join would be a privilege escalation primitive and the capability authorizing it would become the most valuable object in the system.

### 18.2 Fault Recovery

**The cohort is the recovery unit.** If any member faults, the entire cohort is torn down and restarted.

Per-process recovery is unsound for cohort members and must not be attempted. Members share a protection domain, so a member that faults may have corrupted any peer's state arbitrarily before doing so, and nothing in the system can establish that it did not. Restarting only the faulting member leaves the survivors in a state no one can vouch for.

The cost is real and should be stated plainly: a bug in a minor driver takes down the entire storage stack. That is the price of the zero-copy trade, and it is why cohort membership deserves attention rather than being a convenience for grouping related code. Drivers that do not need zero-copy data paths with each other should not share a cohort.

On teardown the kernel revokes every handle held by every member, force-signals their timelines in error state, releases clients blocked sending to them with `SYS_ERR_OWNER_DEAD`, and delivers `SYS_PULSE_DEAD_OWNER` to every channel with an outstanding rendezvous. Section 12.5 covers why the two directions need different mechanisms.

```{=typst}
#figure(include "fig/cohortfault.typ", caption: [A cohort fault. Member B is torn down although it did not fault, every outside party is released with an error rather than a timeout, and the supervisor decides what happens next.])
```

The figure makes the cost of the cohort trade concrete: **B is destroyed having done nothing wrong.** That is the whole content of the recovery-unit rule, and it is why cohort membership is a decision to be made deliberately rather than a convenient way to group related code.

Note also that every party outside the cohort is released by an explicit event, not by a deadline. A client blocked on a dying member returns immediately, and a supervisor waiting on the completion timeline is woken rather than polling. Restart latency is therefore bounded by the teardown itself, and a system whose recovery time depended on whatever deadlines its clients happened to pass would not be one worth analyzing.

### 18.3 Supervision

**Restart is a userspace responsibility and the kernel holds no restart policy.**

The mechanism is the one from section 10.4: a cohort's parent waits on its completion timeline and relaunches it. That is all the kernel provides. Whether the supervisor is the root process, a single system-wide service, or a hierarchy of monitors with per-cohort policies and restart backoff, is an implementation choice this document deliberately does not make.

Keeping policy out of the kernel matters here because restart policy is exactly the kind of thing that varies per deployment. A medical device and a set-top box want different answers, and neither answer belongs in a kernel that is trying to be small enough to analyze.

### 18.4 Scatter-Gather and Lazy Mapping

Members allocate from a shared physical pool and pass `sys_sg_entry_t` arrays directly, without crossing a protection boundary.

Where a scatter-gather message *does* cross a protection boundary, its elements are made available in the receiver's address space on receive, but the contents are **mapped on demand as the receiver references them**. A server that forwards a message without inspecting its contents, such as a protocol layer prepending a header, never maps the payload at all. For a network stack this removes most of the mapping cost from the forwarding path. **This applies to Tier 2 only**, since it rests on the fault-driven mapping of section 15.3, which requires an MMU. On Tier 0 the elements are simply available, and there is nothing to defer.

`sys_sg_build()` takes a grant handle rather than a raw virtual address, and takes the caller's array capacity along with an out-parameter for the required entry count. Both are required: without a capacity the kernel cannot bound its writes, and without the out-count the caller cannot discover how large an array it needed. `SYS_ERR_TRUNCATED` with a valid out-count lets the caller retry against a correct allocation.

---

## 19. Writing a Driver

Everything a driver needs has now been specified, but it has been specified one mechanism at a time. This section assembles them. It introduces no new interface and defines nothing that is not already defined elsewhere; a reader who wants only the mechanisms can skip it. It exists because the shape of a driver is the clearest evidence for or against the claims in section 2, and because a design that never shows its mechanisms working together is asking every reader to guess the same thing separately.

### 19.1 A Driver Is an Ordinary Process

There is no driver model, no driver API, and no privileged mode a driver runs in. A driver is a process that happens to hold three kinds of handle that most processes do not: a grant covering its device registers, an interrupt handle, and a channel it serves requests on. It is spawned by `sys_proc_spawn()` like anything else, it is scheduled like anything else, and it faults and dies like anything else.

This is a consequence of decisions already made rather than a goal pursued for its own sake. Section 20 removed the ambient root, so a driver cannot reach a device it was not given. Section 7 made authority a handle, so holding the device is holding a handle. Section 13.2 made interrupt delivery a timeline advance, so waiting for the device is the same wait as any other. Once those three hold, there is nothing left for a driver model to be.

### 19.2 What a Driver Is Given at Spawn

A driver receives everything it needs in its spawn call and acquires nothing afterwards. The list is short and complete.

**A grant covering its device registers.** Memory-mapped device registers are memory, and the authority over them is a grant like any other, per section 15.1. On Tier 2 this is a mapping with device attributes; on Tier 0 it is a region the process may already address, and the grant is what makes the permission explicit rather than what creates it.

**An interrupt handle, already attached to a timeline.** The parent calls `sys_irq_attach()` and passes the timeline handle to the child, or passes the interrupt handle and lets the child attach. The first is preferable: it keeps the authority to attach an interrupt in the launcher rather than in the driver, and a driver that cannot attach interrupts cannot attach somebody else's.

**A channel to receive requests on.** This is what its clients hold the other end of.

**A namespace array, if it serves names.** Per section 20.4, a driver that registers itself under a name receives the directory handle to register in, and receives no other directory.

**Its cohort membership, if any.** Fixed at configuration time per section 18.1, not requested at runtime.

Nothing here is discovered. There is no probe, no bus walk in the kernel, and no device tree the kernel parses. Whatever assembles the system reads the platform description and turns it into spawn calls, which is the same arrangement section 20 makes for names: the kernel holds no registry, so there is nothing to enumerate and nothing to protect.

### 19.3 The Shape of the Loop

A driver is a request loop and an interrupt wait, and how they are combined is the only structural decision its author makes.

The single-threaded form is the one to reach for first. The driver waits on its channel, receives a request, programs the device, waits on the interrupt timeline, and replies. It is correct, it is trivially analysable, and it serves one request at a time.

Where that is not enough, the shape that follows from this document is **two threads and no shared state**: a request thread blocked in `sys_msg_recv()`, and an interrupt thread blocked on the interrupt timeline. The blocking invariant of section 5 forbids one thread waiting on both, so where a driver must be responsive to a client while a transfer is in flight, it uses two threads. This is the concrete cost of having no wait-set, and it is the case `PROPOSALS.md` names wait-sets for.

Three properties of that arrangement come free and are worth naming, because a driver author who does not know they are free will build them.

**The interrupt thread does no device work at the interrupt priority.** The ISR advanced a timeline and returned, per section 13.2. Whatever the interrupt thread then does runs at its own priority and is preemptible like any other thread. There is no bottom half to schedule and no deferred work queue, because the deferral already happened when the ISR returned.

**A slow client cannot stall the device.** The two threads block on different objects and neither waits for the other.

**Priority follows the request.** A client at priority 40 sending to the driver donates to the driver's request thread for the duration of the rendezvous, per section 12.3. A driver serving two clients of different priorities does not need to know their priorities, sort them, or implement a queue discipline. It receives in donated-priority order because the kernel already ordered its senders.

### 19.4 Where the Data Goes

A driver that copies is a driver with a copy in its worst case, so the design provides for not copying, and which mechanism applies depends on the boundary being crossed.

**Across a cohort boundary there is no copy.** Members share a protection domain, so a buffer handed from the filesystem to the block driver is handed as an address, per section 18. This is the whole reason cohorts exist and the reason the trade in section 2.6 is worth making.

**Across a process boundary the buffer is granted, not copied.** The client grants the driver access to its buffer for the duration and revokes it afterward. Section 18.4 covers the scatter-gather case, where a request names many discontiguous regions.

**Nothing is mapped that is not being used.** A grant is authority over a region, and lazy mapping means the pages are attached when touched rather than at grant time, per section 18.4.

### 19.5 When the Device Misbehaves

A driver's failure handling is the supervision of section 18.3 and the faults of section 16, and there is nothing else.

**A device that does not respond is a timeout, and a timeout is a deadline.** The driver waits on the interrupt timeline with an absolute deadline per section 14.3. Expiry is a deadline miss it can act on, not an error the kernel invents.

**A driver that faults dies, and its supervisor restarts it.** If it is in a cohort, the cohort restarts, per section 18.2. Clients discover this through the completion timeline they hold, or through `SYS_ERR_STALE` on a reply to a driver that no longer exists.

**A driver that wedges is cancelled from outside.** `sys_thread_cancel()` breaks it out of whatever it is blocked on, per section 10.5, and the closed unblock list of section 10.6 is what guarantees that this always works. A supervisor never has to know which of the two waits a wedged driver is sitting in.

**A restarted driver must reprogram its device.** The kernel restores no device state, because it knows of none. This is where the restart policy of section 18.2 stops being free: a driver whose device cannot be safely reinitialised from an unknown state is a driver that cannot be restarted, and that is a property of the hardware which the system design has to accommodate rather than one this document can remove.

---

## 20. Namespaces

**The mount table lives in the C library. The kernel has no namespace syscalls.**

A kernel-resident mount table would require mount, unmount and resolve syscalls, and a generation epoch to invalidate cached resolutions. None of it is necessary.

### 20.1 Why This Is Safe

The instinct that a kernel table is needed for sandboxing is wrong, and the capability model is why. **Names are not authority. Handles are.**

A sandboxed process is confined by the contents of its handle table, which its parent populated at spawn. A userspace mount table only maps path prefixes onto handles the process already holds. A process that ignores its own mount table and uses its handles directly gains nothing, because it could already use those handles. There is no authority in the name, so there is nothing for the kernel to protect.

### 20.2 What This Deletes

Three syscalls, the epoch mechanism, and a batch opcode. Path resolution becomes ordinary library code walking a local table, costing no syscall and needing no invalidation protocol, because the library owns the table and knows when it changed.

Server death is still handled, by the handle generation counter rather than an epoch: a cached handle to a dead server's channel fails with `SYS_ERR_STALE` on use, and the library re-resolves.

The cost is that a batch cannot resolve a path in kernel context. A submission resolves in userspace and passes handles. Given that resolution is now free, this is arguably an improvement.

It also helps section 17. Since the batch has no resolve opcode, a program already holding directory handles submits a handle and a short relative path with no resolution step, which makes prebuilt submissions more reusable.

### 20.3 Naming Is Handle-Relative

**There are no absolute paths in the protocol, and there is no global filesystem namespace.** Every operation names a directory handle and a path relative to it.

This is not a change to what servers see, since a message to a server has always carried a handle plus a relative path. Making it the shape of the interface rather than an internal detail changes three things.

**It makes section 20.1 syntactically visible.** "Names are not authority, handles are" is otherwise a property the reader must reason about. When an operation cannot be written without naming the authority it uses, a function's reach is legible from its signature.

**It removes the ambient root.** A conventional `/` is ambient authority, available to every process without anyone granting it, which is exactly what the handle model eliminates elsewhere. Letting the C library reintroduce one would undo that at the top level. It also strengthens `NS_NEW` from a convention into a fact: a process given no directory handles cannot name anything at all, rather than being given an empty mount table the library must be trusted to honour.

**It removes time-of-check-to-time-of-use on resolution.** A directory handle is a stable referent. A path resolved in several steps can be invalidated between them by a concurrent change to the namespace.

**This buys no additional security, and should not be claimed to.** Confinement comes from the handle table. A process holding a directory handle to the root of a filesystem is exactly as powerful under handle-relative naming as under absolute paths. The gain is clarity, auditability, and the removal of the resolution race.

**Compatibility with absolute-path interfaces is a library concern.** Code written against a conventional filesystem API calls `open("/etc/foo")`. Handle-relative naming is the native interface; a compatibility layer holds a root handle and synthesizes the ambient root for code that expects one. This is the same division as lowest-first descriptor allocation in section 7.3: the accommodation is real, and it lives in the compatibility layer rather than in the native interface.

**Dot-dot is resolved in two places, at two layers, for two different reasons.** Neither rule alone is sufficient, and assigning the whole job to either one breaks something.

**The library resolves `..` lexically, before selecting a namespace entry.** A path is reduced to canonical form first, and only then matched against the process's array of named directory handles.

This ordering is forced. Consider a process holding `/` and `/dev` as separate handles served by different processes, opening `/dev/../etc/passwd`. If the entry were selected first, the library would match `/dev` and send `../etc/passwd` to the device server, which has no knowledge that a namespace boundary exists above it and can only refuse. The path is legitimate and names something the process is entitled to reach, but no server is in a position to say so. **A server cannot resolve `..` across a namespace boundary, because the boundary is not visible to it.** Reducing lexically first sends `etc/passwd` to the handle bound at `/`, which is correct.

**Confinement does not depend on the library being honest.** This is what makes the lexical rule safe to place in userspace. A canonical path that lands outside every entry in the process's namespace array matches no handle, and there is nothing to send a message to. A process that skips the library and constructs messages itself gains nothing, because it still holds only the handles it was given, and lexical reduction cannot manufacture one.

**A server must still refuse to resolve above the root of the subtree its channel represents.** This is the backstop for a client that sends `../..` directly on a handle rather than going through the library. It is a rule about a channel's own scope, so a server needs no knowledge of any client's namespace to enforce it, and it does not conflict with the library rule above.

An implementation should also note that lexical and physical resolution diverge where a name is bound to a directory elsewhere in the tree. The library rule is lexical by definition. A server implementing physical resolution must apply its own root check to the physical walk.

### 20.4 The Namespace Is Handed Over at Launch

A process receives its namespace as an array of named directory handles at spawn, and receives nothing else. The mechanism is deliberately mundane and parallels the way environment variables are delivered: the kernel installs the handles, the loader places a startup record in the child's memory alongside `argv` and `envp`, and the C library consumes the namespace array into a mount table before `main()` runs.

```{=typst}
#figure(include "fig/namespace.typ", caption: [A process receives its namespace at launch and nothing else.])
```

The mount table stops being a name resolution service and becomes what it actually is: the set of directory handles this process was given, with names attached for convenience. Two consequences follow.

There is nothing to invalidate. A cache of resolved names would need an epoch or an invalidation broadcast; holding the live handle needs neither. `SYS_ERR_STALE` on generation mismatch is the only staleness mechanism, and it is uniform with every other handle in the system.

And composition becomes trivial. `NS_SHARE`, `NS_CLONE` and `NS_NEW` are choices about what array to pass: the parent's entries, a filtered subset, or nothing. Union directories, where one prefix resolves against several handles in order, are a library feature requiring no kernel involvement.

---

## 21. Composition of the Priority Mechanisms

Four mechanisms adjust priority: IPC donation to a server, fault donation to a pager, timeline donation to a signal-right holder, and priority-inheriting futexes. They must compose, because one thread can be inside several at once.

The blocking invariant makes this tractable. Because a thread blocks on exactly one object at a time, the blocking relation forms a **chain, not a graph**, and donation walks it in one direction with no branching:

```{=typst}
#figure(include "fig/priochain.typ", caption: [The blocking relation is a chain, so priority donation walks it in one direction.])
```

Three rules govern it.

1. **Chain length is bounded** by `SYS_PRIO_CHAIN_MAX`. An operation that would exceed the bound fails with `SYS_ERR_DEADLOCK`. This is what gives `SYS_ERR_DEADLOCK` an operational definition rather than leaving it as a code an implementation must guess when to return.
2. **Cycles are deadlock.** Because the relation is a chain, a cycle is found by walking it, and the walk is bounded by rule 1. The pager chain check in section 15.4 is the same walk applied at pager-set time.
3. **Cross-core donation costs an IPI** and one scheduling decision, per section 11.4.

Kernel-signalled timelines terminate a chain without donating, per section 9.2.

### 21.1 Priority-Inheriting Futexes

A futex offering only `WAIT` and `WAKE` cannot implement priority inheritance, because those operations never tell the kernel who owns the lock, and the owner is the single fact inheritance requires. Priority-inheriting operations are therefore distinct primitives rather than a policy applied to the basic ones.

`SYS_FUTEX_LOCK_PI` and `SYS_FUTEX_UNLOCK_PI` use the owner-in-word protocol: the low bits of the futex word identify the owning thread and a waiters bit is set under contention. The kernel reads the owner from the word and donates to it.

Because the futex word is 32 bits and cannot widen, it carries `SYS_ID_TAG31` of the owner: the index plus as many generation bits as fit below `SYS_FUTEX_WAITERS` in the top bit, which is eleven on a 64-bit build and twenty-one on a 32-bit one. This is the second beneficiary of the identifier layout in section 7.1, and arguably the stronger case of the two. A word carrying only an index cannot distinguish a lock owner from a later thread that inherited its slot, so the kernel would have to treat the word as a hint and validate against its own table on every operation. With generation bits present the word identifies an incarnation on its own, and one ABA argument covers pulses and futexes rather than each needing its own.

The futex uses a 31-bit tag where the pulse uses 32 because it must reserve the top bit for the waiters flag. The word stays 32 bits rather than widening to hold a full identifier because Tier 0 targets have no 64-bit atomic instruction.

Owner death is handled by the same force-signal principle used for timelines. A PI futex whose owner dies is marked inconsistent and the next acquirer receives `SYS_ERR_OWNER_DEAD` rather than blocking forever. This is required for the fault recovery in section 18.2 to recover anything.

---

## 22. Introspection

The kernel and system servers expose state as **text key-value attributes**, declared with macros rather than written by hand and read with `sys_stat_read()` against a handle.

The system itself never parses these strings. Consumers are development tools: a remote debugger, or an on-target interactive shell. The kernel produces text, and nothing in the kernel or in any driver ever reads it back.

### 22.1 Why Text

Text is scriptable, self-describing, and needs no versioned binary schema shared between the kernel and every tool that inspects it. A binary interface would couple every tool to the kernel's internal layout and require lockstep updates. The usual objection to text, that parsing is slow, does not apply because nothing on a real-time path ever parses.

**Attribute reads are explicitly not real-time paths.** Formatting happens lazily, only when a debugger reads, so an unobserved system pays nothing. This must remain true: no attribute may be formatted eagerly, and no attribute's accessor may take a lock that a real-time thread can hold.

### 22.2 What Is Exposed

At minimum, enough to build a tracer, a deadlock inspector, and a pool-sizing tool, since all three are needed to size the pools and to see what the system is doing:

* **Threads.** State, current and inherited priority, core affinity, and what the thread is blocked on, named by handle.
* **Channels.** Receive queue depth, pulse queue depth, and overflow flag.
* **Timelines.** Current value, error state, and waiter count.
* **Pools.** Current and high-water utilization for every object type, which is the measurement that makes static pool sizing an engineering exercise rather than a guess.
* **Cohorts.** Membership, restart count, and last fault cause.

Blocked-on information is what turns the priority chain in section 21 from a design claim into something observable. A deadlock inspector walks it directly.

### 22.3 There Is No Discovery Primitive

`sys_stat_read()` names an object by handle, and **there is no call that enumerates objects a process does not already hold a handle to.** This is a consequence of the capability model rather than an oversight: a process reaches only what its parent placed in its table, and a debugger is a process.

Inspection therefore **follows the supervision tree.** A supervisor already holds handles to everything it spawned, so it is already in a position to inspect them, and a debugger obtains information by asking a supervisor rather than by searching. The root process can reach the whole system because it spawned the whole system, and each supervisor below it can reach its own subtree and no further.

This has a real cost and it is stated rather than argued away. **A debugger can only inspect what something supervises**, so a process nobody supervises is opaque, and every supervisor that wants to be debuggable must implement a proxy interface. There is no attaching to an arbitrary running process that was not arranged to be observable.

What it buys is that debuggability introduces no authority the system did not already have. An enumeration primitive would necessarily be a master key, since being able to list every object is being able to reach every object, and that would be the single largest concentration of authority in a design that has otherwise avoided them.

A **compile-time debug capability**, granting enumeration to a designated agent in builds that opt into it, is a plausible future addition for development images, and section 22.4 is the first one of these: it exists only in some builds, its absence in production builds is the default, and its presence is announced at boot. Enumeration itself is still not specified, because that decision is better made against a real debugging workflow than in advance and the workflow that forced the console has not yet needed it.

### 22.4 The Debug Console

**A build may include a system call that writes to the kernel console, taking no handle and checking nothing about its caller.** It is enabled by `SYS_DEBUG_CONSOLE` and absent otherwise, and where it is absent `SYS_OP_DEBUG_WRITE` returns `SYS_ERR_NOT_SUPPORTED` like any other operation the build does not provide.

**It is ambient authority, and that is the whole of the objection to it.** Section 20.1 is the argument that names are not authority and handles are; section 22.3 declines an enumeration primitive on the grounds that being able to list every object is being able to reach every object. This is a smaller version of exactly what those refuse: a capability every process holds because it exists rather than because anything granted it. The right shape is a console service reached through a handle placed in the startup namespace, and that is what replaces this.

It is here anyway because the alternative is worse. A program with no way to say anything can only be debugged by inference from what the kernel observes about it, and a system whose first user programs cannot print is a system whose first user programs are debugged by guesswork. The cost of that is paid immediately and by everyone; the cost of the backdoor is paid by a build option.

**Three rules keep it from becoming an interface.**

**Absence is the default and presence is announced.** A production configuration compiles it out. A build that does not prints a line at boot saying so, because a hole in the capability model that is invisible in the boot log is one that survives into an image nobody meant to ship it in.

**The length is bounded, and the bound is interface rather than configuration.** The copy runs in the kernel at the caller's priority, and on Tier 0 with interrupts disabled, so an unbounded write is an unbounded non-preemptible section and belongs to the interrupt latency of section 13.3 rather than to taste. A caller with more to say makes more calls, and each is a preemption point. `SYS_DEBUG_WRITE_MAX` is therefore visible to callers, who have to split against it.

**It carries no formatting and no file descriptors.** Bytes to the console. The C library's `write()` accepts standard output and standard error and refuses everything else, which is a truthful account of a system with no descriptor table rather than a stub that pretends to have one.

**What it is not.** It is not `sys_stat_read()` and it is not a tracer. Sections 22.1 through 22.3 describe reading attributes from an object a caller holds a handle to, which is inspection that introduces no authority the system did not already have. This introduces some, deliberately and temporarily.

---

## 23. The System Call Interface

section 26 gives the C interface. This section gives the binding underneath it: how a call is made, how arguments and results travel, and what the kernel guarantees about registers and faults. Without it two implementations could agree on everything else in this part and still be unable to run the same binary.

**A port defines the register mapping. This section defines everything else.**

### 23.1 There Is One Entry Point

Every operation enters the kernel through a single trap carrying a pointer to one `sys_op_t` **operation descriptor**.

There is no per-operation trap and no syscall number. The opcode inside the descriptor is what a conventional design would put in a syscall-number register.

The trap carries a pointer and a count. **A single operation is the count-of-one case**, which is what makes the submission engine of section 17 an extension of this interface rather than a second one: the engine passes an array of the same descriptor, and nothing else about this section changes. The gain is that every operation has exactly one implementation, rather than a fast path per call and a general path underneath it that must be kept in agreement.

A reader meeting the system for the first time can stop here. Section 17 is what happens when the count exceeds one, and nothing in sections 5 through 16 depends on it.

A port names the trap instruction and the two registers carrying the pointer and the count. Nothing else is passed in registers.

**Consequence worth stating: the argument area is memory, not registers.** Arguments live in the descriptor and in whatever `arg_ptr` refers to. This costs a memory round trip that a register-passing convention would avoid, and buys three things the design depends on: the argument count is not bounded by the register file, one descriptor format serves every operation, and a batch of many operations costs exactly one trap.

### 23.2 Validation Happens Once, Before Execution

On entry the kernel validates, in this order:

1. `count` does not exceed `SYS_BATCH_MAX_ENTRIES`, or the call returns `SYS_ERR_INVALID_ARG` having executed nothing.
2. The descriptor array is readable and writable by the caller for `count × sizeof(sys_op_t)` bytes, or the call returns `SYS_ERR_FAULT` having executed nothing.
3. Each descriptor's opcode is defined, or that entry fails with `SYS_ERR_INVALID_ARG` when reached.

The array must be writable because the kernel writes each entry's status field into it, per section 17.6.

**Validation of the array is not repeated during execution.** This is what makes the status write free of per-entry checking, and it is why the array must not be shared with another thread that could alter it mid-batch. An implementation may not assume the caller respects that; it must be safe against concurrent modification in the sense that the kernel never faults or misbehaves, but the resulting statuses are then unspecified.

### 23.3 Results

The trap returns a single `sys_err_t`:

* `SYS_OK` if every entry completed successfully.
* The status of the **first entry that failed**, if any did.
* `SYS_ERR_INTERRUPTED` if the thread was cancelled mid-batch, per section 10.5.

Per-entry results are always in the entries themselves, and the return value is a summary for the common single-entry case. A caller submitting more than one entry reads statuses from the array rather than inferring them from the return.

### 23.4 Register and Memory Guarantees

A port must document its register mapping, and every port obeys the following regardless of it.

**Callee-saved registers are preserved across the trap.** A caller need not save them.

**Caller-saved registers are unspecified on return**, except the one carrying the return value. A caller must assume they are destroyed.

**No kernel state is exposed in any register on return.** This is a security requirement, not a convention: registers not carrying a result are cleared rather than left holding whatever the kernel last put there.

**The stack pointer is unchanged**, and the kernel does not use the caller's stack. A port supplies its own kernel stack per thread.

### 23.5 Faults During a System Call

A user address supplied to the kernel may be unmapped. On Tier 2 that is an ordinary page fault, and the resolution differs from a fault in user code in one respect worth stating.

**A fault on a user address touched by the kernel is resolved through the faulting thread's pager**, exactly as in section 15.3, because the thread is the one that supplied the address and is the one blocked. The kernel does not acquire its own pager relationship and does not fault on the caller's behalf at a different priority.

**If that fault cannot be resolved, the operation fails with `SYS_ERR_FAULT`** rather than terminating the caller. A bad pointer is an error return, not a signal, because there is nothing in this document to deliver a signal with.

On Tier 0 there is no fault, and an invalid address is not caught at all, per section 6.1.

### 23.6 What a Port May Not Change

For completeness, since the list is short and the temptation to optimise here is real:

* The descriptor format `sys_op_t`, including field order and width.
* The single-entry-point structure, and the absence of syscall numbers.
* The validation order in section 23.2, since programs depend on nothing having executed when `count` is rejected.
* The return value rule in section 23.3.
* The register-clearing guarantee in section 23.4.

A port may choose the trap instruction, the two argument registers, the return register, the kernel stack size, and the alignment it requires of the descriptor array.

---

## 24. The Root Process

**This section defines nothing new.** Like section 19, it assembles mechanisms described elsewhere into the shape of a program. An implementation may ignore it entirely, or replace the root process described here with a different one, which is exactly what section 2.8 means by replaceable parts.

It is written out because the first process is where the authority model, the supervision model and the boot sequence meet, and a reader who has understood each of them separately can still be left without a picture of how a system starts.

### 24.1 The Boot Contract

**The kernel starts one process and understands one address.** It parses no image format, reads no file, and implements no protocol. Section 8 hands the root process a `sys_bootinfo_t` describing the physical memory map, the pool configuration, and the initial handles; the entry point is supplied by the build or by the port's bring-up stage (section 27).

Two of those handles are worth naming, because the rest of the boot sequence is built on them.

**The boot archive region.** A read-only memory region containing the images of every program started at boot, including the ones root spawns before any filesystem exists.

**The platform configuration region.** A read-only memory region holding whatever the platform hands over to describe itself, a flattened device tree on the targets that use one. It is `SYS_HANDLE_INVALID` where the platform supplies nothing. The kernel does not read it. Root does not read it either, beyond passing it to whichever child owns device enumeration.

**The kernel never touches a file, and there is no later point at which it needs to.** Program loading is a userspace responsibility (section 15), a page fault is a message the faulting thread sends to a userspace pager (section 15.3), and restart is a supervisor respawning from an image it already holds. A filesystem reader inside the kernel would be used exactly once, at boot, for one file whose address was already known.

This is why the alternative shapes are rejected. A kernel that understood the archive format would carry a parser of build-supplied data in the most privileged component in the system. A kernel that answered queries about the platform configuration would be a discovery service, which contradicts the rule that the kernel grants nothing on request, and it would be the first thing in the system handing out information nobody granted.

### 24.2 Policy Is a Table, Compiled In

**Root's configuration is a `const` array linked into its image.** On Tier 0 it executes in place from flash and costs no RAM at all. There is no configuration file, no parser, and no interpreter.

The reasoning is bootstrap before it is anything else. A parser in root would be reading a file from a filesystem that root itself started, which is a cycle, and it would put untrusted input handling in the one process that holds every capability in the system. Rebuilding the image to change the set of programs that start is an entirely reasonable price for an embedded system, and it is the same price section 2.7 already charges for pool sizes.

**The table is a list of policies, not a list of programs.** The program is one field. What varies between children, and what a Unix `init` has no field for, is everything else:

| Field | Meaning |
| -------------------- | ------------------------------------------------------------------- |
| Image | Name of the member in the boot archive. |
| Spawn attributes | Cohort, namespace array, inherited handles: `sys_spawn_attr_t`. |
| Restart group | Which siblings restart with it. Empty means it restarts alone. |
| Restart intensity | N restarts within T before escalating. |
| Escalation action | Which rung of section 24.3's ladder its failure reaches. |
| Essential | Whether the product is still doing its job without it. |

**Essential is the field that separates a health monitor from a respawner**, and it is where the multiuser lineage has nothing to offer. On a timesharing system every daemon is roughly optional, which is why `init` has no such flag. On a device some children are load-bearing, and the correct response to losing one is a controlled reset rather than a patient retry. The precedent is ARINC 653 health monitoring, cited in section 3: a build-time table mapping error level to a fixed recovery action, with no runtime policy engine anywhere.

**The restart group is not the cohort.** The cohort is the *protection* recovery unit and the kernel enforces it (section 18.2). The restart group is policy and may be larger: a filesystem holding handles to a block cohort that restarts usually has to restart too, and nothing in the kernel knows that. Expressing it as an edge in the table keeps the knowledge where the configuration is.

**The table is checkable at build time**, which is the payoff for it being static. An essential child whose restart group contains a non-essential one, or that depends on a non-essential child, is a configuration error. So is a cycle in the restart groups. So is a namespace entry naming an archive member that does not exist. A build that rejects these turns the whole recovery policy of the system into one reviewable artifact, which is also what an auditor will ask for.

### 24.3 Supervision Is a Tree, and the Shape Is Forced

**Root supervises a few children, each of which may supervise its own.** This is not a stylistic preference. A thread blocks on one object at a time (section 5) and there is no multi-wait primitive (see `PROPOSALS.md`), so watching N completion timelines costs N threads. Flat supervision from root would put N stacks inside the process holding every capability in the system, which on Tier 0 is unaffordable outright and everywhere else is the wrong place to spend them.

It also converges with introspection. Section 22 already establishes that inspection follows the supervision tree, so a hierarchy here means the debugging story and the recovery story describe the same structure rather than two.

**Restart is bounded and escalates.** V10 `init` respawns immediately and forever, and every later Unix retrofitted some version of "respawning too fast" onto it, which is evidence about the default rather than about those implementations. The formulation that has held up is Erlang/OTP's: N restarts within T, and on exceeding it the supervisor fails to *its* parent rather than continuing to flap.

The ladder, from a child's failure upward:

1. Restart the child, or its cohort if it has one.
2. Restart the restart group.
3. Restart the subtree, which means the supervisor itself failing upward.
4. Enter a degraded mode.
5. Reset the board.

**Escalation terminates, and that is the embedded difference.** A multiuser system has no rung 5 and so has no reason to build the ladder. Here root is the last supervisor, and the natural implementation of rung 5 is that root stops petting the hardware watchdog. Root pets it only while the essential set is healthy, so the failure of the escalation path and the failure of root itself produce the same recovery.


**Below the ladder there is a kernel rule, and it is not policy.** Root, and
any process the configuration marks essential, must not terminate. If one
does, the kernel panics and stops the machine rather than continuing with a
system that can no longer recover anything. This is not the supervisor
escalating, because there is no supervisor left to escalate to; it is the
kernel refusing to run a system whose recovery path has already failed.

It exists because rung 5 assumes a watchdog, and a board without one would
otherwise sit there with a dead root and no way to notice. Stopping is also
the more useful failure for a system under development, where a machine that
exits is observable and a machine that hangs has to be watched.

**Root has no supervisor.** Nothing above it can restart it, and the watchdog is the entire recovery story for a bug inside it. That is the argument for keeping its policy declarative: the code is one loop over a table, small enough to read line by line, and the part that changes per product is data that a build-time checker inspects.

### 24.4 The Boot Archive

**A header table of fixed-size records, followed by payloads aligned to the port's page or region granularity, and no compression.**

Alignment is the detail worth getting right the first time, because section 2.9 makes it expensive to move later. Text executes in place on Tier 0 (section 15.2) and is mapped on Tier 2, and both want the member payload naturally placed. The `cpio` format that the initramfs lineage inherited aligns to two or four bytes, which defeats both. Compression is left out of the base format because it implies a decompressor and a RAM budget that the compact build does not have; a hosted build that wants it decompresses into a RAM filesystem at boot and the format above stays uniform.

**Root walks the archive, and so does the romfs server.** The walk is a loop over fixed-size records, on the order of forty lines, and it is not a filesystem implementation: it speaks no protocol and resolves no paths. Duplicating it in two programs is the correct trade, because the alternative is a server in the boot path before root is able to spawn anything.

**Spawning from the archive is nearly free below Tier 2.** Text and rodata are already at their final addresses in flash, so root points the new process at the member and copies only writable data. The full loader service of section 15.2 is an ordinary child that root spawns for runtime RAM loading, and the bootstrap does not need it.

### 24.5 Startup Order and Readiness

**Order is computed at build time.** The system is statically configured, so there is nothing to resolve at runtime. This is where the sysvinit lineage is worth learning from rather than copying: numbered runlevel sequences were replaced everywhere they were tried, because sequential numbering encodes dependencies badly. The answer is not a better runtime resolver, it is doing the resolution in the build.

**Most children need no readiness protocol at all.** Root creates the channel and passes both ends, so a client that starts first blocks in `sys_msg_send()` until the server receives. This is the property socket activation was invented to provide in systems that could not arrange it directly, and here it falls out of synchronous IPC: the client waits rather than proceeding on a buffered request, which is the better behaviour for a system that wants to state its timing. Priority donation (section 12.3) covers the case where the waiting client outranks the starting server.

**The exception is romfs, and it is instructive.** Root needs directory handles to place in its children's namespace arrays (section 20.4), and those handles come *from* romfs. Root is waiting for data, not for a signal, so romfs's first act is sending its root directory handle back on the channel root created, and readiness is the handle arriving. Root then performs exactly one client-side operation of section 25, a walk to derive per-child subtree handles, and never opens a file.

The resulting sequence:

```
kernel  ->  root  ->  [Tier 2: pager]  ->  romfs  ->  everything else
```

**The pager edge is the only genuine ordering constraint, and only on Tier 2.** Root holds the root pager channel, and demand-paged child text (section 15.3) cannot fault until something is behind it. Tier 0 has no such edge because it has no faults to service.

### 24.6 What Root Must Not Become

Every item here is something that a first process accumulates naturally and that this one should not.

**Not a filesystem server.** It walks an archive and serves nothing.

**Not a name server.** Namespaces are arrays of handles handed over at spawn (section 20.4). There is no process to ask.

**Not a pager.** It holds the root pager channel at handoff and hands it on.

**Not configurable from outside.** A hosted build will want a command line, and the platform configuration will contain one. Let it select among modes root already contains, and never let it name a child, a path, or a handle. A bootloader that can name a child is a bootloader that can inject authority, and the reviewability that section 24.2 buys is gone.

---

## 25. The Filesystem Protocol

This section matters more than its length suggests. It gives the messages a filesystem server understands, and it is the one interface the rest of the design leans on hardest, for the reason section 2.9 gives: substitutability rests on a fixed interface, and anything replacing a filesystem is replacing it against this.

### 25.1 It Is a Protocol, Not a Layer

**There is no VFS server, and adding one would be a mistake.** Section 20 already places the mount table in the C library, so the naming half of what other systems call a VFS is specified and requires no process. What remains is the wire between a client and a filesystem, which is what this section defines.

A VFS process would sit in the path of every operation without being able to enforce anything, because section 20.1 is exactly the argument that there is no authority in a name to protect. Plan 9 is the precedent: in a message-passing system the VFS *is* the protocol, and 9P is what it looks like when the layer is dissolved into it.

**Identifiers in this section are prefixed `fs_` rather than `sys_`.** This is not a kernel interface. It is carried by ordinary messages over ordinary channels, and no part of it is known to the kernel.

### 25.2 A File Handle Is a Connection

**A directory or file is named by a connection handle, minted by the server with `sys_chan_connect()` on its own channel and moved to the client, with the server holding the per-connection state that binds it to a node.** Section 12.1 is why only the server can mint one.

This follows the channel and connection split of section 12.1 and matches how QNX resource managers work, where a descriptor is a connection. It is the choice that makes handle-relative naming (section 20.3) real rather than conventional: the authority is a kernel object, so a client cannot forge a reference to a node it was never given, and passing a directory to a child is an ordinary handle move.

Three consequences follow.

**Rights attenuate on duplication, per section 7.** A read-only directory handle is a duplicate with the write right removed, and the server must check rights on the connection rather than trusting the request.

**Open files draw from the static object pools of section 8.** A build must size the connection pool for the maximum number of simultaneously open files across the whole system, and exhaustion is a configuration error rather than a runtime condition to recover from. This is the ordinary cost of section 2.7 applied to a place where general-purpose systems assume growth, and a design that opens files in response to load must state its bound.

**Server-side state is reclaimed on `FS_OP_CLOSE`.** A client closing its handle without sending it leaves the server holding a record it cannot reclaim, since the kernel provides no notification when a connection object is destroyed. See `PROPOSALS.md`.

### 25.3 Operations

| Operation | Effect |
| ------------------ | ---------------------------------------------------------------- |
| `FS_OP_WALK` | Resolve a relative path against this connection, returning a new connection to the named node. |
| `FS_OP_STAT` | Attributes of the node this connection names. |
| `FS_OP_READ` | Read `length` bytes at `offset`. |
| `FS_OP_WRITE` | Write at `offset`, or at the end if `FS_WRITE_APPEND` is set. |
| `FS_OP_CREATE` | Create a node under this directory and return a connection to it. |
| `FS_OP_UNLINK` | Remove a name from this directory. |
| `FS_OP_RENAME` | Rename within this directory. |
| `FS_OP_READDIR` | Read directory entries from an opaque cursor. |
| `FS_OP_TRUNCATE` | Set the length of the node this connection names. |
| `FS_OP_SYNC` | Commit outstanding writes to the underlying device. |
| `FS_OP_CLOSE` | Release the server's state for this connection. |
| `FS_OP_BATCH` | Execute a sequence of operations, stopping at the first failure. |

**A server must implement every operation**, returning `SYS_ERR_NOT_SUPPORTED` where the backing store cannot provide one. A read-only server implements `FS_OP_WRITE` as a rejection rather than as silence, so that a client discovers the limit at the point of use rather than by inference.

**`FS_OP_WALK` must refuse to resolve above the root of the subtree its connection represents.** This is the server-side backstop of section 20.3 and it is required, not advisory: it is what confines a client that constructs messages directly instead of going through the library's lexical reduction. A server needs no knowledge of any client's namespace to enforce it, because the rule is about the connection's own scope.

**Path arguments are relative and bounded.** There are no absolute paths in the protocol, and a path exceeding the server's configured maximum is rejected with `SYS_ERR_INVALID_ARG` rather than truncated.

The encoding is a request header in `inline_data`, followed by the path bytes where the operation takes one. Bulk data travels in the grant, never inline.

```c
typedef enum fs_op {
    FS_OP_WALK     = 1,
    FS_OP_STAT     = 2,
    FS_OP_READ     = 3,
    FS_OP_WRITE    = 4,
    FS_OP_CREATE   = 5,
    FS_OP_UNLINK   = 6,
    FS_OP_RENAME   = 7,
    FS_OP_READDIR  = 8,
    FS_OP_TRUNCATE = 9,
    FS_OP_SYNC     = 10,
    FS_OP_CLOSE    = 11,
    FS_OP_BATCH    = 12,
} fs_op_t;

#define FS_WRITE_APPEND  (1u << 0)   /* server computes the offset */
#define FS_WALK_DIR_ONLY (1u << 1)   /* fail unless the result is a directory */

typedef enum fs_kind {
    FS_KIND_FILE   = 1,
    FS_KIND_DIR    = 2,
    FS_KIND_DEVICE = 3,
} fs_kind_t;

/* Carried in inline_data. Path bytes follow the header and are not
 * NUL-terminated. RENAME carries both names, old then new, with path_len
 * covering the first and name_len the second. */
typedef struct fs_req {
    uint32_t op;
    uint32_t flags;
    uint64_t offset;      /* READ, WRITE, TRUNCATE; cursor for READDIR */
    uint32_t length;      /* bytes requested; grant capacity bounds it */
    uint16_t path_len;
    uint16_t name_len;
} fs_req_t;

/* Carried in the reply's inline_data. A connection returned by WALK or
 * CREATE arrives as a moved handle, not in this structure. */
typedef struct fs_rep {
    sys_err_t err;
    uint32_t  flags;
    uint64_t  count;      /* bytes transferred, entries written, or new size */
    uint64_t  cursor;     /* READDIR: pass as offset in the next request */
} fs_rep_t;

/* STAT reply payload, following fs_rep_t inline. */
typedef struct fs_stat {
    uint64_t    size;
    sys_ticks_t mtime;
    uint32_t    kind;     /* fs_kind_t */
    uint32_t    rights;   /* what this connection may do, not the node's mode */
} fs_stat_t;

/* READDIR writes a packed sequence of these into the grant. Each is followed
 * by name_len bytes and padded to eight. */
typedef struct fs_dirent {
    uint32_t kind;        /* fs_kind_t */
    uint32_t name_len;
} fs_dirent_t;
```

**`rights` in `fs_stat_t` reports the connection, not the node.** A mode field describing what some other principal could do with the node would be reporting authority the caller does not hold, and section 20.1 is the argument that names and modes are not where authority lives. A caller learns what it may do, which is the only question it can act on.

### 25.4 Offsets Travel in the Message

**A connection carries no file position.** Every read and write states its own offset, and the C library owns whatever position a POSIX-shaped API needs to present.

This removes shared mutable state from the connection, so several threads may use one handle without coordinating, and it makes a retry after a failed request mean the same thing as the original. A server-side position would additionally have to be recovered after a restart, which is state a restarting filesystem should not be trying to reconstruct.

**`FS_WRITE_APPEND` is the single exception and is deliberate.** Logging is a stated use of this protocol, and an append whose offset was computed by the client is a lost record whenever two writers share a file. The flag makes the server compute the offset, which is the only operation in the protocol whose result depends on server state the client cannot see.

### 25.5 Payload

**Small payloads travel inline and bulk payloads travel in the message buffer**, per section 12.4. A client passes a buffer for the destination of a read or the source of a write, and by section 6.1 must not touch that memory until the reply arrives. A reply larger than the inline payload writes into that buffer, because the reply-path caveat of section 6.1 forbids a server handing back memory of its own.

On Tier 2 a server forwarding a payload it does not inspect never maps it, per the lazy mapping of section 18.4. This is what keeps a filesystem layered over a block driver from paying a mapping cost per layer.

**Requests execute at the client's priority.** This is not a property of the protocol but of the transport, and it is stated here because it is the reason the protocol is synchronous. A filesystem server sizes its receiving thread count for the concurrency it wants; the threads are ordinary and the donation of section 12.3 applies to each independently.

### 25.6 Failure, Staleness, and Removable Media

**The protocol defines no error namespace of its own.** Replies carry `sys_err_t`, and a server must not invent codes.

**Device removal is server death, and the existing mechanisms cover it.** A filesystem whose device disappears exits. Clients blocked in a request are released with `SYS_ERR_OWNER_DEAD` (section 12.5), cached connection handles fail with `SYS_ERR_STALE` on next use, and the library re-resolves or reports (section 20.2). No timeout is involved anywhere, and a client learns promptly rather than after a wait it chose in advance.

**A filesystem server that parses media it does not control must not share a cohort with a server for fixed storage.** A cohort fault destroys the cohort (section 18.2), so placing a parser of attacker-supplied media beside the internal storage stack means a malformed volume takes down internal storage. This is a rule about configuration rather than about implementation, and it is one the build-time check of section 24.2 can enforce.

Data with a durability requirement across power loss is not served by putting it on such a volume, and this document does not attempt to make it so. Calibration and configuration records want a small A/B or append-only store with an explicit commit, which is a different problem from a filesystem and is out of scope here.

### 25.7 Compound Requests

Opening a file and reading it is the operation this protocol exists for, and in the form above it costs three round trips: walk, read, close. `FS_OP_BATCH` is those three in one message, and it is the concrete case of section 12.7.

**What it saves is not mainly context switches.** Six become two, which is worth having. The larger saving is that a whole-file read stops allocating anything durable. Section 25.2 requires a build to size the connection pool for the maximum number of simultaneously open files across the whole system, and calls exhaustion a configuration error rather than a runtime condition. A client reading a small file end to end has no use for a durable reference to it, and in the three-message form it holds one anyway, across two further round trips. In the compound form the intermediate node lives only inside the server's execution of one request, and the pool is never touched.

**Path resolution was already one round trip.** `FS_OP_WALK` takes a whole relative path, and section 20.3 puts lexical `..` reduction in the library before any message is sent. Compounding does not remove a resolution round trip, because there was not one to remove. What it removes is the open and the close.

#### Shape

**The sequence travels inline, and only bulk data uses the message buffer.** A header carrying `FS_OP_BATCH` and a count, then fixed-size entries, each followed by its path bytes padded to eight. The reply is a header and nothing else, because the data went where the client asked for it.

Putting the sequence inline rather than in a buffer is the same argument section 17.2 makes for a batch: it is small, bounded, and fixed in advance, so it can be checked once before any of it runs. A sequence in a buffer would additionally have to be checked against a length the client controls, on memory the client can change, which is the problem the inline payload exists to avoid.

```c
#define FS_BATCH_MAX     4

typedef struct fs_batch {
    uint32_t fb_op;        /* FS_OP_BATCH */
    uint32_t fb_count;     /* entries following this header */
} fs_batch_t;

/* Followed by fe_namelen path bytes, padded to eight. */
typedef struct fs_entry {
    uint8_t  fe_op;
    uint8_t  fe_flags;
    uint16_t fe_namelen;
    uint32_t fe_length;    /* READ: bytes wanted */
    uint64_t fe_offset;    /* READ: where to start */
} fs_entry_t;

typedef struct fs_batch_reply {
    sys_err_t err;         /* the first failure, or SYS_OK */
    uint32_t  failed;      /* entry that failed, when err is not SYS_OK */
    uint32_t  completed;   /* entries that ran to completion */
    uint32_t  length;      /* bytes the read produced */
} fs_batch_reply_t;
```

#### Rules

**Chaining is implicit and linear.** Each entry operates on the node the entry before it produced, and the first operates on the connection the message arrived on. There is no reference field.

An earlier draft gave each entry the index of the entry whose node it used, and that was wrong twice over. It put the index in `offset`, which a read needs for the file position, so a compound read at a non-zero offset could not be written at all. And it bought a generality nothing wanted: the sequence this exists for is a straight line, and section 17.5 makes the same argument about `LINK`, that chains are not graphs. An entry that needs to name something other than what came before it is a different request.

**A compound reaches exactly what the same operations sent one at a time would reach.** The starting authority is the same connection handle and each walk applies the same root check as section 25.3. Nothing about confinement changes, which is what makes this an optimisation rather than a new interface.

**Execution stops at the first failure and the reply names it.** Entries after the failing one do not run, `failed` says which one, and `completed` says how many ran. A client that could not open the file learns that rather than receiving three separate errors it has to interpret.

**Intermediate nodes are named by position and are never handles.** An entry that produced a node holds it for the duration of the request only. This is what removes the pool allocation, and it is also what makes abort trivial: the state a failed compound abandons belongs to the server, which is the party doing the abandoning, so there is nothing to unwind across a boundary.

**There is no close entry, because there is nothing left to close.** The node is released when the reply is sent, whether or not anything asked. Including one would cost sixteen bytes of a sixty-four byte payload to do nothing. The honest comparison is therefore two operations in one message against three messages, not three against three.

**A short read is not a failure.** Reading past the end of a file yields a count smaller than requested and `SYS_OK`. Treating it as an error would make every whole-file read of a file whose length the client does not know abort its own sequence, which is precisely the case the compound exists to serve.

**There is one data-bearing entry and the reply carries one count.** Per-entry statuses are redundant under abort-at-first-failure: every entry below `failed` succeeded and every entry above it did not run. What is not redundant is a length, because a short read succeeds, so the read reports its own.

**The compound is bounded, and the bound is a configuration value like every other in section 2.7.** A server rejects a sequence with more entries than it is built for, or one that does not fit the payload it arrived in, with `SYS_ERR_INVALID_ARG` before executing any of it.

**A durable connection is not available from a compound.** A client that wants to keep a file open uses `FS_OP_WALK` and gets a connection, which is what that operation is for. Returning one from a compound would put back exactly the allocation the compound exists to avoid, for a client that has said by its choice of interface that it does not want one.

#### What it measures

The claim is that one message costs a third of the round trips and none of the connection pool. Both are countable rather than argued, and both hold:

| | rendezvous | connections |
| ----------- | ---------- | ----------- |
| walk, read, close | 3 | 1 |
| compound | 1 | 0 |

The second column is the one worth having. Section 25.2 requires a build to size the connection pool for every file open at once across the whole system, and a client reading a small file end to end has no use for a durable reference to it. Context switches are the visible saving and the pool is the structural one.

#### What it costs

**A compound cannot adapt.** It cannot read the size out of the walk's answer and size the read from it, because that is a branch and the sequence has none, for the same reason section 17.2 has none. A client reads a fixed length and accepts a short count, or sends a second request. This is the ordinary shape of a whole-file read, where the client has already chosen a buffer, and it is a poor fit for anything whose next step depends on the last one's result.

**A compound runs the server for longer at one client's priority.** This is correct rather than a problem, since the work is the same work, but a server sizing its receiving threads should count a compound as one request of several operations' duration rather than as one operation.

**Two ways to write the same thing is a cost in itself.** A server must implement both forms, and a client library must decide which to use. The rule that keeps this small is that the compound is not a general facility: it exists for sequences fixed in advance, and a client doing anything conditional uses individual requests.

### 25.8 What Is Not in the Base Protocol

Each of these is absent for a reason, and the reason is the same in every case: it buys a property stated elsewhere.

**Cross-directory rename.** Naming a second directory would require moving a duplicated handle into the request, which consumes it, and the atomic replace pattern that rename exists for is within one directory. `FS_OP_RENAME` is therefore scoped to one connection.

**Locking.** There is no advisory lock operation. Exclusion is the property of a server that owns the resource, and a protocol-level lock would be a way for a client to hold state in a server across requests, which section 25.4 has otherwise removed.

**Mapping and writeback.** Files are read and written, not mapped. Section 15.1 has no writeback paging; `PROPOSALS.md` describes what adding it would cost.

**Change notification.** There is no watch operation. A client that needs to know when something changed asks the process that changes it.

---

## 26. C Header Specification

```c
/**
 * @file os/sys.h
 * @brief Nyctos kernel API specification. Revision 4.
 *
 * The sys_ and SYS_ prefixes are deliberate rather than provisional. A
 * kernel is not a library: it does not link into someone else's program and
 * has no third-party namespace to stay clear of. Nyctos owns its entire C
 * namespace, including its libc, so the prefix can say what the operations
 * are rather than which project shipped them.
 */

#ifndef OS_SYS_H
#define OS_SYS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* Types and Identifiers                                                      */
/* ========================================================================== */

typedef int32_t   sys_err_t;
typedef uint64_t  sys_ticks_t;

/* Absolute deadline on the system timebase. Relative timeouts drift across
 * interruption and restart, which is exactly the error real-time code exists
 * to avoid. Relative helpers live in libc. See section 14.3. */
typedef uint64_t  sys_deadline_t;

#define SYS_DEADLINE_NOW      ((sys_deadline_t)0)          /* poll, never block */
#define SYS_DEADLINE_NEVER    ((sys_deadline_t)UINT64_MAX) /* block forever */

/* An identifier is one machine word: a table index in the low bits and a
 * generation above it, making use-after-free detectable rather than silently
 * operating on a recycled slot.
 *
 * The split is sized to the pools and therefore differs by word size:
 * 20 and 44 on a 64-bit build, 10 and 22 on a 32-bit one. Twenty bits of
 * index on a part holding a few dozen objects would describe a range no
 * configuration of it could reach, and those bits are worth more as
 * generation. See section 7.1.
 *
 * On a 64-bit build the split also exists so the low 32 bits carry the
 * index plus 12 bits of generation. Two places in this document must fit
 * an identity into 32 bits: the pulse value field and the PI futex word.
 * Both carry generation bits alongside the index, so neither is vulnerable
 * to a slot being recycled between when a value is written and when it is
 * read. On a 32-bit build an identifier is already 32 bits and the pulse
 * value carries all of it. See sections 10.5 and 21.1.
 *
 * A handle is opaque above the kernel. SYS_ID_INDEX and SYS_ID_GENERATION
 * are here to document the layout, not to invite a program to read it. */
typedef uintptr_t sys_handle_t;
typedef uintptr_t sys_thread_id_t;
/* Identity for introspection and reporting only. Operations take a handle,
 * because a handle carries authority and an identity does not. See section 5. */
typedef uintptr_t sys_proc_id_t;

#if UINTPTR_MAX > 0xFFFFFFFFu
#define SYS_ID_INDEX_BITS     20    /* 1,048,576 slots per pool */
#define SYS_ID_GEN_BITS       44
#else
#define SYS_ID_INDEX_BITS     10    /* 1,024 slots per pool */
#define SYS_ID_GEN_BITS       22
#endif

#define SYS_ID_INDEX_MAX      (((uintptr_t)1 << SYS_ID_INDEX_BITS) - 1u)

#define SYS_HANDLE_INVALID    ((sys_handle_t)0)
#define SYS_THREAD_INVALID    ((sys_thread_id_t)0)

#define SYS_ID_INDEX(id)      ((uint32_t)((id) & SYS_ID_INDEX_MAX))
#define SYS_ID_GENERATION(id) ((uintptr_t)((id) >> SYS_ID_INDEX_BITS))

/* Index plus generation, truncated to fit where a full identifier cannot.
 * TAG32 (index + 12 generation bits) is carried in the pulse value field.
 * TAG31 (index + 11 generation bits) is carried in the PI futex word, which
 * must reserve its top bit for SYS_FUTEX_WAITERS. A false match requires
 * thousands of recycles of one slot inside the window between write and
 * read. See sections 10.5 and 21.1. */
#define SYS_ID_TAG32(id)      ((uint32_t)((id) & 0xFFFFFFFFu))
#define SYS_ID_TAG31(id)      ((uint32_t)((id) & 0x7FFFFFFFu))

/* ========================================================================== */
/* Error Codes                                                                */
/* ========================================================================== */

#define SYS_OK                    0
#define SYS_ERR_TIMEOUT          -1
#define SYS_ERR_WOULDBLOCK       -2
#define SYS_ERR_INVALID_ARG      -3
#define SYS_ERR_NO_MEMORY        -4   /* static pool exhausted; see section 8 */
#define SYS_ERR_PERM_DENIED      -5   /* handle lacks the required right */
#define SYS_ERR_DEADLOCK         -6   /* chain bound exceeded, or cycle */
#define SYS_ERR_STALE            -7   /* generation mismatch */
#define SYS_ERR_NO_ENT           -8
#define SYS_ERR_CANCELED         -9   /* chain aborted by an earlier failure */
#define SYS_ERR_OWNER_DEAD      -10   /* server, signaler or lock owner terminated */
#define SYS_ERR_TRUNCATED       -11   /* caller buffer too small, see out count */
#define SYS_ERR_WRONG_TYPE      -12   /* handle names the wrong object type */
#define SYS_ERR_INTERRUPTED     -13   /* unblocked by sys_thread_cancel() */
#define SYS_ERR_NOT_SUPPORTED   -14   /* not available at this tier */
#define SYS_ERR_FAULT           -15   /* bad user address */

/* ========================================================================== */
/* Handle Rights                                                              */
/* ========================================================================== */

#define SYS_RIGHT_DUPLICATE   (1u << 0)
#define SYS_RIGHT_TRANSFER    (1u << 1)   /* may be moved over IPC */
#define SYS_RIGHT_READ        (1u << 2)
#define SYS_RIGHT_WRITE       (1u << 3)
#define SYS_RIGHT_EXEC        (1u << 4)
#define SYS_RIGHT_MAP         (1u << 5)
#define SYS_RIGHT_SEND        (1u << 6)
#define SYS_RIGHT_RECV        (1u << 7)
#define SYS_RIGHT_WAIT        (1u << 8)   /* timeline: may block on a value */
#define SYS_RIGHT_SIGNAL      (1u << 9)   /* timeline: may advance; donation target */
#define SYS_RIGHT_MEM_LOCK    (1u << 10)  /* may lock memory; paging path only */
#define SYS_RIGHT_IRQ         (1u << 11)  /* may attach interrupts */
#define SYS_RIGHT_STAT        (1u << 12)  /* may read introspection attributes */
#define SYS_RIGHT_ALL         0xFFFFFFFFu

/* ========================================================================== */
/* Object Types and Handle Operations                                         */
/* ========================================================================== */

#define SYS_OBJ_CHANNEL     1
#define SYS_OBJ_GRANT       2
#define SYS_OBJ_TIMELINE    3
#define SYS_OBJ_TIMER       4
#define SYS_OBJ_PROGRAM     5
#define SYS_OBJ_PROCESS     6
#define SYS_OBJ_THREAD      7
#define SYS_OBJ_IRQ         8
#define SYS_OBJ_CONNECTION  9   /* section 25.2; absent from earlier drafts */

sys_err_t sys_handle_close(sys_handle_t h);
sys_err_t sys_handle_type(sys_handle_t h, uint32_t *out_type);
sys_err_t sys_handle_rights(sys_handle_t h, uint32_t *out_rights);

/* Duplication may only attenuate. new_rights must be a subset of the source
 * handle's rights, or the call fails with SYS_ERR_PERM_DENIED. */
sys_err_t sys_handle_dup(sys_handle_t h, uint32_t new_rights,
                         sys_handle_t *out);

/* ========================================================================== */
/* System Page and Static Pools                                               */
/* ========================================================================== */

#define SYS_FEATURE_MMU          (1u << 0)
#define SYS_FEATURE_MPU          (1u << 1)
#define SYS_FEATURE_SMP          (1u << 2)
#define SYS_FEATURE_DMA_COHERENT (1u << 3)
#define SYS_FEATURE_PAGING       (1u << 4)  /* Tier 2 only */
#define SYS_FEATURE_RAM_LOADER   (1u << 5)  /* compile-time; see section 15.2 */

/* Pool sizes are fixed at build time. There is no kernel heap, at any tier.
 * Published read-only so userspace can see the limits it works against. */
typedef struct sys_pool_limits {
    uint32_t max_processes;
    uint32_t max_threads;
    uint32_t max_channels;
    uint32_t max_grants;
    uint32_t max_timelines;
    uint32_t max_timers;
    uint32_t max_programs;
    uint32_t max_handles_per_proc;
    uint32_t pulse_queue_depth;     /* per channel */
    uint32_t pulse_queue_reserved;  /* slots system pulses may always use */
} sys_pool_limits_t;

typedef struct sys_syspage {
    volatile uint32_t seq;          /* seqlock; odd means write in progress */
    uint32_t          ticks_per_sec;
    uint32_t          page_size;
    uint32_t          cpu_count;
    uint32_t          feature_flags;
    uint32_t          prio_max;
    uint32_t          prio_rt_min;  /* at or above this, strict priority only */
    sys_pool_limits_t pools;
    uint64_t          uptime_ticks; /* read via sys_syspage_uptime() only */

    /* Seconds to add to uptime to obtain calendar time. Written by a
     * userspace time service, never computed or validated by the kernel,
     * which owns no clock driver. Scheduling, deadlines and timers are
     * defined against uptime alone, so adjusting this cannot perturb
     * real-time behaviour. Read via sys_syspage_utc_offset(). Section 14.5. */
    int64_t           utc_offset;
} sys_syspage_t;

typedef struct sys_proc_descriptor {
    sys_proc_id_t   pid;
    sys_syspage_t  *syspage;
    uint32_t        cpu;            /* current core affinity */
    uint32_t        _reserved;
} sys_proc_descriptor_t;

extern sys_proc_descriptor_t *__sys_current_proc;

#define SYS_SYSPAGE_ENTRY(field) (__sys_current_proc->syspage->field)

/* Torn-read-safe accessor for the 64-bit uptime field. Direct field access is
 * not valid on 32-bit targets. This is also the clock read that makes
 * absolute deadlines affordable: no syscall. See sections 6.2 and 14.3. */
static inline uint64_t sys_syspage_uptime(void)
{
    const sys_syspage_t *sp = __sys_current_proc->syspage;
    uint32_t s0, s1;
    uint64_t v;

    do {
        s0 = sp->seq;
        v  = sp->uptime_ticks;
        s1 = sp->seq;
    } while ((s0 & 1u) || (s0 != s1));

    return v;
}

/* Relative-to-absolute helper. Lives here rather than in the kernel. */
static inline sys_deadline_t sys_deadline_in(sys_ticks_t ticks)
{
    return (sys_deadline_t)(sys_syspage_uptime() + ticks);
}

/* Calendar time, seqlock-protected like uptime. Direct field access is not
 * valid on 32-bit targets. */
static inline int64_t sys_syspage_utc_offset(void)
{
    const sys_syspage_t *sp = __sys_current_proc->syspage;
    uint32_t s0, s1;
    int64_t  v;

    do {
        s0 = sp->seq;
        v  = sp->utc_offset;
        s1 = sp->seq;
    } while ((s0 & 1u) || (s0 != s1));

    return v;
}

/* Seconds since the epoch. No syscall: both terms come from the mapped
 * system page. */
static inline int64_t sys_time_now(void)
{
    const sys_syspage_t *sp = __sys_current_proc->syspage;
    return (int64_t)(sys_syspage_uptime() / sp->ticks_per_sec)
         + sys_syspage_utc_offset();
}

/* ========================================================================== */
/* Boot Handoff                                                               */
/* ========================================================================== */

typedef struct sys_mem_region {
    uintptr_t base;
    size_t    length;
    uint32_t  flags;
    uint32_t  _reserved;
} sys_mem_region_t;

/* Delivered to the root process only. It holds all authority at handoff and
 * distributes it by spawning children with explicit inherit arrays. */
typedef struct sys_bootinfo {
    uint32_t           version;
    uint32_t           region_count;
    sys_mem_region_t  *regions;
    sys_handle_t       root_pager;      /* SYS_HANDLE_INVALID below Tier 2 */
    sys_handle_t       boot_archive;    /* read-only region; see section 24.1 */
    sys_handle_t       platform_config; /* read-only region, or INVALID */
    sys_handle_t      *initial_handles;
    uint32_t           initial_count;
    uint32_t           _reserved;
} sys_bootinfo_t;

/* ========================================================================== */
/* Timelines and Fences                                                       */
/* ========================================================================== */

/* The system's single synchronization object. Driver completion, interrupt
 * delivery, timer expiry and thread exit are all timeline advances. */

sys_err_t sys_timeline_create(uint64_t initial_value, sys_handle_t *out);

/* Requires SYS_RIGHT_SIGNAL. Value must exceed the current value;
 * monotonicity is what makes a late waiter safe. */
sys_err_t sys_timeline_signal(sys_handle_t h, uint64_t value);

/* Requires SYS_RIGHT_WAIT. Blocks until the counter reaches value or the
 * timeline enters error state. Donates the caller's priority to the holder of
 * a signal-righted handle; kernel-signalled timelines have no donation
 * target. Waiting on a value not yet promised is legal. */
sys_err_t sys_timeline_wait(sys_handle_t h, uint64_t value,
                            sys_deadline_t deadline);

sys_err_t sys_timeline_query(sys_handle_t h, uint64_t *out_value,
                             sys_err_t *out_error);

/* Force-signal in error state, satisfying every outstanding and future wait.
 * Also triggered automatically by batch abort, by the last signal-righted
 * handle closing, by signaler death, and by timer destruction. Section 9.3. */
sys_err_t sys_timeline_fail(sys_handle_t h, sys_err_t code);

/* ========================================================================== */
/* Timers                                                                     */
/* ========================================================================== */

#define SYS_TIMER_ONESHOT   0
#define SYS_TIMER_PERIODIC  1

/* Advances the bound timeline at each deadline. Requires SYS_RIGHT_SIGNAL on
 * the timeline, which the kernel then holds for the timer's lifetime.
 *
 * Phase is absolute: the Nth activation occurs at first + N*period, measured
 * from the origin and never from the previous delivery, so the counter always
 * holds the number of activations that should have occurred. A task that
 * waited for N+1 and finds N+3 missed exactly two and knows when each should
 * have happened. Catching up is a single counter update, not a backlog.
 * Whether to catch up or skip is the caller's choice per iteration; the
 * kernel offers no policy. Destroying a timer force-signals its timeline in
 * error state. There is no timer pulse. See section 14.2. */
sys_err_t sys_timer_create(sys_handle_t timeline, uint32_t mode,
                           sys_deadline_t first, sys_ticks_t period,
                           sys_handle_t *out_timer);
sys_err_t sys_timer_cancel(sys_handle_t timer);

/* ========================================================================== */
/* Processes and Threads                                                      */
/* ========================================================================== */

#define SYS_PRIO_MIN            0
#define SYS_PRIO_CHAIN_MAX      8   /* donation chain bound, see section 21 */

#define SYS_SCHED_FIFO          0   /* default within a priority level */
#define SYS_SCHED_RR            1   /* opt-in, requires an explicit quantum */

/* Process reentrancy, following VSTa. See section 10.3. */
#define SYS_REENTRANT_SINGLE    0   /* one thread */
#define SYS_REENTRANT_SERIAL    1   /* many threads, one running at a time */
#define SYS_REENTRANT_PARALLEL  2   /* fully parallel */

/* Thread and process states, as reported by status queries. */
#define SYS_STATE_READY         0
#define SYS_STATE_RUNNING       1
#define SYS_STATE_BLOCKED       2
#define SYS_STATE_EXITED        3

typedef struct sys_thread_attr {
    uint32_t    priority;
    uint32_t    sched_policy;
    uint32_t    quantum_ticks;  /* SYS_SCHED_RR only */
    uint32_t    cpu;            /* initial core affinity */
    void       *stack;
    size_t      stack_size;
} sys_thread_attr_t;

/* A named directory handle. The array of these passed at spawn becomes the
 * child's entire filesystem namespace: there is no global namespace and no
 * ambient root. libc builds its mount table from this array, in the same way
 * a Unix runtime builds environ from envp. See section 20.4. */
typedef struct sys_ns_entry {
    const char   *prefix;   /* e.g. "/dev" */
    sys_handle_t  dir;      /* directory handle, moved into the child */
} sys_ns_entry_t;

typedef struct sys_spawn_attr {
    uint32_t          reentrancy;
    uint32_t          cohort_id;     /* 0 for none; fixed at configuration */

    /* Unnamed handles placed in the child's table by position, the way a
     * Unix child inherits descriptors 0, 1 and 2. */
    sys_handle_t     *inherit;
    size_t            inherit_count;

    /* Named directory handles. Becomes the child's namespace, and nothing
     * else does. An empty array is a child that can name no files at all,
     * which is what NS_NEW means. */
    sys_ns_entry_t   *ns;
    size_t            ns_count;

    int               argc;
    const char      **argv;
    const char      **envp;

    sys_handle_t      pager;         /* SYS_HANDLE_INVALID inherits parent's */
    sys_thread_attr_t initial_thread;
} sys_spawn_attr_t;

/* Delivered to the child's entry point. The kernel installs the handles and
 * reports where they landed in the child's table; the loader places the
 * record and the strings in the child's memory, as it is writing the image
 * anyway. libc consumes ns[] into its mount table before main() runs. */
typedef struct sys_startup {
    uint32_t          version;
    int               argc;
    const char      **argv;
    const char      **envp;
    uint32_t          ns_count;
    sys_ns_entry_t   *ns;            /* child-side handle values */
    uint32_t          inherit_count;
    sys_handle_t     *inherit;
} sys_startup_t;

sys_err_t sys_thread_create(const sys_thread_attr_t *attr,
                            void (*entry)(void *), void *arg,
                            sys_handle_t *out_thread);
void      sys_thread_exit(int32_t code);
sys_err_t sys_thread_set_priority(sys_handle_t thread, uint32_t priority);
sys_err_t sys_thread_migrate(sys_handle_t thread, uint32_t cpu);

sys_err_t sys_spawn(const char *path, const sys_spawn_attr_t *attr,
                    sys_handle_t *out_proc);
sys_err_t sys_proc_kill(sys_handle_t proc, int32_t code);

/* Completion timeline for a thread or process handle. Exit advances it, so
 * joining is an ordinary timeline wait and needs no new blocking primitive.
 *
 * The timeline is embedded in the object rather than drawn from the timeline
 * pool, so this cannot fail for want of a slot. The returned handle carries
 * SYS_RIGHT_WAIT only and the kernel holds the sole signal right: a process
 * able to signal its own completion could fake its own death and have its
 * supervisor reclaim it while running. See sections 8 and 10.4. */
sys_err_t sys_object_completion(sys_handle_t obj, sys_handle_t *out_timeline);

/* Any out_ parameter may be NULL if the caller does not want the value; the
 * kernel omits that write. Inputs are never optional. See section 5. */

/* A timeline value is a counter and cannot carry an exit code, so status is
 * read from the handle, which stays valid after exit until closed. */
sys_err_t sys_thread_status(sys_handle_t thread, int32_t *out_code,
                            uint32_t *out_state);
sys_err_t sys_proc_status(sys_handle_t proc, int32_t *out_code,
                          uint32_t *out_state);

/* Unblocks a thread; its pending blocking call returns SYS_ERR_INTERRUPTED.
 * Cancellation is prompt. If a server has already received the cancelled
 * client's message, the server additionally receives SYS_PULSE_DEAD_OWNER so
 * it can release the request's resources, and its eventual reply returns
 * SYS_ERR_STALE. A thread cancelled mid-batch stops at the next entry
 * boundary. See section 10.5. */
sys_err_t sys_thread_cancel(sys_handle_t thread);

/* ========================================================================== */
/* Channels and Synchronous IPC                                               */
/* ========================================================================== */

#define SYS_IPC_INLINE_BYTES     64
#define SYS_IPC_INLINE_HANDLES    4

#define SYS_MSG_IS_PULSE     (1u << 0)  /* recv: a pulse, not a message */
#define SYS_MSG_IS_FAULT     (1u << 1)  /* recv: a page fault, section 15.3 */
#define SYS_BUF_READ         (1u << 0)  /* buf_flags */
#define SYS_BUF_WRITE        (1u << 1)
#define SYS_MSG_PULSE_LOST   (1u << 3)  /* recv: user pulses were dropped */

typedef struct sys_ipc_msg {
    uint32_t      flags;
    uint32_t      inline_len;                       /* bytes valid below */

    /* Replies only: the server's verdict on the request, outside the
     * opaque payload so that the kernel can tell a delivered message from
     * a satisfied request without knowing the protocol. Zero is success.
     * Section 12.4. */
    sys_err_t     status;
    uint32_t      _pad;
    uint8_t       inline_data[SYS_IPC_INLINE_BYTES];

    /* Bulk payload. Moved for the duration of the call: the sender may not
     * touch the underlying memory until reply. See section 6.1.
     *
     * The address is the sender's on the way in and the receiver's on the
     * way out, rewritten at the rendezvous, so nothing crosses unchanged.
     * A field rather than a handle because a payload lives for one call;
     * a grant is for memory that outlives the call carrying it. */
    uintptr_t     buf;
    uint32_t      buf_len;
    uint32_t      buf_flags;      /* SYS_BUF_READ, SYS_BUF_WRITE */

    /* Handles, moved. Requires SYS_RIGHT_TRANSFER. Carried inline when
     * handle_count <= INLINE_HANDLES, otherwise entirely in handle_list. On
     * receive, handle_capacity is the caller's array size and handle_count is
     * written with the required count; a short buffer yields
     * SYS_ERR_TRUNCATED and moves nothing. */
    uint32_t      handle_count;
    uint32_t      handle_capacity;
    sys_handle_t  inline_handles[SYS_IPC_INLINE_HANDLES];
    sys_handle_t *handle_list;
} sys_ipc_msg_t;

/* Creating a channel returns SYS_RIGHT_RECV: holding that right is what it
 * means to be the server. */
sys_err_t sys_chan_create(sys_handle_t *out_chan);

/* Mint a connection to this channel. Requires SYS_RIGHT_RECV, so only a
 * server can do this; the resulting handle is then moved to a client. The
 * cookie is the server's own per-connection state, delivered with every
 * message that arrives on it and never interpreted by the kernel. Zero is
 * reserved to mean "sent on the bare channel, no connection". Section
 * 12.1. */
sys_err_t sys_chan_connect(sys_handle_t chan, uint64_t cookie,
                           uint32_t rights, sys_handle_t *out_conn);

/* Blocks until a server receives and replies. Sender queues are ordered by
 * sender priority, highest first, ties by arrival. `target` names a
 * connection, or a bare channel handle carrying SYS_RIGHT_SEND. */
sys_err_t sys_msg_send(sys_handle_t target, const sys_ipc_msg_t *send,
                       sys_ipc_msg_t *reply, sys_deadline_t deadline);

/* Returns the client thread id and the connection cookie. The id is valid
 * as a reply target only while that thread remains blocked; the generation
 * makes a stale reply detectable. Any thread in the server process may
 * reply, not only the one that received. */
sys_err_t sys_msg_recv(sys_handle_t chan, sys_ipc_msg_t *recv,
                       sys_thread_id_t *out_client, uint64_t *out_cookie,
                       sys_deadline_t deadline);

/* Never blocks. Returns SYS_ERR_STALE if the client is no longer blocked on
 * this request, which covers a client that died, timed out, or was
 * cancelled. */
sys_err_t sys_msg_reply(sys_thread_id_t client, const sys_ipc_msg_t *reply);

/* ========================================================================== */
/* Pulses                                                                     */
/* ========================================================================== */

#define SYS_PULSE_DEAD_OWNER    1   /* system pulse; uses reserved slots */
#define SYS_PULSE_USER_BASE  1024

/* For SYS_PULSE_DEAD_OWNER the value is SYS_ID_TAG32 of the departed thread's
 * id: index plus 12 bits of generation. A server matches it against
 * SYS_ID_TAG32(client_id) for the id it already holds. A bare index would be
 * vulnerable to the slot being recycled between enqueue and dequeue, which is
 * why a 64-bit build splits the identifier so that generation bits fall
 * below bit 32. See sections 7.1 and 10.5. */
typedef struct sys_pulse {
    uint16_t  code;
    uint16_t  _reserved;
    uint32_t  value;
} sys_pulse_t;

/* Never blocks. Safe from contexts where blocking is not permitted. User
 * pulses may be dropped when the queue fills, setting SYS_MSG_PULSE_LOST on
 * the next receive; system pulses use reserved slots that user pulses cannot
 * displace, because a lost death notification leaks resources permanently.
 * See section 13.1. */
sys_err_t sys_pulse_send(sys_handle_t chan, const sys_pulse_t *pulse);

/* ========================================================================== */
/* Interrupts                                                                 */
/* ========================================================================== */

/* Requires SYS_RIGHT_IRQ, and SYS_RIGHT_SIGNAL on the timeline. The ISR
 * advances the timeline by increment and returns; it runs no driver logic,
 * takes no lock a thread can hold, and does not allocate. Section 13.2. */
sys_err_t sys_irq_attach(uint32_t irq, sys_handle_t timeline,
                         uint64_t increment, sys_handle_t *out_irq);
sys_err_t sys_irq_detach(sys_handle_t irq);
sys_err_t sys_irq_unmask(sys_handle_t irq);

/* ========================================================================== */
/* Memory Grants and Scatter-Gather                                           */
/* ========================================================================== */

typedef struct sys_sg_entry {
    uintptr_t phys_addr;
    size_t    length;
} sys_sg_entry_t;

sys_err_t sys_grant_create(void *vaddr, size_t len, uint32_t rights,
                           sys_handle_t *out_grant);
sys_err_t sys_grant_map(sys_handle_t grant, void **out_vaddr, size_t *out_len);
sys_err_t sys_grant_revoke(sys_handle_t grant);

/* Caller supplies the array and its capacity. out_count always receives the
 * required entry count, so SYS_ERR_TRUNCATED can be retried with a correct
 * allocation. */
sys_err_t sys_sg_build(sys_handle_t grant,
                       sys_sg_entry_t *entries, size_t capacity,
                       size_t *out_count);

/* ========================================================================== */
/* Demand Paging (Tier 2 only)                                                */
/* ========================================================================== */

/* Carried in inline_data when SYS_MSG_IS_FAULT is set. The kernel synthesizes
 * the message as though the faulting thread had sent it, so the pager replies
 * with sys_msg_reply() carrying a grant.
 *
 * Mapping exists to bring in read-only shared library and program text, not
 * to provide POSIX mmap. There is no copy-on-write and no swap, so access is
 * always READ or EXEC, never WRITE; a write fault against a demand-paged
 * region is a program error. See sections 15.1 and 15.3. */
typedef struct sys_fault_msg {
    uintptr_t fault_addr;
    uintptr_t fault_pc;
    uint32_t  access;       /* SYS_RIGHT_READ or SYS_RIGHT_EXEC */
    uint32_t  _reserved;
} sys_fault_msg_t;

/* Walks the pager chain and rejects a cycle or an unterminated chain with
 * SYS_ERR_DEADLOCK. The chain must end in a memory-locked process with no
 * pager. See section 15.4. */
sys_err_t sys_thread_set_pager(sys_handle_t thread, sys_handle_t pager_chan);

/* Requires SYS_RIGHT_MEM_LOCK. Mandatory for every process on the paging
 * path. A memory-locked process that faults is fatal, not recoverable. */
sys_err_t sys_mem_lock(sys_handle_t proc);

/* ========================================================================== */
/* Priority-Inheriting Futexes                                                */
/* ========================================================================== */

#define SYS_FUTEX_WAIT          0
#define SYS_FUTEX_WAKE          1
#define SYS_FUTEX_LOCK_PI       2
#define SYS_FUTEX_UNLOCK_PI     3

/* PI protocol: the word holds SYS_ID_TAG31 of the owning thread: the index
 * plus as many generation bits as fit below SYS_FUTEX_WAITERS in the top
 * bit, which is 11 on a 64-bit build and 21 on a 32-bit one.
 * The generation bits are what let the word identify an incarnation rather
 * than merely a slot, so a thread that inherits a dead owner's index does not
 * inherit its lock. An owner that dies leaves the lock inconsistent and the
 * next acquirer receives SYS_ERR_OWNER_DEAD. See section 21.1.
 *
 * The futex uses a 31-bit tag rather than the 32-bit tag pulses use because
 * it must reserve the top bit for the waiters flag. The word stays 32 bits
 * because Tier 0 targets have no 64-bit atomic. */
#define SYS_FUTEX_WAITERS       (1u << 31)
#define SYS_FUTEX_OWNER_MASK    0x7FFFFFFFu  /* SYS_ID_TAG31 of the owner */

sys_err_t sys_futex(volatile uint32_t *addr, int op, uint32_t val,
                    sys_deadline_t deadline);

/* ========================================================================== */
/* Introspection                                                              */
/* ========================================================================== */

/* Text key-value attributes. The system never parses these strings; consumers
 * are development tools. Requires SYS_RIGHT_STAT on the target handle.
 *
 * Attribute reads are explicitly not real-time paths. Formatting is lazy, so
 * an unobserved system pays nothing, and no accessor may take a lock a
 * real-time thread can hold.
 *
 * There is no enumeration call. A caller inspects only objects it holds a
 * handle to, so inspection follows the supervision tree rather than searching
 * the system. See section 22.3.
 *
 * The macros for declaring attributes are a kernel-internal facility and are
 * not part of this interface. They belong with the object definitions they
 * describe, and they need a formatter that does not drag stdio into the
 * kernel. */

sys_err_t sys_stat_list(sys_handle_t obj, char *buf, size_t buflen,
                        size_t *out_len);
sys_err_t sys_stat_read(sys_handle_t obj, const char *key,
                        char *buf, size_t buflen, size_t *out_len);

/* ========================================================================== */
/* ========================================================================== */
/* Operations and the Submission Engine                                       */
/* ========================================================================== */

#define SYS_BATCH_REGS           16
#define SYS_BATCH_MAX_ENTRIES   256

/* Blocking ops. These are the only entries that can block. */
#define SYS_OP_NOP                0
#define SYS_OP_IPC_SEND           1
#define SYS_OP_IPC_RECV           2
#define SYS_OP_FENCE_WAIT         3

/* Non-blocking ops. Path resolution is absent: the mount table lives in libc
 * and resolution happens in userspace. See section 20. */
#define SYS_OP_IPC_REPLY          4
#define SYS_OP_FENCE_SIGNAL       5
#define SYS_OP_HANDLE_CLOSE       6
#define SYS_OP_HANDLE_DUP         7

/* Issuing ops. Start hardware work and write a timeline handle to reg_out,
 * returning immediately. The only source of concurrency in an in-context
 * engine. See section 17.3. */
#define SYS_OP_DMA_ISSUE          8
#define SYS_OP_READ_ISSUE         9
#define SYS_OP_WRITE_ISSUE       10

/* Entry flags. Ignored for a single-operation call. */
#define SYS_BATCH_LINK      (1u << 0)  /* next entry depends on this one */
#define SYS_BATCH_REG_IN    (1u << 1)  /* take the target from reg_in */
#define SYS_BATCH_REG_OUT   (1u << 2)  /* write the result to reg_out */

/* Register tags. A register read as the wrong tag fails with
 * SYS_ERR_INVALID_ARG rather than fabricating a handle from an integer. */
#define SYS_REG_EMPTY       0
#define SYS_REG_HANDLE      1
#define SYS_REG_SCALAR      2

/* One descriptor serves both the single-operation call and the submission
 * engine; a single operation is the count-of-one case. The reg_in, reg_out and
 * LINK fields are meaningful only when count exceeds one. See section 23.1. */
typedef struct sys_op {
    uint16_t      opcode;
    uint16_t      flags;
    uint16_t      reg_in;       /* ignored unless SYS_BATCH_REG_IN is set */
    uint16_t      reg_out;      /* ignored unless SYS_BATCH_REG_OUT is set */

    sys_handle_t  target;       /* ignored when SYS_BATCH_REG_IN is set */
    uintptr_t     arg_ptr;      /* op-specific argument structure */
    uint32_t      arg_len;
    uint32_t      _reserved;
    uint64_t      user_data;    /* opaque caller tag, never interpreted */

    /* Written by the kernel. Keeping status in the entry avoids validating a
     * separate user status pointer per entry. See section 17.6. */
    sys_err_t     status;
    uint32_t      _pad;
} sys_op_t;

/* Executes one operation. Equivalent to sys_batch_exec with a count of one,
 * and provided separately because it is the overwhelmingly common case. */
sys_err_t sys_op_exec(sys_op_t *op, sys_deadline_t deadline);

/* Executes entries in order, in the calling thread's context and at its
 * priority. Preemption points are at entry boundaries. Kernel execution time
 * is bounded by the sum of entry costs; wall-clock time is not, because
 * blocking ops may wait. On abort, every timeline the batch was going to
 * signal is force-signalled in error state. */
sys_err_t sys_batch_exec(sys_op_t *entries, size_t count,
                         sys_deadline_t deadline);

/* Prepared programs: validated once, executed many times. */
sys_err_t sys_prog_load(const sys_op_t *entries, size_t count,
                        sys_handle_t *out_prog);
sys_err_t sys_prog_exec(sys_handle_t prog, sys_handle_t *args,
                        size_t arg_count, sys_deadline_t deadline);

#ifdef __cplusplus
}
#endif

#endif /* OS_SYS_H */
```

---

# Part III — Implementation Notes

This part is advisory. It names the surface a port has to supply underneath Part II, and suggests the data structures that are the obvious choices, so that neither has to be rediscovered.

`ROADMAP.md` gives the order to build things in.

---

## 27. What a Port Provides

Part II is deliberately silent about hardware. This is the surface a port supplies underneath it. The list is short, which is the point: everything else in the system is written once and shared.

**Boot-time construction of `sys_bootinfo_t`.** The kernel is entered with that structure already built, so a port supplies whatever stage builds it. **The simplest such stage takes no handover at all**: where the memory size and the archive address are fixed by the build, the port supplies both as constants, leaves `platform_config` invalid, and reads nothing. A pinned emulator target or a single-board product should prefer this, since discovering a constant is not an improvement on stating one. On a target whose bootloader hands over a flattened device tree, in `x0` on aarch64 and `a1` on riscv64, that stage reads the memory map from `/memory` and the boot archive bounds from `/chosen`, then passes the blob itself through to the root process as the platform configuration region of section 24.1. It is a targeted property lookup and not a general device tree library, because a statically configured kernel needs nothing else from one.

This stage runs at the kernel's own privilege level, so it is not an isolation claim. What it buys is that the kernel's entry contract is byte-identical on every port, that the parser is a small and separable thing to get wrong, and that a target placing its archive at a build-known address carries no parser at all. A port booting through UEFI, where the archive arrives by `EFI_LOAD_FILE2` rather than through `/chosen`, or an ACPI machine with no device tree at all, differs only inside this stage.

**Integrity of the handover data**, where the platform verifies the boot image at all. The memory map and the archive bounds must be covered by the same verification as the image. A device tree an attacker can edit is one whose `/chosen` names an archive of their choosing, and the whole system is then loaded from it. A port that cannot verify its handover data should say so, because section 24.2's argument about reviewable configuration does not hold for it.

**Context switch.** Save and restore of a thread's register state, including whatever floating-point or vector state the target has and the system chooses to preserve. This is the only place in the kernel that knows the register file.

**Trap entry and exit.** The instruction and the entry sequence for section 23, including the register mapping and the clearing of caller-saved registers on return.

**A monotonic time source** with a resolution at least as fine as `ticks_per_sec` and a width that does not wrap within the system's service life, or a documented wrap handling.

**A programmable timer interrupt** capable of expiring at an absolute time, since section 14.4 defines deadlines absolutely. A port with only an interval timer must convert, and the conversion error becomes part of the timer accuracy it publishes.

It must be programmable **one-shot**, because section 14.1 has no tick and reprograms the timer at every scheduling decision that changes the earliest pending event. The cost of that reprogramming is a port characteristic worth knowing: on a part where the timer sits behind a slow peripheral bus, it is on the dispatch path and is not small.

**A mapping from every hardware exception to a fault kind.** Section 16.1 enumerates the kinds, and a port must state which of its exceptions produces which. An exception with no mapping has undefined behaviour, and the ones most often left out are the imprecise external aborts that section 16.1 separates for exactly that reason. Where the hardware cannot raise a kind at all, which is most of them on Tier 0, the mapping says so rather than omitting the row.

**Interrupt controller programming**: mask, unmask, acknowledge, and priority if the hardware has it.

**Memory protection programming**, per tier. Tier 0 supplies nothing. Tier 2 programs page tables and provides TLB maintenance.

**Cache maintenance for DMA**, on targets where the DMA engines are not coherent with the CPU. This is invisible in Part II because it is invisible to the API, and getting it wrong produces data corruption that looks like a driver bug.

**An inter-processor interrupt**, on multicore targets only. Section 11.4 requires it for cross-core priority donation.

**The idle behaviour.** What a core does with nothing runnable is a port decision, and it interacts with interrupt latency directly: a deep sleep state that takes microseconds to exit adds that time to the latency of section 13.3, so it is either accounted for or disabled.

---

## 28. Suggested Structures

Offered because they are the obvious choices and a team should not have to re-derive them. None is required.

**Ready queue.** A bitmap of non-empty priority levels plus a FIFO per level. Selecting the highest runnable thread is a count-leading-zeros on the bitmap and a head-of-list read. At 32 levels the bitmap is one word, which is why section 11.1 suggests 32 for Tier 0.

**Handle table.** A flat array indexed by the handle's index field, each slot holding a generation counter, an object pointer, a rights mask, and a free-list link. Allocation pops the free list, and closing pushes and increments the generation. The index field of section 7.1 bounds the array; a sparse table only pays off at sizes a static configuration will not reach.

**Receive queue.** For the small number of blocked senders a real system has, an insertion-sorted linked list ordered by effective priority is simpler than a heap and faster in practice. Note *effective* priority: a donated priority changes a blocked sender's queue position, so the insertion point can move while it waits.

**Timeline waiters.** A list sorted by awaited value. Signalling walks from the head and releases while the head's value is reached, which is the whole operation. Entering the error state releases the entire list without regard to value.

**Thread state.** Keep the object and the kernel stack in one allocation. It halves the pool bookkeeping and makes the common lookup a single dereference.

**The blocked-on pointer.** Give every thread one field naming what it is blocked on. Section 21's chain walk, the cancellation path, and the introspection attribute of section 22.2 are all reads of that single field, and it is the direct representation of the invariant in section 5.

---

# Index

*The bold page number is where the term is actually explained.*

::: index
:::
