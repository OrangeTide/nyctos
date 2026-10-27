/* trace.h : recording of thread wait states.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares struct trace_event and the trace_* functions that enable and reset
 * the recorder, log the wait state each thread enters, and report or query what
 * was recorded. Included by the timeline, channel and trace sources and by
 * sys/kern/init_main.c.
 *
 */

#ifndef _SYS_TRACE_H_
#define _SYS_TRACE_H_

#include <sys/thread.h>

#include <stdbool.h>
#include <stdint.h>

struct trace_event {
    const char *tr_thread;
    uint8_t     tr_wait;        /* enum thread_wait */
    uint8_t     tr_prio;
};

void trace_enable(bool on);
void trace_reset(void);
void trace_wait(const struct thread *t, enum thread_wait wait);
void trace_report(const char *what);
bool trace_saw(enum thread_wait wait);

/* The same, about one thread by name. See the implementation. */
bool trace_saw_thread(const char *name, enum thread_wait wait);

#endif /* _SYS_TRACE_H_ */
