/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Timelines. See sys/sys/timeline.h for what one is and why.
 *
 * Section 9.2 has a waiter donate its priority to the holder of a
 * signal-righted handle. That is not implemented here, and cannot be
 * usefully: donation needs the priority chain that arrives with IPC, and
 * the design does not say which holder receives it when more than one
 * exists. Noted where it would go.
 */

#include <sys/intr.h>
#include <sys/object.h>
#include <sys/sched.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/time.h>
#include <sys/timeline.h>

struct timeline *
timeline_create(const char *name)
{
    struct timeline *tl;

    tl = pool_alloc(timeline_pool(), NULL);
    if (tl == NULL) {
        return NULL;
    }

    tl->tl_id = pool_id_of(timeline_pool(), tl);
    tl->tl_name = name;
    tl->tl_value = 0;
    tl->tl_waiters = NULL;
    tl->tl_signalers = 0;
    tl->tl_error = false;

    return tl;
}

/* Insert into the waiter list, which is ordered by the value awaited. */
static void
wait_insert(struct timeline *tl, struct thread *t)
{
    struct thread **link = &tl->tl_waiters;

    while (*link != NULL && (*link)->t_waitval <= t->t_waitval) {
        link = &(*link)->t_wnext;
    }

    t->t_wnext = *link;
    *link = t;
}

/*
 * Release one waiter. The caller is between sched_defer and sched_undefer,
 * so this queues without switching and every waiter a signal frees is freed
 * before any of them runs.
 */
static void
wait_release(struct thread *t, sys_err_t res)
{
    t->t_wnext = NULL;
    t->t_blocked_on = NULL;
    t->t_waitres = res;
    sched_ready(t);
}

/*
 * Take a thread off the waiter list without releasing it, for a wake that
 * came from somewhere other than a signal. Returns whether it was there.
 */
bool
timeline_unwait(struct timeline *tl, struct thread *t)
{
    struct thread **link = &tl->tl_waiters;

    while (*link != NULL && *link != t) {
        link = &(*link)->t_wnext;
    }
    if (*link != t) {
        return false;
    }

    *link = t->t_wnext;
    t->t_wnext = NULL;
    t->t_blocked_on = NULL;

    return true;
}

static void
release_all(struct timeline *tl, sys_err_t res)
{
    struct thread *t;

    while (tl->tl_waiters != NULL) {
        t = tl->tl_waiters;
        tl->tl_waiters = t->t_wnext;
        wait_release(t, res);
    }
}

sys_err_t
timeline_wait(struct timeline *tl, uint64_t value, uint64_t deadline)
{
    struct thread *self = thread_current();
    intr_state_t s = md_intr_disable();

    /*
     * A value that was reached stays reached, so this is checked before the
     * error state and not after. Section 9.3 says the error state satisfies
     * every outstanding and future wait with an error, but taking that over
     * an already-reached value would contradict the monotonicity that makes
     * a late waiter safe, which is the more load-bearing of the two.
     */
    if (tl->tl_value >= value) {
        md_intr_restore(s);
        return SYS_OK;
    }
    if (tl->tl_error) {
        md_intr_restore(s);
        return SYS_ERR_OWNER_DEAD;
    }

    /*
     * Section 14.3: deadlines are absolute. A poll is a deadline that has
     * already passed, so it needs no separate path.
     */
    if (deadline <= time_now()) {
        md_intr_restore(s);
        return SYS_ERR_TIMEOUT;
    }

    self->t_waitval = value;
    self->t_waitres = SYS_ERR_INTERRUPTED;   /* so a missing wake is loud */
    self->t_blocked_on = tl;
    wait_insert(tl, self);
    if (deadline != SYS_DEADLINE_NEVER) {
        time_sleep_until(self, deadline);
    }

    /* Section 9.2 donation would happen here. */

    /*
     * Interrupts come back on inside the switch, when whatever runs next
     * restores its own state. They are off again by the time this returns,
     * because the thread that woke this one did so from a critical section
     * of its own.
     */
    sched_block();

    /*
     * Whichever of the two happened first, the other is now stale: a
     * signal leaves a deadline registered, and a timeout leaves the thread
     * on the waiter list. Both are withdrawn here rather than by whoever
     * did the waking, so neither has to know about the other.
     */
    time_sleep_cancel(self);
    (void)timeline_unwait(tl, self);

    md_intr_restore(s);

    if (self->t_blocked_on != NULL) {
        panic("timeline %s: woke a thread still marked blocked", tl->tl_name);
    }

    return self->t_waitres;
}

sys_err_t
timeline_signal(struct timeline *tl, uint64_t value)
{
    struct thread *t;
    intr_state_t s = md_intr_disable();

    if (tl->tl_error) {
        md_intr_restore(s);
        return SYS_ERR_OWNER_DEAD;
    }

    /*
     * Section 9.1: signalling a value at or below the current one is an
     * error. Monotonicity is the whole basis of a late waiter being safe,
     * so going backwards is refused rather than clamped.
     */
    if (value <= tl->tl_value) {
        md_intr_restore(s);
        return SYS_ERR_INVALID_ARG;
    }

    tl->tl_value = value;

    sched_defer();
    while (tl->tl_waiters != NULL && tl->tl_waiters->t_waitval <= value) {
        t = tl->tl_waiters;
        tl->tl_waiters = t->t_wnext;
        wait_release(t, SYS_OK);
    }
    sched_undefer();

    md_intr_restore(s);

    return SYS_OK;
}

void
timeline_add_signaler(struct timeline *tl)
{
    tl->tl_signalers++;
}

/*
 * Section 9.3 rule 2. Losing the last signal right means no one can advance
 * this timeline again, so every waiter blocked on a value it has not
 * reached is released with an error, and the error is kept so that a waiter
 * arriving later is failed just as promptly.
 */
void
timeline_drop_signaler(struct timeline *tl)
{
    intr_state_t s = md_intr_disable();

    if (tl->tl_signalers == 0) {
        panic("timeline %s: dropped a signal right it never had", tl->tl_name);
    }

    tl->tl_signalers--;
    if (tl->tl_signalers > 0 || tl->tl_error) {
        md_intr_restore(s);
        return;
    }

    tl->tl_error = true;

    sched_defer();
    release_all(tl, SYS_ERR_OWNER_DEAD);
    sched_undefer();

    md_intr_restore(s);
}
