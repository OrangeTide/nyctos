/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Time, timers and deadlines, per section 14.
 *
 * There is no tick. The hardware timer is armed one-shot for the earliest
 * thing the core knows about, which is the nearest of the next timer
 * activation and the next deadline being waited on. A core with nothing
 * pending arms nothing at all, which is what makes an idle core genuinely
 * idle rather than woken to be told nothing happened.
 *
 * Everything here is monotonic uptime. Calendar time is an offset a
 * userspace service writes and nothing in the kernel depends on, so a
 * clock correction of any size cannot make a timer miss an activation or
 * a wait return early. That is structural rather than defended against.
 */

#ifndef _SYS_TIME_H_
#define _SYS_TIME_H_

#include <sys/op.h>
#include <sys/op.h>
#include <sys/pool.h>

#include <stdbool.h>
#include <stdint.h>

struct thread;
struct timeline;

/* Absolute instants on the monotonic timebase, in hardware ticks. */
typedef uint64_t sys_time_t;

/* Named in sys/op.h, because a deadline is part of every blocking
 * operation and a program names one without knowing about clocks. */

struct timer {
    sys_id_t         tm_id;
    const char      *tm_name;
    struct timeline *tm_timeline;

    /*
     * Phase is absolute, measured from the origin and never from the last
     * delivery: activation N happens at tm_first + N * tm_period. That is
     * what makes the counter mean a time rather than a count of things
     * that happened to be delivered, and what stops delivery jitter
     * accumulating into the phase.
     */
    sys_time_t       tm_first;
    sys_time_t       tm_period;     /* zero for a one-shot */

    uint64_t         tm_count;      /* activations signalled so far */
    bool             tm_active;
    struct timer    *tm_next;       /* pending list, earliest first */
};

void time_init(void);

/* Now, on the monotonic timebase. */
sys_time_t time_now(void);
uint64_t time_hz(void);
sys_time_t time_from_ms(uint64_t ms);

/*
 * A one-shot advances its timeline once at tm_first. A periodic advances
 * it to the activation count the current time implies, which after a stall
 * is one update rather than a backlog to drain.
 */
struct timer *timer_create(const char *name, struct timeline *tl,
  sys_time_t first, sys_time_t period);
void timer_destroy(struct timer *tm);

/*
 * Register the running thread to be woken at deadline if it is still
 * blocked. Paired with time_sleep_cancel, which a normal wake calls so a
 * thread that was released early is not woken twice.
 */
void time_sleep_until(struct thread *t, sys_time_t deadline);
void time_sleep_cancel(struct thread *t);

/* The timer interrupt. */
void time_interrupt(void);

#if SYS_TIER == 2
/*
 * Another core changed what is pending and asked this one to arm for
 * it. Called where an inter-processor interrupt arrives, because that
 * is the poke the other core sent to say so.
 */
void time_rearm_if_asked(void);
#else
#define time_rearm_if_asked()   ((void)0)
#endif

#if SYS_TIER == 2
/*
 * Another core changed what is pending and asked this one to arm for
 * it. Called where an inter-processor interrupt arrives.
 */
void time_rearm_if_asked(void);
#else
#define time_rearm_if_asked()   ((void)0)
#endif

#endif /* _SYS_TIME_H_ */
