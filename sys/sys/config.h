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

#endif /* _SYS_CONFIG_H_ */
