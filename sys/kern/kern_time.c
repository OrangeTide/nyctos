/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Timers and deadlines. See sys/sys/time.h for the shape and section 14
 * for the reasoning.
 */

#include <sys/lock.h>
#include <sys/cpu.h>
#include <sys/intr.h>
#include <sys/object.h>
#include <sys/sched.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/time.h>
#include <sys/timeline.h>

/*
 * The lists below are one set for the machine and are reached from
 * every core: a thread on any core can arm a timer, destroy one or
 * sleep, and the core that owns the hardware walks them when it fires.
 *
 * Section 11.4 forbids a global scheduler lock and this is not one.
 * Nothing under it decides what runs. What it does hold together is a
 * pair of ordered lists, which is the same argument the pools got, and
 * this stage locked those while leaving these because they are counted
 * by nothing.
 *
 * Held across the walk and put down before anything can switch away.
 * The deferral in time_interrupt is what makes that possible: a waiter
 * released mid-pass is queued rather than dispatched, so nothing leaves
 * this file holding the lock.
 */
#if SYS_TIER == 2

static struct spinlock time_lock = { 0, 0, "timers" };

static intr_state_t
time_enter(void)
{
    intr_state_t s = md_intr_disable();

    spin_lock(&time_lock);

    return s;
}

static void
time_leave(intr_state_t s)
{
    spin_unlock(&time_lock);
    md_intr_restore(s);
}

#else

static intr_state_t
time_enter(void)
{
    return md_intr_disable();
}

static void
time_leave(intr_state_t s)
{
    md_intr_restore(s);
}

#endif /* SYS_TIER == 2 */

/* Pending timers and sleeping threads, each ordered earliest first. */
static struct timer *timers;
static struct thread *sleepers;

sys_time_t
time_now(void)
{
    return md_time_now();
}

uint64_t
time_hz(void)
{
    return md_time_hz();
}

sys_time_t
time_from_ms(uint64_t ms)
{
    return (md_time_hz() * ms) / 1000ULL;
}

/* When a timer next wants to fire, which for a spent one-shot is never. */
static sys_time_t
timer_deadline(const struct timer *tm)
{
    if (!tm->tm_active) {
        return SYS_DEADLINE_NEVER;
    }

    return tm->tm_first + ((tm->tm_count + 1) * tm->tm_period);
}

static sys_time_t
next_event(void)
{
    sys_time_t when = SYS_DEADLINE_NEVER;

    if (timers != NULL) {
        when = timer_deadline(timers);
    }
    if (sleepers != NULL && sleepers->t_deadline < when) {
        when = sleepers->t_deadline;
    }

    return when;
}

/*
 * The core whose hardware is armed for the whole machine.
 *
 * There is one list of pending events and the hardware is per core, so
 * arming has to name a core. It names this one, and a core that changes
 * the list and is not this one asks it to arm rather than arming
 * itself: a timer armed on the wrong core still fires, and what it
 * interrupts is a core that has nothing to do with the deadline.
 *
 * That is not a small difference. It was found as an interrupt check
 * failing about one run in ten, because the event the check was
 * counting arrived on the idle core, where it is an interrupt taken in
 * the kernel rather than one taken from a program.
 */
#define TIMER_CORE  0

static void
arm_here(void)
{
    sys_time_t when = next_event();

    if (when == SYS_DEADLINE_NEVER) {
        md_timer_disarm();
    } else {
        md_timer_at(when);
    }
}

/*
 * Arm the hardware for the earliest pending event, or for nothing. This is
 * the whole of section 14.1: every change to what is pending comes back
 * here, and a core with nothing pending leaves the timer disarmed.
 */
static void
reprogram(void)
{
#if SYS_TIER == 2
    if (cpu_self()->cp_index != TIMER_CORE) {
        struct cpu *owner = cpu_at(TIMER_CORE);

        /*
         * Asked, not done. The list has already changed, so what that
         * core has to be told is only that it changed, and the poke
         * this milestone already has is what tells it.
         */
        __atomic_store_n(&owner->cp_rearm, 1u, __ATOMIC_RELEASE);
        md_cpu_ipi(TIMER_CORE);
        return;
    }
#endif

    arm_here();
}

#if SYS_TIER == 2
void
time_rearm_if_asked(void)
{
    intr_state_t s;

    if (__atomic_exchange_n(&cpu_self()->cp_rearm, 0u,
          __ATOMIC_ACQUIRE) == 0) {
        return;
    }

    s = time_enter();
    arm_here();
    time_leave(s);
}
#endif

static void
timer_link(struct timer *tm)
{
    struct timer **link = &timers;

    while (*link != NULL && timer_deadline(*link) <= timer_deadline(tm)) {
        link = &(*link)->tm_next;
    }

    tm->tm_next = *link;
    *link = tm;
}

static void
timer_unlink(struct timer *tm)
{
    struct timer **link = &timers;

    while (*link != NULL && *link != tm) {
        link = &(*link)->tm_next;
    }
    if (*link == tm) {
        *link = tm->tm_next;
    }
    tm->tm_next = NULL;
}

void
time_init(void)
{
    timers = NULL;
    sleepers = NULL;
}

struct timer *
timer_create(const char *name, struct timeline *tl, sys_time_t first,
  sys_time_t period)
{
    struct timer *tm;
    intr_state_t s;

    tm = pool_alloc(timer_pool(), NULL);
    if (tm == NULL) {
        return NULL;
    }

    tm->tm_id = pool_id_of(timer_pool(), tm);
    tm->tm_name = name;
    tm->tm_timeline = tl;
    tm->tm_first = first;
    tm->tm_period = period;
    tm->tm_count = 0;
    tm->tm_active = true;
    tm->tm_next = NULL;

    /*
     * The timer holds a signal right for as long as it exists, which is
     * what section 14.2 relies on: destroying it drops the last one and
     * the timeline enters the error state under section 9.3 rule 2,
     * releasing anyone waiting for an activation that will never come.
     */
    timeline_add_signaler(tl);

    s = time_enter();
    timer_link(tm);
    reprogram();
    time_leave(s);

    return tm;
}

/*
 * Section 14.2: destroying a timer closes the kernel's signal right, which
 * force-signals the timeline in error state under section 9.3 rule 2. No
 * new rule is needed, which is the point of having only one.
 */
void
timer_destroy(struct timer *tm)
{
    intr_state_t s = time_enter();

    if (tm->tm_active) {
        tm->tm_active = false;
        timer_unlink(tm);
        reprogram();
    }
    time_leave(s);

    timeline_drop_signaler(tm->tm_timeline);
    (void)pool_free(timer_pool(), tm->tm_id);
}

void
time_sleep_until(struct thread *t, sys_time_t deadline)
{
    struct thread **link = &sleepers;

    t->t_deadline = deadline;

    while (*link != NULL && (*link)->t_deadline <= deadline) {
        link = &(*link)->t_dnext;
    }
    t->t_dnext = *link;
    *link = t;

    reprogram();
}

void
time_sleep_cancel(struct thread *t)
{
    struct thread **link = &sleepers;

    while (*link != NULL && *link != t) {
        link = &(*link)->t_dnext;
    }
    if (*link == t) {
        *link = t->t_dnext;
        t->t_dnext = NULL;
        reprogram();
    }
}

/*
 * Advance one timer to where the clock says it should be.
 *
 * The counter is set to the activation number the current time implies,
 * in one update. After a stall there is no backlog to deliver and no queue
 * to drain: a task that wanted activation N+1 and finds N+3 has missed two
 * and knows it, which is the overrun detection section 14.2 gets for free.
 */
static void
timer_fire(struct timer *tm, sys_time_t now)
{
    uint64_t due;

    if (tm->tm_period == 0) {
        tm->tm_count = 1;
        tm->tm_active = false;
        timer_unlink(tm);
    } else {
        /*
         * Activation N happens at first + N * period, so the number due
         * by now is the number of whole periods since the origin. The
         * obvious off-by-one here reports the first firing as activation
         * two, which pushes the counter permanently ahead of the clock
         * and makes every later wait return before its phase.
         */
        due = (uint64_t)((now - tm->tm_first) / tm->tm_period);

        /*
         * The caller only reaches here for a timer whose deadline has
         * passed, which means at least one more activation is due than has
         * been signalled. Returning without moving the timer would leave
         * it at the head of a list the caller loops over while its
         * deadline is past, which hangs rather than fails.
         */
        if (due <= tm->tm_count) {
            panic("timer %s: fired with nothing due", tm->tm_name);
        }
        tm->tm_count = due;

        timer_unlink(tm);
        timer_link(tm);
    }

    (void)timeline_signal(tm->tm_timeline, tm->tm_count);
}

void
time_interrupt(void)
{
    struct thread *t;
    intr_state_t s;
    sys_time_t now;

    /*
     * Deferred across the whole pass so that a timer whose waiter outranks
     * the interrupted thread does not switch away before the remaining
     * timers have been serviced and the hardware re-armed.
     */
    sched_defer();

    /*
     * And the lists held, which they were not. This walks and rewrites
     * both of them, and another core arming or destroying a timer
     * rewrites the same links. The deferral above is what lets the
     * lock be held across the whole pass: a waiter released here is
     * queued rather than dispatched, so nothing switches away while
     * this core holds it.
     */
    s = time_enter();

    now = md_time_now();

    while (timers != NULL && timer_deadline(timers) <= now) {
        timer_fire(timers, now);
    }

    while (sleepers != NULL && sleepers->t_deadline <= now) {
        t = sleepers;
        sleepers = t->t_dnext;
        t->t_dnext = NULL;
        thread_wake_timeout(t);
    }

    reprogram();

    time_leave(s);

    sched_undefer();
}
