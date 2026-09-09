/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Build-time configuration.
 *
 * There is no kernel heap. Every object type has a pool whose size is fixed
 * here, so the kernel's object memory is a property of this file rather than
 * of what applications do. A system that exhausts a pool is reconfigured and
 * rebuilt; see design.md section 8.
 *
 * These are the values published read-only to userland in the system page as
 * struct sys_pool_limits, which is why they carry the SYS_ prefix rather than
 * a kernel-private one.
 *
 * The numbers below are placeholders sized for a Tier 0 target running the
 * milestone 1 vertical slice. They have not been justified by measurement,
 * because there is nothing yet to measure.
 */

#ifndef _SYS_CONFIG_H_
#define _SYS_CONFIG_H_

/*
 * Sixteen was enough until the boot checks started spawning programs
 * that spawn programs. Nothing is reaped before M7, so every process
 * the checks create holds its slot for the rest of the run, and the
 * seventeenth spawn is what found this: section 8 calls exhaustion a
 * configuration error rather than a condition to recover from, and
 * this is what fixing one looks like. It is also the clearest measure
 * so far of what reaping buys, since a system that returned slots
 * would need about four.
 */
/*
 * Which protection tier this image is built for, per section 6. The
 * build supplies it; the default is here so that a translation unit
 * compiled without the flag is Tier 0 rather than undefined.
 *
 * Tier 0 is flat memory that trusts the contract of section 6.1 and
 * Tier 2 is an MMU that enforces it. Nothing else in the interface
 * differs, which is the claim milestone 4 exists to test.
 */
#ifndef SYS_TIER
#define SYS_TIER                0
#endif

#if SYS_TIER != 0 && SYS_TIER != 2
#error "SYS_TIER must be 0 or 2; see section 6"
#endif

/*
 * How many of each there can be.
 *
 * These are sized by the boot check sequence rather than by a workload,
 * and that is worth saying rather than leaving to be discovered. Nothing
 * is reaped until M7: a process that has exited keeps its slot so that
 * its status stays readable, so a run that starts forty programs one
 * after another needs forty slots even though two are ever alive. What
 * makes these numbers a property of a system instead of a property of
 * its tests is reclamation, and that is M7.
 */

/*
 * Lowered, for the first time, because they have stopped meaning what
 * the paragraph above says they mean.
 *
 * Three milestones running raised these because nothing was reaped and
 * a process that had exited kept its slot for ever, so the numbers
 * tracked how long the boot sequence was. M7 stage 1 returns a slot
 * when nothing can ask about the object any more, and the boot
 * sequence that needed forty-six processes now needs thirteen at once
 * and fifteen threads. These are set at roughly twice that, which is
 * headroom for the stages still to come rather than a measurement.
 *
 * They are still not a property of the system in general. What they
 * are now is a property of how much runs at the same time, which is a
 * far better thing for them to be and is what section 8 wanted.
 */
/*
 * How many cores this kernel will use, per section 11.4.
 *
 * Two, because two is enough to falsify everything a partitioned
 * scheduler claims and small enough that what each core did fits in a
 * report a person reads. Nothing here assumes the number: a core is
 * found, claims an entry, and is scheduled on its own.
 *
 * One at Tier 0, per section 13.3. That tier disables interrupts inside
 * the kernel and bounds latency by where the preemption points are, on
 * the argument that such parts are simple and often uniprocessor, and a
 * second core would need the locking the argument is avoiding.
 */
#if SYS_TIER == 2
#define SYS_MAX_CORES              2
#else
#define SYS_MAX_CORES              1
#endif

#define SYS_MAX_PROCESSES         24

/*
 * How many address space identifiers this system will use, whatever the
 * hardware is willing to give.
 *
 * Both ports answer sixteen bits, which is 65535 names for a system
 * that has 24 processes. What the cap is for is the list of names
 * handed back, which is an array of this size and has to be one: a
 * kernel that allocates in order to free is a kernel with a way to fail
 * while giving something up.
 *
 * Thirty-two is above the number of spaces ever live at once, which is
 * what keeps a space from having to run with no name at all. It is not
 * what makes names get reused; a name goes back the moment its space
 * dies and the next space takes it, which happens at any cap. What a
 * cap this size makes possible is running out, which is a state the
 * checks reach on purpose. See vm_as_create() and asid_reuse_checks().
 */
#define SYS_ASID_LIMIT            32
#define SYS_MAX_THREADS           32
/*
 * Channels are allocated and never released: section 18.2's teardown is
 * not built, so a channel a check creates is a slot the rest of the
 * boot does not have. At 32 the boot reached the cohort check with one
 * slot left, which made the next check that needed a channel impossible
 * to write rather than merely expensive. Raised so that there is room
 * to check things, and it stays a number worth watching until a channel
 * can be given back.
 */
#define SYS_MAX_CHANNELS          48

/*
 * A channel's pulse queue, per section 13.1, and the part of it user
 * pulses may not touch.
 *
 * The reservation is correctness rather than politeness. A dropped
 * death notice leaks a dead client's resources for good, and a process
 * that floods a channel with pulses of its own must not be able to
 * cause that. Two is enough for what can be outstanding at once: a
 * server serves one client at a time, so it can be owed one death
 * notice, and the second is slack for a notice that arrives while the
 * first is being collected.
 */
#define SYS_PULSE_SLOTS            8
#define SYS_PULSE_RESERVED         2
#define SYS_MAX_GRANTS            64
#define SYS_MAX_TIMELINES         64
#define SYS_MAX_TIMERS            32
/* Prepared programs held at once, per section 17.7. */
#define SYS_MAX_PREPARED           4

#define SYS_MAX_PROGRAMS           8

/*
 * Frames the bitmap of sys/page.h can cover, which is a ceiling on the
 * memory a port may hand over rather than an amount reserved: the
 * bitmap is one bit each, so this costs sixteen kilobytes for the half
 * a gigabyte it covers. A machine with more says so at boot and uses
 * this much.
 */
#define SYS_MAX_FRAMES        131072

/* Per-process, not a global pool. Present here because it is one of the
 * limits the system page publishes. The handle table arrives in stage 4. */
#define SYS_MAX_HANDLES_PER_PROC  64

/*
 * Kernel stack per thread, carried inside the thread object per section 28.
 * This dominates kernel memory: at the thread count above it is two orders
 * of magnitude larger than every other pool combined. It is a guess, and
 * the first thing to revisit once anything real runs on one.
 */
#define SYS_KSTACK_SIZE         4096

/*
 * Priority levels. Section 11.1 suggests 32 on Tier 0, where the ready
 * bitmap is one word and finding the highest runnable thread is a single
 * count-leading-zeros instruction.
 */
#define SYS_PRIO_COUNT          32
#define SYS_PRIO_MAX            (SYS_PRIO_COUNT - 1)

/*
 * The floor of the real-time band. Section 11.2 schedules everything at or
 * above this by strict priority, and everything below it from the
 * proportional tree of section 11.5.
 *
 * That tree does not exist, and a thread scheduled from a band with no
 * scheduler behind it would simply never run. So the floor is zero and the
 * whole range is the real-time band. Section 11.2 already names this as the
 * degenerate case: a system with everything above the floor has no
 * scheduler left, only a priority list. That is exactly what this is, and
 * it stays that way until the tree exists.
 */
#define SYS_PRIO_RT_MIN         0

/*
 * Interrupt numbers the kernel will bind. Sized to cover the timer PPI on
 * aarch64, which is 30, rather than to any real device census.
 */
#define SYS_MAX_IRQS            64

/*
 * Depth of the priority donation chain of section 12.3. A send that would
 * exceed it fails rather than walking an unbounded structure with
 * interrupts disabled.
 */
#define SYS_PRIO_CHAIN_MAX      8

/*
 * How many entries one submission may carry, per section 17.2. A batch
 * is a linear array with no branches, so its cost is the sum of its
 * entries and this bounds that sum.
 *
 * Section 13.3 says this constant does not appear in interrupt latency
 * at all and that the cost of one entry does, because the batch is
 * preemptible at every entry boundary. That claim becomes checkable
 * with this milestone and not before.
 */
#define SYS_BATCH_MAX_ENTRIES   16

/*
 * The debug console of section 22.4: a system call that prints, holding no
 * handle and asking nothing of the caller.
 *
 * It is ambient authority and a permanent, gated exception to the
 * capability model rather than scaffolding. A facility that exists only in
 * development images means the image being debugged is not the image being
 * shipped, which is the situation that makes a production-only fault
 * undiagnosable. So it stays, and what varies is whether a build carries
 * it and whether a build that carries it has it live.
 *
 * SYS_DEBUG_CONSOLE decides whether the operation is compiled in at all.
 * A build without it has no code for this and no way to acquire any.
 *
 * SYS_DEBUG_CONSOLE_INIT decides whether a build that carries it starts
 * with it live. An image can therefore ship with the console dormant and
 * have it turned on to diagnose something, without being a different
 * image. The kernel owns that switch; nothing reachable from a system
 * call moves it, because a hole a program can open for itself is not
 * gated at all.
 *
 * The length bound is not politeness. The write runs in the kernel at the
 * caller's priority, and on Tier 0 with interrupts disabled, so an
 * unbounded one is an unbounded non-preemptible section and lands straight
 * in the interrupt latency of section 13.3.
 */
#define SYS_DEBUG_CONSOLE       1
#define SYS_DEBUG_CONSOLE_INIT  1

/*
 * Blocking transitions the tracer of section 22.2 keeps. Sized to hold a
 * whole file read with room to see the shape of it; the ring overwrites
 * rather than stopping, and reports that it did.
 */
#define TRACE_EVENTS            32

/* Connections, which are what a client holds onto a server. */
#define SYS_MAX_CONNECTIONS     32

#endif /* _SYS_CONFIG_H_ */
