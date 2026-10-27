/* time.h : monotonic time, timers and deadline sleeps.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares sys_time_t on the monotonic timebase, struct timer, and the time_*
 * and timer_* functions that read the clock, create and destroy timers, and
 * sleep a thread until a deadline. Included by sys/object.h and the kernel
 * time, timeline, process, scheduler and operation sources.
 *
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

/*
 * Build the system page, once at boot, and the frame it
 * lives in. See sys/syspage.h.
 */
void syspage_build(void);
uintptr_t syspage_frame(void);
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
