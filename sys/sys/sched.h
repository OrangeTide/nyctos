/* sched.h : scheduler dispatch interface.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares the functions that make a thread runnable or block it, yield the
 * core, change a thread's priority, defer and undefer preemption, and migrate a
 * thread to another core. Included by the thread, interrupt, process,
 * timeline, time, channel and operation sources, and by sys/kern/init_main.c.
 *
 */

#ifndef _SYS_SCHED_H_
#define _SYS_SCHED_H_

#include <sys/thread.h>

void sched_init(void);

/*
 * Make t runnable. If it outranks the running thread, it takes the core
 * before this returns: a thread becoming runnable at a
 * higher priority preempts rather than waiting for a quantum.
 */
void sched_ready(struct thread *t);

/*
 * Make a thread runnable without taking the scheduling decision, for a
 * caller that is about to block and will take it in sched_block(). See the
 * definition: this exists because readying a peer and ceasing to be
 * runnable have to be one step.
 */
void sched_ready_nopreempt(struct thread *t);

/* Current goes to the tail of its own level and the highest runnable runs. */
void sched_yield(void);

/*
 * Current leaves the run set without being queued, and the highest runnable
 * thread takes the core. Returns when something makes current runnable
 * again, which for a thread that has exited is never.
 */
void sched_block(void);

/*
 * Move a thread to a new effective priority. If it is queued it changes
 * queue. Donation moves a thread
 * rather than the dispatch decision knowing about donation.
 */
void sched_reprioritise(struct thread *t, uint8_t prio);

/*
 * Hold off the preemption that sched_ready would otherwise take at once.
 *
 * Releasing several waiters is one operation: if the first release switched
 * away, the rest would sit queued but unreleased until this thread happened
 * to run again, and the signal that was supposed to free them all would
 * have freed only some. Between defer and undefer, sched_ready queues
 * without dispatching, and undefer takes the one decision that is owed.
 *
 * Nests, so a caller need not know whether its own caller deferred.
 */
void sched_defer(void);
void sched_undefer(void);

/*
 * Move a thread to a named core. Refuses a thread that is running,
 * because stopping a thread on another core needs an inter-processor
 * interrupt, which is not sent.
 */
sys_err_t sched_migrate(struct thread *t, uint32_t core);

/* Another core has made something runnable here and said so. */
void sched_ipi(void);

/*
 * The scheduling loop of a core that is not the first one. Does not
 * return: the core is either running something migrated to it or is
 * offering itself.
 */
void sched_secondary(void);

/*
 * How many times a core has made a higher-priority thread runnable on
 * another core and declined to preempt itself for it.
 */
extern uint64_t sched_remote_readies;

/*
 * How many scheduling decisions this core has taken. Per core: the
 * other one is dispatching the whole time, so a count for the system
 * answers what the machine did rather than what this path did.
 */
uint64_t sched_decisions(void);

#endif /* _SYS_SCHED_H_ */
