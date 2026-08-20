---
title: Nyctos
subtitle: Proposals
revision: "1"
---

# Proposals

Ideas that are not being built. Each was designed far enough to be worth
keeping, and each is out of scope for the system described in `design.md`.

This file exists so that `design.md` contains only what is intended to be
built. Material here is not a commitment, not a plan, and not a schedule.
Several entries would change the shape of the kernel substantially, and none of
them should be added on the strength of the argument written here alone. The
system does not work yet. Until it does, an idea that survived a design
conversation has been tested against nothing.

References below name sections of `design.md` by title rather than by number,
because the numbers move.

---

## 1. Resource Containers

A resource accounting boundary, and nothing else. It would confer no naming, no
authority, no shared memory, and no shared fate.

Containers elsewhere are bundles: a namespace, a filesystem view, a network
stack, a resource limit and an image format packaged together. Here three
quarters of that bundle already exists and is not part of this proposal. The
namespace design gives the naming boundary, since a process sees the directories
it was launched with and there is no ambient root to escape to. Handles give the
authority boundary. What is missing, and all that is missing, is a way to say
that a group of processes together may use no more than a stated share of the
machine.

### 1.1 The Four Resources

Each already has a mechanism, and a container would add a boundary rather than a
mechanism.

**CPU** is a node in the scheduling tree. A container is another internal node,
with cohort nodes beneath it, which requires no new scheduling concept. The
real-time band is excluded: threads above the real-time floor are scheduled by
priority alone, so a container cannot bound a real-time thread's CPU use and
must not appear to.

**Kernel objects** are a quota against the static pools, bounding the threads,
channels, timelines and handles a group may hold. Because the pools are
statically sized, a quota is a division of something already fixed.

**Memory** is largely settled. There is neither swap nor overcommit, so a
container's memory is the grants its processes hold. There is nothing to reclaim
under pressure because there is no pressure mechanism.

**Naming** is the array of directory handles the processes were launched with,
and nothing else.

### 1.2 Nesting

A cohort would belong to exactly one container. The two are different kinds of
group and conflating them is the most likely way to get this wrong. A cohort is
a protection and failure boundary, fixed at build time because membership grants
access to peer memory. A container is a resource boundary, implying neither
shared memory nor shared fate.

Strict nesting keeps the scheduling tree an actual tree, which is what bounds the
scheduling walk by depth. If nesting proved awkward, the fallback is containers
as a pure accounting overlay with no structural relationship to cohorts. That is
weaker, because the tree stops being a tree, but it is the retreat to take
rather than inventing a third kind of group.

### 1.3 The Unresolved Part

Quota enforcement is genuinely unresolved. Three questions, stated rather than
answered.

**What happens when a container reaches its object limit?** The candidate answer
is that allocation fails exactly as pool exhaustion does, so that a container
limit is indistinguishable from a smaller machine. Attractive because it
introduces no new error and no new state. Unattractive because it makes a
container's failures depend on the order its processes start.

**Are quotas reserved or merely capped?** A reserved quota is subtracted from the
pools at configuration time and is guaranteed available, making admission a
static question. A capped quota is checked at allocation, which allows
overcommitting the pools and is therefore strictly more useful and strictly less
analysable. The static configuration principle argues for reservation. Nothing
else does.

**Can a container be resized while running?** Enlarging is easy and shrinking is
not, because shrinking below current use has no defined meaning. The candidate
answer is that a shrink sets a ceiling that takes effect as objects are
released, so the container converges rather than being forced. Whether
convergence that may never complete is acceptable needs a real workload to
answer.

---

## 2. Region-Based Protection

A third protection level between flat memory and the MMU, using an ARM MPU or
RISC-V PMP to give processes isolated memory windows without virtual mapping.

The design carries two levels: flat memory that trusts the memory contract, and
an MMU that enforces it. A middle level would enforce region boundaries without
address translation.

Removed because it triples the explanation of every memory mechanism while
adding a third thing to test, and because the two-level form already carries the
whole lesson. What a contract means, and what hardware can enforce of it, is
demonstrated by two points as well as by three.

Worth noting that the riscv64 target already programs PMP, permissively, to
reach unprivileged mode. The hardware is present. Only the semantics are absent.

---

## 3. Deferred Mechanisms

Smaller items. Each states what is absent and what its absence buys, because in
every case the absence is paying for something.

**Writeback paging.** Mapping is read-only. This buys the removal of transparent
dirty tracking, of swap, and of a plain store instruction that can block on a
pager. The extension would be explicit sync rather than transparent dirty
tracking, since a filesystem already knows its own dirty ranges. The cost is that
the write fault returns, and a store to a mapped page becomes an operation that
may block, invisibly, at a point the source does not mark.

**Waiting on several fences at once.** There is no merge, and dependencies are
waited on one at a time. This buys constant-time single-target priority donation
and precise error attribution. `SYS_OP_FENCE_WAIT_MULTI` is described under "There Is No Merge
Operation" in `design.md`, as the extension to make if fixed batch shape ever
proves to matter.

**Wait-set objects.** A thread waits on one source, and a server needing several
uses several threads. This buys a kernel with no queue object and keeps the
blocking invariant trivially true. If the stacks ever prove unaffordable, the
extension is the **queue model, not the array model**: a caller passing a list of
objects and blocking until one signals does not scale, because every wait costs
time proportional to the list and the list is rebuilt whenever the set changes. A
queue that objects register against once and the waiter drains has neither
property. Evaluate first whether letting a timeline enqueue a coalesced pulse on
a channel is sufficient, since that gives one receive point for messages, pulses
and completions with no new object type at all.

**Gang scheduling.** Cohort scheduling nodes approximate co-scheduling by keeping
yielded time within a related group. This buys per-core independence. The
extension would be statically declared gangs with reserved cores, admitted
atomically. The cost is that dispatch couples cores that were independent, and
an inter-processor interrupt to every member enters the analysis. Dynamically
formed gangs additionally make admission a global decision and should not be
attempted.

**Return-path short-circuiting.** A read that crosses a filesystem and a
block driver unwinds through both on the way back, so the reply pays a
rendezvous per layer for data neither layer looks at. The extension is to
let a server forward a request and hand the reply along with it: the
driver completes the transfer and replies to the original client, and the
filesystem is out of the return path entirely. With the client's own
buffer travelling down the chain, per the reply-path caveat of section
6.1, the transfer lands where it is wanted and only a status and a count
come back. Four rendezvous become two.

**Most of the mechanism is already here, which is the reason to write this
down rather than to build it.** A reply is addressed by client thread id
and not by channel (section 12.2), so a forwarded reply needs no new
object and no reply capability: the id is the token, and passing it on is
passing a value. `channel_reply` looks the thread up and checks that it is
still waiting, and nothing in it requires the replying thread to be the
one that received.

That last sentence is also the problem. **There is no control over who may
reply**, so forwarding and forging are the same operation, and today the
only thing standing between them is that a thread id is hard to guess.
Section 12.2 says the id "permits exactly one thing, which is completing
the request the client is blocked in", which is precisely the authority a
forwarded reply needs and precisely the authority a third party should not
have. Making forwarding deliberate means making an unforwarded reply
checkable, which is a rule the reply path does not currently have.

Two further costs. **Donation would have to move with the reply.**
`undonate` runs only when the replying thread is the one the client
donated to, so a driver replying to a client it never received from keeps
whatever priority it was donated and the client's donation is never
returned. Section 12.3 says donation "reverts on reply" without saying
which reply, and a forwarded chain is where that matters. And **the
intermediate server stops learning whether the work succeeded**, which is
tolerable for a stateless forwarder and not for one keeping a cache.

The trigger is a layered driver stack that measures badly, which needs M6
underneath it. Doing it earlier would optimise a path with one layer in
it. `REF/ai-5` collects the background: L4's decoupled call and reply
phases, seL4 reply capabilities delegated down a chain, Fuchsia handing a
buffer capability to the block queue so file reads bypass the control
plane, and Mach's proxy memory objects.

**An asynchronous send.** Every send blocks, so a client cannot have two
requests to servers outstanding at once and the concurrency of section 17.3
reaches devices but not services. The extension is an issuing form of send
whose completion advances a timeline, with the reply written into the entry's
already-validated argument structure. This buys concurrent service calls from
one thread, and it costs the property that a blocked client thread is itself
the reply's queue slot (section 12.2). Something else would have to hold the
destination, which is a new object or a new thread state, and reply
addressing, cancellation and owner-death all read from that slot today. The
cheap alternative is more threads, and the trigger for revisiting is a client
that genuinely cannot afford one thread per outstanding request.

**A deferred entry flag for the submission engine.** A chain that aborts
partway can leave a connection open in a server, because the entry that would
have closed it never ran and its handle is only in a register (section 17.5).
The extension is a flag meaning "run even if the chain aborted, provided the
input register holds a value", which stays inside the branchless argument
because the predicate is whether a register was written. It costs a second
flag on every entry and a second pass over the array on the abort path. The
trigger is a real batch that acquires something it must release; the
compound requests of section 25.7 avoid the case rather than solving it.

**Connection death notification.** A server learns that a client is finished with
a connection only when the client closes it. Destroying the connection object
tells the server nothing. This buys a kernel in which closing a handle is a
purely local operation. The extension would be a reserved system pulse on the
channel, delivered when the connection object is destroyed. The cost is that
closing a handle stops being local: it can enqueue work on another process's
channel, so a full pulse queue makes handle closure a case with an error to
consider, on an operation that currently has none.

---

## 4. Subsystems

Whole areas of work, named so they stay named.

**A copy-on-write filesystem.** The design specifies a filesystem *protocol* and
uses a boot filesystem and a RAM disk as its implementations. A real storage
engine is a separate concern, and deliberately so: anything speaking the protocol
works, so the storage engine can be developed, tested and replaced without
touching the kernel or libc.

The shape considered is a copy-on-write B-tree with read-only snapshots and
rollback, taking from btrfs and HAMMER2. Copy-on-write rather than soft updates
because the correctness argument is short: write new nodes to fresh locations,
flush, then atomically update the superblock pointer. If power fails before the
third step the filesystem is untouched at the previous checkpoint. Snapshots
follow for free, since a snapshot is a duplicate pointer to a tree root and old
blocks are never overwritten. It is also the right shape for flash, because
copy-on-write writes flow sequentially.

Because namespace composition happens in the client, snapshot mounts and root
pivoting are ordinary handle-table operations rather than kernel work.

**Display and graphics.** A display server compositing client buffers into a
framebuffer, with a software 2D drawing library above it. No OpenGL and no
Vulkan: modern GPUs on the plausible target boards are undocumented, depend on
proprietary firmware, and would require porting an enormous userspace driver
stack and writing a shader compiler backend. That is a project, not a feature.

Two things are worth having without any of it. A linear framebuffer set up by
the bootloader and mapped to a display server covers everything up to an
interactive UI. And a hardware cursor plane, where the display controller
composites a small sprite during scanout, removes the save-render-blit-restore
cycle that dominates a software cursor, so moving the mouse costs a register
write instead of a screen region redraw. Raspberry Pi 4/5 is the plausible first
target for that, since its display engine is covered by open Linux drivers.

**Real hardware ports.** Everything today targets QEMU `virt`. Real boards bring
firmware handover, interrupt controller variation, clock and PHY bring-up, and
cache maintenance for non-coherent DMA. None of it is interesting until the
system works on the emulator.

**A self-hosting toolchain.** A Plan 9 style compiler suite with compile-time
backend selection, so the system can rebuild itself without a host toolchain.
Appealing and entirely separable from the operating system.
