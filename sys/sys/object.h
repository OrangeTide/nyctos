/* object.h : kernel object types and their pool accessors.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares struct program, the accessors that return each kernel object pool,
 * and the object_init, object_report and object_check functions. Included by
 * the kernel object, process, thread, timeline, channel, connection, grant,
 * prepared program and time sources.
 *
 */

#ifndef _SYS_OBJECT_H_
#define _SYS_OBJECT_H_

#include <sys/pool.h>
#include <sys/channel.h>
#include <sys/connection.h>
#include <sys/grant.h>
#include <sys/proc.h>
#include <sys/thread.h>
#include <sys/time.h>
#include <sys/timeline.h>

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
struct pool *connection_pool(void);
struct pool *grant_pool(void);
struct pool *timeline_pool(void);
struct pool *timer_pool(void);
struct pool *program_pool(void);
struct pool *prepared_pool(void);

void object_init(void);
void object_report(void);
void object_report_usage(void);
void object_check(void);

#endif /* _SYS_OBJECT_H_ */
