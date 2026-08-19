/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Timelines: the system's single synchronization object, per section 9.
 *
 * A timeline is a monotonically increasing 64-bit counter, and a waiter
 * blocks until it reaches or passes a named value. Driver completion,
 * interrupt delivery, timer expiry and thread exit are all meant to be
 * advances of one of these, so that a thread blocks on one kind of object
 * whatever it is waiting for.
 *
 * Monotonicity is what makes a late waiter safe: a value already passed
 * satisfies a wait immediately, so there is no window in which a waiter can
 * miss a value and no registration protocol to get wrong. Signalling a
 * value at or below the current one is therefore an error.
 *
 * The error state exists so that the disappearance of the only party that
 * could satisfy a wait is delivered as an event rather than left to a
 * timeout. It is a property the timeline keeps, not an event that has to be
 * observed, so a waiter arriving afterwards is failed just as promptly as
 * one already blocked.
 */

#ifndef _SYS_TIMELINE_H_
#define _SYS_TIMELINE_H_

#include <sys/op.h>
#include <sys/pool.h>

#include <stdbool.h>
#include <stdint.h>

struct thread;

struct timeline {
    sys_id_t        tl_id;
    const char     *tl_name;

    uint64_t        tl_value;

    /*
     * Waiters, ordered by the value each is waiting for. Signalling walks
     * from the head and releases while the head's value has been reached,
     * which is the whole operation.
     */
    struct thread  *tl_waiters;

    /*
     * Live handles carrying SYS_RIGHT_SIGNAL. When the last one goes, no
     * one can advance this timeline again, which is section 9.3 rule 2.
     */
    uint32_t        tl_signalers;

    bool            tl_error;
};

struct timeline *timeline_create(const char *name);

/*
 * Block until the counter reaches value. Returns immediately if it already
 * has, whatever else has happened to the timeline, because a value that was
 * reached stays reached.
 */
sys_err_t timeline_wait(struct timeline *tl, uint64_t value);

/* Advance to value, releasing every waiter that value satisfies. */
sys_err_t timeline_signal(struct timeline *tl, uint64_t value);

/*
 * Called when a handle carrying SYS_RIGHT_SIGNAL is created or destroyed.
 * Losing the last one puts the timeline into the error state.
 */
void timeline_add_signaler(struct timeline *tl);
void timeline_drop_signaler(struct timeline *tl);

#endif /* _SYS_TIMELINE_H_ */
