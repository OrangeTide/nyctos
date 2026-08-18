/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The dispatch decision: which runnable thread runs.
 *
 * Section 11.2 keeps this in one place deliberately, separate from the
 * mechanisms that make a thread runnable, because the latency claims are
 * claims about this code and a reader needs it in one piece.
 *
 * Only the real-time band of section 11.2 exists here: a bitmap of
 * non-empty priority levels and a FIFO per level, so selecting the highest
 * runnable thread is a count-leading-zeros and a head-of-list read, and
 * costs the same whatever the number of runnable threads. The proportional
 * tree below the floor is not built; see SYS_PRIO_RT_MIN in config.h.
 *
 * Section 11.2 closes the list of points at which a decision is taken. Of
 * that list, the two that can happen yet are here: a thread blocks, and a
 * thread is made runnable. Quantum expiry and return from interrupt arrive
 * with the timer and the interrupt path.
 */

#ifndef _SYS_SCHED_H_
#define _SYS_SCHED_H_

#include <sys/thread.h>

void sched_init(void);

/*
 * Make t runnable. If it outranks the running thread, it takes the core
 * before this returns: section 11.3 says a thread becoming runnable at a
 * higher priority preempts rather than waiting for a quantum.
 */
void sched_ready(struct thread *t);

/* Current goes to the tail of its own level and the highest runnable runs. */
void sched_yield(void);

/*
 * Current leaves the run set without being queued, and the highest runnable
 * thread takes the core. Returns when something makes current runnable
 * again, which for a thread that has exited is never.
 */
void sched_block(void);

#endif /* _SYS_SCHED_H_ */
