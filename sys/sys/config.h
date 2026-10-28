/* config.h : build-time kernel configuration constants.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Defines SYS_TIER, the protection tier of the build, which defaults to
 * Tier 0. Defines the size of each kernel object pool, including
 * SYS_MAX_PROCESSES, SYS_MAX_CHANNELS, and SYS_PRIO_COUNT. The kernel has
 * no heap, so these sizes set how much object memory it can use. The same
 * limits are published to programs as struct sys_pool_limits in the system
 * page. Included by the kernel object, scheduler, IPC, and init code.
 *
 */

#ifndef _SYS_CONFIG_H_
#define _SYS_CONFIG_H_

/*
 * Which protection tier this image is built for. The
 * build supplies it; the default is here so that a translation unit
 * compiled without the flag is Tier 0 rather than undefined.
 *
 * Tier 0 is flat memory that trusts the contract of the interface and
 * Tier 2 is an MMU that enforces it. Nothing else in the interface
 * differs.
 */
#ifndef SYS_TIER
#define SYS_TIER                0
#endif

#if SYS_TIER != 0 && SYS_TIER != 2
#error "SYS_TIER must be 0 or 2"
#endif

/*
 * How many of each there can be.
 *
 * These are sized by the boot check sequence rather than by a workload.
 */
/*
 * How many cores this kernel will use.
 *
 * Two, because two is enough to falsify everything a partitioned
 * scheduler claims and small enough that what each core did fits in a
 * report a person reads. Nothing here assumes the number: a core is
 * found, claims an entry, and is scheduled on its own.
 *
 * One at Tier 0. That tier disables interrupts inside
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
 * Channels are allocated and never released, because teardown is not
 * built. A channel a check creates stays in use for the rest of the
 * boot, so this number is worth watching until a channel can be given
 * back.
 */
#define SYS_MAX_CHANNELS          48

/*
 * A channel's pulse queue, and the part of it user
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
/* Prepared programs held at once. */
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
 * limits the system page publishes. */
#define SYS_MAX_HANDLES_PER_PROC  64

/*
 * Kernel stack per thread, carried inside the thread object.
 * This dominates kernel memory: at the thread count above it is two orders
 * of magnitude larger than every other pool combined. It is a guess, and
 * the first thing to revisit once anything real runs on one.
 */
#define SYS_KSTACK_SIZE         4096

/*
 * Priority levels. 32 fits Tier 0, where the ready
 * bitmap is one word and finding the highest runnable thread is a single
 * count-leading-zeros instruction.
 */
#define SYS_PRIO_COUNT          32
#define SYS_PRIO_MAX            (SYS_PRIO_COUNT - 1)

/*
 * The floor of the real-time band. Everything at or above this is
 * scheduled by strict priority, and everything below it would be
 * scheduled from a proportional tree.
 *
 * That tree does not exist, and a thread scheduled from a band with no
 * scheduler behind it would simply never run. So the floor is zero and the
 * whole range is the real-time band. A system with everything above the
 * floor has no scheduler left, only a priority list. That is exactly what
 * this is.
 */
#define SYS_PRIO_RT_MIN         0

/*
 * Interrupt numbers the kernel will bind. Sized to cover the highest the
 * ports name: on aarch64 the virtio-mmio block device is the last transport
 * slot, GIC INTID 79, so the table reaches past it.
 */
#define SYS_MAX_IRQS            128

/*
 * Depth of the priority donation chain. A send that would
 * exceed it fails rather than walking an unbounded structure with
 * interrupts disabled.
 */
#define SYS_PRIO_CHAIN_MAX      8

/*
 * How many entries one submission may carry. A batch is a linear array
 * with no branches, so its cost is the sum of its entries and this
 * bounds that sum.
 *
 * This constant does not appear in interrupt latency. The cost of one
 * entry does, because the batch is preemptible at every entry boundary.
 */
#define SYS_BATCH_MAX_ENTRIES   16

/*
 * The debug console: a system call that prints, holding no
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
 * in the interrupt latency.
 */
#define SYS_DEBUG_CONSOLE       1
#define SYS_DEBUG_CONSOLE_INIT  1

/*
 * Blocking transitions the tracer keeps. Sized to hold a
 * whole file read with room to see the shape of it; the ring overwrites
 * rather than stopping, and reports that it did.
 */
#define TRACE_EVENTS            32

/* Connections, which are what a client holds onto a server. */
#define SYS_MAX_CONNECTIONS     32

#endif /* _SYS_CONFIG_H_ */
