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
#include <sys/lock.h>
#include <sys/object.h>
#include <sys/sched.h>
#include <sys/systm.h>
#include <sys/trace.h>
#include <sys/thread.h>
#include <sys/time.h>
#include <sys/timeline.h>

void
timeline_init(struct timeline *tl, const char *name)
{
    tl->tl_id = SYS_ID_INVALID;
    tl->tl_name = name;
    tl->tl_value = 0;
    tl->tl_waiters = NULL;
    tl->tl_signalers = 0;
    tl->tl_error = false;
    tl->tl_owner_type = 0;      /* SYS_OBJ_NONE, without the include */
    tl->tl_owner = NULL;
}

struct timeline *
timeline_create(const char *name)
{
    struct timeline *tl;

    tl = pool_alloc(timeline_pool(), NULL);
    if (tl == NULL) {
        return NULL;
    }

    timeline_init(tl, name);
    tl->tl_id = pool_id_of(timeline_pool(), tl);

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

void
timeline_unwait_thread(void *obj, struct thread *t)
{
    (void)timeline_unwait(obj, t);
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
    bool held = klock_held();
    intr_state_t k = 0;
    intr_state_t s;

    /*
     * Under the kernel lock, which an entry from user mode already
     * holds and a kernel thread does not. Everything below reads the
     * value, decides to wait on it, and joins the waiter list, and
     * the advance on another core writes the value and walks that
     * list. Without one exclusion over both halves the two interleave
     * in the order that leaves this thread on a list nobody is going
     * to look at again, and the deadline is the only thing that ends
     * it. Measured in M21 stage six's review as a join that timed out
     * on a process that had already exited.
     */
    if (!held) {
        k = klock_enter();
    }
    s = md_intr_disable();

    /*
     * A value that was reached stays reached, so this is checked before the
     * error state and not after. Section 9.3 says the error state satisfies
     * every outstanding and future wait with an error, but taking that over
     * an already-reached value would contradict the monotonicity that makes
     * a late waiter safe, which is the more load-bearing of the two.
     */
    if (tl->tl_value >= value) {
        md_intr_restore(s);
        if (!held) {
            klock_exit(k);
        }
        return SYS_OK;
    }
    if (tl->tl_error) {
        md_intr_restore(s);
        if (!held) {
            klock_exit(k);
        }
        return SYS_ERR_OWNER_DEAD;
    }

    /*
     * Section 14.3: deadlines are absolute. A poll is a deadline that has
     * already passed, so it needs no separate path.
     */
    if (deadline <= time_now()) {
        md_intr_restore(s);
        if (!held) {
            klock_exit(k);
        }
        return SYS_ERR_TIMEOUT;
    }

    self->t_waitval = value;
    self->t_waitres = SYS_ERR_INTERRUPTED;   /* so a missing wake is loud */
    self->t_blocked_on = tl;
    self->t_unwait = timeline_unwait_thread;
    wait_insert(tl, self);
    if (deadline != SYS_DEADLINE_NEVER) {
        time_sleep_until(self, deadline);
    }

    /* Section 9.2 donation would happen here. */

    self->t_wait = THREAD_WAIT_FENCE;
    trace_wait(self, THREAD_WAIT_FENCE);

    /*
     * Interrupts come back on inside the switch, when whatever runs next
     * restores its own state. They are off again by the time this returns,
     * because the thread that woke this one did so from a critical section
     * of its own.
     */
    sched_block();

    self->t_wait = THREAD_WAIT_NONE;

    /*
     * Whichever of the two happened first, the other is now stale: a
     * signal leaves a deadline registered, and a timeout leaves the thread
     * on the waiter list. Both are withdrawn here rather than by whoever
     * did the waking, so neither has to know about the other.
     */
    time_sleep_cancel(self);
    (void)timeline_unwait(tl, self);

    md_intr_restore(s);
    if (!held) {
        klock_exit(k);
    }

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

/*
 * Section 17.5's last line: a batch that aborts force-signals every
 * timeline it was going to signal, in error state.
 *
 * A waiter that was going to be released by an entry which now will
 * never run has nothing to wait for, and nothing else will ever tell
 * it so. Section 9.3 rule 2 already says what a timeline nobody can
 * advance does to its waiters, and this is the same thing arrived at
 * from a different direction: the party that could have satisfied the
 * wait is gone, so the waiter is told rather than left to a timeout.
 *
 * Already in error is left alone, because the first thing to put it
 * there is what its waiters were told about and a second reason does
 * not improve on it.
 */
void
timeline_force_error(struct timeline *tl)
{
    intr_state_t s = md_intr_disable();

    if (tl->tl_error) {
        md_intr_restore(s);
        return;
    }

    tl->tl_error = true;

    sched_defer();
    release_all(tl, SYS_ERR_OWNER_DEAD);
    sched_undefer();

    md_intr_restore(s);
}

/*
 * A completion timeline is inside its thread or process, so a handle
 * naming it is a pointer into that object. Saying so here is what lets
 * the handle hold the object: without it, dropping the last process
 * handle reaps the process and leaves every completion handle pointing
 * at a slot the next spawn takes.
 *
 * Section 10.4 asks for something stronger, which is that the object be
 * reaped and the completion handle keep reporting the exit from a value
 * frozen into the holder's slot. That needs the reap to find its
 * holders, and nothing here can: a handle table is per process and
 * there is no list the other way. What is built instead keeps the
 * object until the last handle of either kind goes, which reports the
 * same exit to the same holders and costs the object's memory for
 * longer.
 */
void
timeline_embed(struct timeline *tl, uint32_t type, void *owner)
{
    tl->tl_owner_type = type;
    tl->tl_owner = owner;
}

uint32_t
timeline_owner(const struct timeline *tl, void **out)
{
    *out = tl->tl_owner;

    return tl->tl_owner_type;
}

uint64_t
timeline_value(const struct timeline *tl)
{
    return tl->tl_value;
}
