/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The real-time band of section 11.2: a bitmap of non-empty priority
 * levels, and a FIFO per level.
 *
 * Selecting the highest runnable thread is one count-leading-zeros and one
 * dereference, and does not depend on how many threads are runnable. That
 * is the property the response time figures rest on, and it is the only
 * reason to prefer this over a sorted list.
 */

#include <sys/config.h>
#include <sys/sched.h>
#include <sys/systm.h>
#include <sys/thread.h>

#include <stdbool.h>

struct runq {
    struct thread *head;
    struct thread *tail;
};

static struct runq runq[SYS_PRIO_COUNT];
static uint32_t ready_bits;

/* Preemption held off; see sched_defer. */
static unsigned int defer_depth;
static bool defer_pending;

_Static_assert(SYS_PRIO_COUNT <= 32,
  "the ready bitmap is one 32-bit word; see section 11.1");

/* The highest priority anything runnable has, or -1 if nothing is. */
static int
highest_ready(void)
{
    if (ready_bits == 0) {
        return -1;
    }

    return (int)(31u - (unsigned int)__builtin_clz(ready_bits));
}

void
sched_init(void)
{
    unsigned int i;

    ready_bits = 0;
    defer_depth = 0;
    defer_pending = false;
    for (i = 0; i < SYS_PRIO_COUNT; i++) {
        runq[i].head = NULL;
        runq[i].tail = NULL;
    }
}

/*
 * A thread may only join a run queue from a state that means it is not on
 * one. Enqueuing a thread that is already queued rewrites its link, which
 * truncates the list and silently loses every thread behind it; the symptom
 * of that surfaces far from the cause, so it is worth one comparison here.
 *
 * RUNNING is legal because a thread being preempted or yielding goes back
 * on a queue from there.
 */
static void
enqueue(struct thread *t)
{
    struct runq *q = &runq[t->t_prio];

    switch (t->t_state) {
    case THREAD_NEW:
    case THREAD_RUNNING:
    case THREAD_BLOCKED:
        break;
    case THREAD_READY:
        panic("thread %s: made runnable while already runnable", t->t_name);
    case THREAD_DEAD:
        panic("thread %s: made runnable after exiting", t->t_name);
    default:
        panic("thread %s: made runnable from an unbuilt slot", t->t_name);
    }

    t->t_qnext = NULL;
    if (q->tail == NULL) {
        q->head = t;
    } else {
        q->tail->t_qnext = t;
    }
    q->tail = t;

    ready_bits |= (uint32_t)1 << t->t_prio;
    t->t_state = THREAD_READY;
}

/*
 * The highest set bit names the level to take from. Everything below this
 * line is a plain FIFO pop; the bitmap is what keeps the search off the
 * dispatch path.
 */
static struct thread *
dequeue_highest(void)
{
    struct runq *q;
    struct thread *t;
    unsigned int prio;

    if (ready_bits == 0) {
        return NULL;
    }

    prio = (unsigned int)highest_ready();
    q = &runq[prio];

    t = q->head;
    q->head = t->t_qnext;
    if (q->head == NULL) {
        q->tail = NULL;
        ready_bits &= ~((uint32_t)1 << prio);
    }
    t->t_qnext = NULL;

    return t;
}

/*
 * Nothing is runnable.
 *
 * Section 11.6 makes idle a state of the core rather than a thread, and
 * says a core leaves it on an interrupt. There are no interrupt sources
 * yet, so nothing could ever wake this core: reaching here means every
 * thread has finished and the system is done. Stage 6 gives this a device
 * to wait for and it becomes a real idle.
 *
 * Declared noreturn so that dispatch does not have to carry a path in which
 * it continues with no thread to dispatch to.
 */
static void sched_idle(void) __attribute__((noreturn));

static void
sched_idle(void)
{
    cnputs("sched: nothing runnable\n");
    machine_halt(0);
}

static void
dispatch(void)
{
    struct thread *next;

    next = dequeue_highest();
    if (next == NULL) {
        sched_idle();
    }
    if (next == thread_current()) {
        next->t_state = THREAD_RUNNING;
        return;
    }

    next->t_state = THREAD_RUNNING;
    thread_switch_to(next);
}

void
sched_ready(struct thread *t)
{
    struct thread *cur = thread_current();

    /*
     * A thread readying itself would put a running thread on a run queue,
     * leaving it both running and waiting to run.
     */
    if (t == cur) {
        panic("thread %s: made itself runnable while running", t->t_name);
    }

    enqueue(t);

    /*
     * Section 11.3: a thread becoming runnable at a higher priority takes
     * the core immediately rather than waiting for a quantum. Equal
     * priority does not preempt, because the policy within a level is FIFO
     * and the running thread got there first.
     */
    if (cur == NULL || t->t_prio <= cur->t_prio) {
        return;
    }

    if (defer_depth > 0) {
        defer_pending = true;
        return;
    }

    enqueue(cur);
    dispatch();
}

void
sched_defer(void)
{
    defer_depth++;
}

void
sched_undefer(void)
{
    struct thread *cur;

    if (defer_depth == 0) {
        panic("sched: undefer without a matching defer");
    }

    defer_depth--;
    if (defer_depth > 0 || !defer_pending) {
        return;
    }

    defer_pending = false;

    cur = thread_current();
    if (cur != NULL && highest_ready() > (int)cur->t_prio) {
        enqueue(cur);
        dispatch();
    }
}

void
sched_yield(void)
{
    enqueue(thread_current());
    dispatch();
}

void
sched_block(void)
{
    struct thread *cur = thread_current();

    /*
     * The deferral is one global counter, not one per thread, because it
     * exists to make a short run of readies atomic and never spans a
     * switch. Blocking while deferred would hand the deferral to whatever
     * runs next, which would then not preempt when it should.
     */
    if (defer_depth != 0) {
        panic("sched: %s blocked while preemption was deferred",
          cur->t_name);
    }

    /*
     * A thread that has exited is already in its final state and must not
     * be described as merely waiting for something.
     */
    if (cur->t_state != THREAD_DEAD) {
        cur->t_state = THREAD_BLOCKED;
    }

    dispatch();
}
