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

    /*
     * The object this timeline is embedded in, if it is embedded in
     * one. A completion timeline lives inside its thread or process
     * per section 8, so a handle naming it is a pointer into that
     * object, and reaping the object under it would leave the handle
     * naming a slot somebody else now has.
     *
     * SYS_OBJ_NONE, which is zero, for a timeline drawn from the pool:
     * it is its own object and needs none of this. The type is a bare
     * word rather than the enum because object.h reaches back here.
     */
    uint32_t        tl_owner_type;
    void           *tl_owner;
};

/*
 * Prepare a timeline that is part of another object rather than an entry
 * in the pool, per section 8: a completion timeline belongs to its thread
 * or process, so obtaining it cannot fail for want of a slot. It carries
 * no identifier, because nothing looks one of these up by id; it is
 * reached through the object that owns it.
 */
void timeline_init(struct timeline *tl, const char *name);

/*
 * Say which object this embedded timeline lives inside, so that a
 * handle naming it holds that object rather than pointing into a slot
 * that can be reused underneath it.
 */
void timeline_embed(struct timeline *tl, uint32_t type, void *owner);

/* What it lives inside, or SYS_OBJ_NONE. */
uint32_t timeline_owner(const struct timeline *tl, void **out);

struct timeline *timeline_create(const char *name);

/*
 * Block until the counter reaches value. Returns immediately if it already
 * has, whatever else has happened to the timeline, because a value that was
 * reached stays reached.
 */
sys_err_t timeline_wait(struct timeline *tl, uint64_t value,
  uint64_t deadline);

/* Take a waiter off the list, for a wake that did not come from a signal. */
bool timeline_unwait(struct timeline *tl, struct thread *t);

/* The same, shaped for struct thread t_unwait. */
void timeline_unwait_thread(void *obj, struct thread *t);

/* Advance to value, releasing every waiter that value satisfies. */
sys_err_t timeline_signal(struct timeline *tl, uint64_t value);

/*
 * Called when a handle carrying SYS_RIGHT_SIGNAL is created or destroyed.
 * Losing the last one puts the timeline into the error state.
 */
void timeline_add_signaler(struct timeline *tl);
void timeline_drop_signaler(struct timeline *tl);

#endif /* _SYS_TIMELINE_H_ */
