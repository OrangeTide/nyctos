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

struct runq {
    struct thread *head;
    struct thread *tail;
};

static struct runq runq[SYS_PRIO_COUNT];
static uint32_t ready_bits;

_Static_assert(SYS_PRIO_COUNT <= 32,
  "the ready bitmap is one 32-bit word; see section 11.1");

void
sched_init(void)
{
    unsigned int i;

    ready_bits = 0;
    for (i = 0; i < SYS_PRIO_COUNT; i++) {
        runq[i].head = NULL;
        runq[i].tail = NULL;
    }
}

static void
enqueue(struct thread *t)
{
    struct runq *q = &runq[t->t_prio];

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

    prio = 31u - (unsigned int)__builtin_clz(ready_bits);
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
 */
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

    enqueue(t);

    /*
     * Section 11.3: a thread becoming runnable at a higher priority takes
     * the core immediately rather than waiting for a quantum. Equal
     * priority does not preempt, because the policy within a level is FIFO
     * and the running thread got there first.
     */
    if (cur != NULL && t->t_prio > cur->t_prio) {
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
    dispatch();
}
