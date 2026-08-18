/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The kernel object types, and the pools they are allocated from.
 *
 * Each structure below is deliberately thin. The milestone that gives a
 * type its behaviour is the milestone that gives it fields, and the stage
 * is named on each so that the growth is expected rather than surprising.
 * What exists now is the allocation and naming machinery every later stage
 * depends on.
 *
 * Objects owned by another object do not appear here and do not draw from a
 * pool. A thread's completion timeline is part of the thread and a channel's
 * pulse queue is part of the channel, so that the accounting is exact and
 * the calls that produce them cannot fail. See design.md section 8.
 */

#ifndef _SYS_OBJECT_H_
#define _SYS_OBJECT_H_

#include <sys/pool.h>

/* Stage 9: address space, handle table, threads. */
struct proc {
    sys_id_t p_id;
};

/* Stage 2: register context, kernel stack, scheduler links. */
struct thread {
    sys_id_t t_id;
};

/* Stage 8: receive queue, owner, pulse queue. */
struct channel {
    sys_id_t c_id;
};

/* Milestone 4: the backing region and the tier's mapping state. */
struct grant {
    sys_id_t g_id;
};

/* Stage 5: the monotonic counter and its waiter list. */
struct timeline {
    sys_id_t tl_id;
};

/* Stage 7: deadline, period, and the timeline it advances. */
struct timer {
    sys_id_t tm_id;
};

/* Milestone 9: a validated entry array, executed many times. */
struct program {
    sys_id_t pg_id;
};

/*
 * The pools themselves. Exposed so that later stages can allocate from them
 * directly; the accessors exist because the instances are file-static.
 */
struct pool *proc_pool(void);
struct pool *thread_pool(void);
struct pool *channel_pool(void);
struct pool *grant_pool(void);
struct pool *timeline_pool(void);
struct pool *timer_pool(void);
struct pool *program_pool(void);

void object_init(void);
void object_report(void);
void object_check(void);

#endif /* _SYS_OBJECT_H_ */
