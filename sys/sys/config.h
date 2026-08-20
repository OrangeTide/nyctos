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

#define SYS_MAX_PROCESSES         16
#define SYS_MAX_THREADS           64
#define SYS_MAX_CHANNELS          32
#define SYS_MAX_GRANTS            64
#define SYS_MAX_TIMELINES         64
#define SYS_MAX_TIMERS            32
#define SYS_MAX_PROGRAMS           8

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
 * The debug console of section 22.4: a system call that prints, holding no
 * handle and asking nothing of the caller.
 *
 * It is ambient authority and a hole in the capability model, which is why
 * it is a build option whose absence is the default and whose presence the
 * kernel announces at boot. A build with this on is a development image.
 *
 * The length bound is not politeness. The write runs in the kernel at the
 * caller's priority, and on Tier 0 with interrupts disabled, so an
 * unbounded one is an unbounded non-preemptible section and lands straight
 * in the interrupt latency of section 13.3.
 */
#define SYS_DEBUG_CONSOLE       1

/*
 * Blocking transitions the tracer of section 22.2 keeps. Sized to hold a
 * whole file read with room to see the shape of it; the ring overwrites
 * rather than stopping, and reports that it did.
 */
#define TRACE_EVENTS            32

/* Connections, which are what a client holds onto a server. */
#define SYS_MAX_CONNECTIONS     32

#endif /* _SYS_CONFIG_H_ */
