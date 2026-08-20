/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Recording what threads blocked on, per section 22.2.
 *
 * The two sides of a rendezvous cannot be observed by sampling, because
 * whichever thread does the sampling is running and therefore cannot see a
 * system in which something else is. States are recorded as they are
 * entered and read back afterwards.
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

#endif /* _SYS_TRACE_H_ */
