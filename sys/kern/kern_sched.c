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
#include <sys/cpu.h>
#include <sys/intr.h>
#include <sys/lock.h>
#include <sys/sched.h>
#include <sys/systm.h>
#include <sys/thread.h>

#include <stdbool.h>

/*
 * The queues and the bitmap belong to the core reading them, per
 * section 11.4. They were one set for the system, which was the same
 * thing while there was one core and is not now: a core dispatching
 * from a queue another core is enqueueing to would be taking the
 * decision jointly, and a jointly taken decision needs a lock.
 */
#define my_runq             (cpu_self()->cp_runq)
#define ready_bits          (cpu_self()->cp_ready)

/*
 * How many times this core has readied a thread that outranks what it
 * is running and belongs to another core. Preempting for one of those
 * would stop this core for a thread that is not going to run on it, so
 * the count exists to show the case happens rather than to be believed
 * about.
 */
uint64_t sched_remote_readies;


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


/*
 * Link a thread onto this core's queue for its level. No state is
 * decided here: a thread arriving from another core was already made
 * ready by the core that sent it, and this is the moment it stops
 * being on its way and starts being runnable somewhere.
 */
static void
queue_link(struct thread *t)
{
    struct runq *q = &my_runq[t->t_prio];

    t->t_qnext = NULL;
    if (q->rq_tail == NULL) {
        q->rq_head = t;
    } else {
        q->rq_tail->t_qnext = t;
    }
    q->rq_tail = t;

    ready_bits |= (uint32_t)1 << t->t_prio;

    /*
     * Which cores have ever had this thread on a queue. A thread that
     * was put on the other core is supposed never to have been on this
     * one, and that is a claim about history rather than about now.
     */
    t->t_linked_on |= (uint32_t)1 << cpu_self()->cp_index;
}

/*
 * A thread this core does not own, left where the core that owns it
 * will find it. One compare and exchange and no lock, because a lock
 * here is a lock the other core would have to take to dispatch, and
 * section 11.4 is precisely that it should not have to.
 */
static void
push_arriving(struct thread *t)
{
    struct cpu *target = cpu_at(t->t_core);
    struct thread *head;

    for (;;) {
        head = __atomic_load_n(&target->cp_incoming, __ATOMIC_RELAXED);
        t->t_inext = head;
        if (__atomic_compare_exchange_n(&target->cp_incoming, &head, t,
              false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            /*
             * And told, per section 11.4. Leaving it on the list is
             * leaving it until that core happens to decide again for
             * some other reason, and a donation that arrives when the
             * core next feels like it is a donation that did not work.
             */
            md_cpu_ipi(t->t_core);
            return;
        }
    }
}

/*
 * Everything that has arrived since the last look, put on the queues.
 * One exchange takes the whole list, so a core sending while this runs
 * simply starts a new one and nothing waits for anything.
 *
 * The order threads arrived in is not preserved and is not a scheduling
 * decision. Each one goes on the queue its priority names, and section
 * 11.2 is where the order comes from.
 */
static void
take_arriving(void)
{
    struct thread *list;

    list = __atomic_exchange_n(&cpu_self()->cp_incoming,
      (struct thread *)NULL, __ATOMIC_ACQUIRE);

    while (list != NULL) {
        struct thread *t = list;

        list = t->t_inext;
        t->t_inext = NULL;
        queue_link(t);
    }
}
/* Take a specific thread off whatever run queue it is on. */
static void
dequeue_specific(struct thread *t)
{
    struct runq *q;

    /*
     * This core's queues, because those are the only ones it may
     * touch. A thread queued on another core is reached by asking that
     * core, which is what the stage after this one builds.
     */
    if (t->t_core != cpu_self()->cp_index) {
        panic("sched: %s is queued on core %u and this is core %u",
          t->t_name, (unsigned int)t->t_core,
          (unsigned int)cpu_self()->cp_index);
    }

    q = &my_runq[t->t_prio];
    struct thread **link = &q->rq_head;
    struct thread *prev = NULL;

    while (*link != NULL && *link != t) {
        prev = *link;
        link = &(*link)->t_qnext;
    }
    if (*link != t) {
        panic("sched: %s not on the queue it claims", t->t_name);
    }

    *link = t->t_qnext;
    if (q->rq_tail == t) {
        q->rq_tail = prev;
    }
    t->t_qnext = NULL;

    if (q->rq_head == NULL) {
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
        my_runq[i].rq_head = NULL;
        my_runq[i].rq_tail = NULL;
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

    t->t_state = THREAD_READY;

    if (cpu_at(t->t_core) == cpu_self()) {
        queue_link(t);
        return;
    }

    push_arriving(t);
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

    /*
     * What another core sent here is looked at exactly once per
     * decision, and before the decision. A thread that arrived after
     * this waits for the next one, which is the same thing that
     * happens to a thread readied a moment after a dispatch.
     */
    take_arriving();

    if (ready_bits == 0) {
        return NULL;
    }

    prio = (unsigned int)highest_ready();
    q = &my_runq[prio];

    t = q->rq_head;
    q->rq_head = t->t_qnext;
    if (q->rq_head == NULL) {
        q->rq_tail = NULL;
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
    bool had;

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

    /*
     * The kernel lock goes down here and nowhere else in the kernel.
     * This is the one place that waits for an interrupt on purpose,
     * and the interrupt that ends the wait has to get into the kernel
     * to deliver itself. A core asleep with the lock held is a core
     * holding the door shut against the only thing that would wake it.
     */
    cpu_self()->cp_idle++;

    had = klock_drop();

    for (;;) {
        md_intr_enable();
        md_idle_wait();
        (void)md_intr_disable();

        /*
         * What another core sent here counts as something to do. The
         * bitmap alone does not know about it: a thread pushed to this
         * core while it was asleep is on the arriving list and on no
         * queue, so a core testing only the bitmap goes back to sleep
         * with work waiting for it.
         */
        take_arriving();

        if (ready_bits != 0) {
            break;
        }
    }

    klock_regain(had);

    thread_set_current(parked);
}

static void
dispatch(void)
{
    struct thread *next;
    uint64_t took;

    cpu_self()->cp_dispatches++;

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

    /*
     * The decision itself, with a count around it of every lock taken
     * that another core could be holding. Section 11.4 requires that
     * number to be zero: a core waiting on a lock to find out what to
     * run next is a core waiting on a decision another core is making,
     * and the queues are per core precisely so that it does not.
     *
     * The bracket covers the choosing and not the switching. Switching
     * away is where this thread stops running, and what the thread
     * that runs next does with locks is its own business.
     */
    took = spin_take_count();

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

        /*
         * Sampled again after idling, because idling is where this
         * core gives the lock up and takes it back, and that is not
         * part of the decision.
         */
        took = spin_take_count();
    }

    if (spin_take_count() != took) {
        panic("sched: the decision took %lu locks another core could hold",
          (unsigned long)(spin_take_count() - took));
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
     *
     * The core it belongs to, and not simply its priority. A thread made
     * runnable on the other core is nothing to do with what runs here:
     * preempting for it would stop this core for a thread that is not
     * going to run on it. Section 11.4 makes preemption a per-core
     * question because the queue it competes for is a per-core queue.
     */
    if (cur != NULL && t->t_prio > cur->t_prio &&
      t->t_core != cpu_self()->cp_index) {
        sched_remote_readies++;
    }
    if (cur != NULL && t->t_prio > cur->t_prio &&
      t->t_core == cpu_self()->cp_index) {
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

/*
 * What a core other than the first one does with itself.
 *
 * It is a thread, at the lowest priority there is, that offers the core
 * back at every turn. Anything migrated here outranks it and runs; when
 * that thread blocks or ends, this is what is left and the core comes
 * back here.
 *
 * It spins rather than waiting for an interrupt, which is temporary and
 * is the shape of the hole the next stage fills. A core waiting on
 * md_cpu_wait is woken by an interrupt, and the interrupt that would say
 * "something arrived for you" is the inter-processor one that section
 * 11.4 has donation use and that this stage does not have yet. Until
 * then a core with nothing to do looks at its arriving list rather than
 * sleeping, and the count it reports says how many times it looked.
 */
void
sched_secondary(void)
{
    /*
     * It leaves the run set and nothing ever makes it runnable again,
     * so the core drops into the idle wait and stays there until
     * something arrives. That is section 11.6 exactly: idle is a state
     * of the core, and this context exists only to be the thing the
     * core dispatches away from.
     *
     * It span, before there was an inter-processor interrupt to wake
     * it. What it cost was a core at full tilt looking at a list.
     */
    for (;;) {
        sched_block();
    }
}

/*
 * Move a thread to a named core, per section 11.4.
 *
 * Explicit, and the only way anything moves. Nothing balances, nothing
 * steals, and no thread drifts: where a thread runs is a fact about the
 * program that placed it, which is the property that makes a per-core
 * analysis of timing mean anything.
 *
 * A running thread cannot be moved from under itself, and a thread
 * running on another core cannot be moved at all yet: doing that means
 * telling that core to stop running it, which is the inter-processor
 * interrupt the next stage builds.
 */
sys_err_t
sched_migrate(struct thread *t, uint32_t core)
{
    intr_state_t s;
    bool queued;

    if (core >= cpu_count()) {
        return SYS_ERR_INVALID_ARG;
    }

    s = md_intr_disable();

    if (t->t_core == core) {
        md_intr_restore(s);
        return SYS_OK;
    }
    if (t == thread_current() || t->t_state == THREAD_RUNNING) {
        md_intr_restore(s);
        return SYS_ERR_WOULDBLOCK;
    }
    if (t->t_state == THREAD_DEAD) {
        md_intr_restore(s);
        return SYS_ERR_STALE;
    }

    /*
     * Taken off the queue it is on before its core changes, because the
     * queue it is on is named by the core it is on. Doing it the other
     * way round looks for it on the queues of a core it has not been on.
     */
    queued = t->t_state == THREAD_READY;
    if (queued) {
        if (t->t_core != cpu_self()->cp_index) {
            md_intr_restore(s);
            return SYS_ERR_WOULDBLOCK;
        }
        dequeue_specific(t);
        t->t_state = THREAD_BLOCKED;
    }

    t->t_core = core;

    if (queued) {
        enqueue(t);
    }

    md_intr_restore(s);

    return SYS_OK;
}

uint64_t
sched_decisions(void)
{
    return cpu_self()->cp_dispatches;
}

/*
 * Another core has put something here and said so.
 *
 * The interrupt itself is the whole message: there is nothing to read,
 * because what arrived is on the list and the list is what is looked
 * at. What this adds over waiting is promptness, which is the entire
 * point of section 11.4 sending one.
 */
void
sched_ipi(void)
{
    struct thread *cur = thread_current();
    int best;

    take_arriving();

    /*
     * Preempt only for something that outranks what is running, which
     * is section 11.3 applied to a thread that arrived rather than to
     * one this core made runnable. A core running nothing is already on
     * its way to a decision and needs no help from here.
     */
    if (cur == NULL) {
        return;
    }

    best = highest_ready();
    if (best < 0 || (uint8_t)best <= cur->t_prio) {
        return;
    }

    if (defer_depth > 0) {
        defer_pending = true;
        return;
    }

    enqueue(cur);
    dispatch();
}
