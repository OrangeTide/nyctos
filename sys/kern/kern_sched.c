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
#include <sys/intr.h>
#include <sys/sched.h>
#include <sys/systm.h>
#include <sys/cpu.h>
#include <sys/thread.h>
#include <sys/cpu.h>
#include <sys/thread.h>

#include <stdbool.h>

struct runq {
    struct thread *head;
    struct thread *tail;
};

static struct runq runq[SYS_PRIO_COUNT];
static uint32_t ready_bits;

/* Preemption held off; see sched_defer. */
/*
 * The deferral, per core. Its comment used to say one counter for the
 * system and not one per thread, because it exists to make a short run
 * of readies atomic and never spans a switch. Per system is per core
 * once there are two, and the runs it makes atomic happen on whichever
 * core is making them.
 */
#define defer_depth         (cpu_self()->cp_defer)
#define defer_pending       (cpu_self()->cp_defer_pending)

_Static_assert(SYS_PRIO_COUNT <= 32,
  "the ready bitmap is one 32-bit word; see section 11.1");

/* Take a specific thread off whatever run queue it is on. */
static void
dequeue_specific(struct thread *t)
{
    struct runq *q = &runq[t->t_prio];
    struct thread **link = &q->head;
    struct thread *prev = NULL;

    while (*link != NULL && *link != t) {
        prev = *link;
        link = &(*link)->t_qnext;
    }
    if (*link != t) {
        panic("sched: %s not on the queue it claims", t->t_name);
    }

    *link = t->t_qnext;
    if (q->tail == t) {
        q->tail = prev;
    }
    t->t_qnext = NULL;

    if (q->head == NULL) {
        ready_bits &= ~((uint32_t)1 << t->t_prio);
    }
}

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
 * Section 11.6 makes idle a state of the core rather than a thread: there
 * is no idle thread occupying a priority level and no thread id meaning
 * "nothing". A core is either running a thread or it is idle.
 *
 * Leaving idle is on the interrupt path, not the scheduling path. Waiting
 * with interrupts enabled is the whole mechanism, and it is why this is
 * reached only after they exist: before that, a core with nothing runnable
 * had nothing that could ever wake it.
 */
static void
sched_idle(void)
{
    struct thread *parked = thread_current();

    /*
     * There is no current thread while the core idles. That is section
     * 11.6 taken literally, and it is load-bearing rather than tidy: the
     * interrupt that ends the idle may ready the very thread whose
     * blocking caused it, and a thread cannot be made runnable while it is
     * still marked as the one running.
     *
     * The idling happens on that thread stack all the same, so it is put
     * back before anything switches away from it.
     *
     * Narrowing the check in sched_ready to fire only on a RUNNING thread
     * looks like the smaller fix and is not one. It leaves the pointer
     * naming a thread that is not running, so the preemption arm there
     * still compares against its priority and enqueues it, putting a
     * blocked thread on a run queue. Tried; it fails one panic later.
     */
    thread_set_current(NULL);

    for (;;) {
        md_intr_enable();
        md_idle_wait();
        (void)md_intr_disable();

        if (ready_bits != 0) {
            break;
        }
    }

    thread_set_current(parked);
}

static void
dispatch(void)
{
    struct thread *next;

    /*
     * Whatever the last switch left for collection, freed here rather
     * than in the switch itself. A thread is parked when the switch
     * leaves it, so by the time any thread reaches a scheduling
     * decision the parked one is not the stack under it. Doing it in
     * the switch missed the case where the decision is to carry on:
     * dispatch returns without switching when the best thread is
     * already running, and a yield with nothing else runnable is
     * exactly that.
     */
    thread_reap_parked();

    next = dequeue_highest();
    if (next == NULL) {
        /*
         * Idle until an interrupt makes something runnable, then take the
         * decision again. An interrupt handler may have readied several
         * threads, so the queue is consulted afresh rather than assuming
         * the first one found is the one to run.
         */
        sched_idle();
        next = dequeue_highest();
        if (next == NULL) {
            panic("sched: left idle with nothing runnable");
        }
    }

    next->t_state = THREAD_RUNNING;
    if (next == thread_current()) {
        return;
    }

    thread_switch_to(next);
}
/*
 * Everything below takes an interrupts-disabled critical section, because
 * the run queues are now reached by interrupt handlers as well as by
 * threads. Section 13.3 makes that the Tier 0 answer rather than locking,
 * which is what buys a kernel with no internal locking at all.
 *
 * The saved state is restored rather than interrupts simply re-enabled, so
 * a region nested inside another leaves it disabled as it found it.
 */
void
sched_ready(struct thread *t)
{
    struct thread *cur = thread_current();
    intr_state_t s = md_intr_disable();

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
     * priority does not preempt, because within a level the policy is FIFO
     * and the running thread got there first.
     */
    if (cur != NULL && t->t_prio > cur->t_prio) {
        if (defer_depth > 0) {
            defer_pending = true;
        } else {
            enqueue(cur);
            dispatch();
        }
    }

    md_intr_restore(s);
}

/*
 * Make a thread runnable and take no scheduling decision at all.
 *
 * This is for a caller that is about to block. Handing a message to a
 * server and ceasing to be runnable are one step from the sender's point
 * of view, and sched_ready() would split them: it dispatches as soon as
 * the readied thread outranks the caller, which is exactly the case where
 * the server runs, replies, and readies a sender that the preemption has
 * already put on a run queue and that has not reached sched_block(). The
 * second ready then panics on a thread that is already queued.
 *
 * Deferral does not solve it. sched_defer() would hold the preemption off,
 * but sched_block() refuses to run inside a deferral, and rightly: the
 * counter is one for the system, so a deferral carried across a switch
 * would leave whatever runs next not preempting when it should.
 *
 * Nothing is skipped, only postponed. The sched_block() that follows takes
 * the decision, and takes it with the caller off the run queues where a
 * blocking thread belongs. A caller that is not about to block wants
 * sched_ready().
 */
void
sched_ready_nopreempt(struct thread *t)
{
    intr_state_t s = md_intr_disable();

    if (t == thread_current()) {
        panic("thread %s: made itself runnable while running", t->t_name);
    }

    enqueue(t);

    md_intr_restore(s);
}

void
sched_defer(void)
{
    intr_state_t s = md_intr_disable();

    defer_depth++;

    md_intr_restore(s);
}

void
sched_undefer(void)
{
    struct thread *cur;
    intr_state_t s = md_intr_disable();

    if (defer_depth == 0) {
        panic("sched: undefer without a matching defer");
    }

    defer_depth--;
    if (defer_depth == 0 && defer_pending) {
        defer_pending = false;

        cur = thread_current();
        if (cur != NULL && highest_ready() > (int)cur->t_prio) {
            enqueue(cur);
            dispatch();
        }
    }

    md_intr_restore(s);
}

void
sched_yield(void)
{
    intr_state_t s = md_intr_disable();

    enqueue(thread_current());
    dispatch();

    md_intr_restore(s);
}

void
sched_block(void)
{
    struct thread *cur = thread_current();
    intr_state_t s = md_intr_disable();

    /*
     * The deferral is one counter for the system, not one per thread,
     * because it exists to make a short run of readies atomic and never
     * spans a switch. Blocking while deferred would hand the deferral to
     * whatever runs next, which would then not preempt when it should.
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

    md_intr_restore(s);
}

/*
 * Give a thread a new effective priority.
 *
 * If it is queued it moves queue, which is the whole of what section 11.2
 * means by the scheduler seeing only the donated value: there is no second
 * base-priority queue and no re-sorting pass, so the dispatch decision
 * never has to know that donation exists.
 */
void
sched_reprioritise(struct thread *t, uint8_t prio)
{
    intr_state_t s = md_intr_disable();
    bool queued = (t->t_state == THREAD_READY);

    if (queued) {
        dequeue_specific(t);
    }

    t->t_prio = prio;

    if (queued) {
        enqueue(t);
    }

    md_intr_restore(s);
}
