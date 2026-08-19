/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Threads: creation, exit, and the context switch itself.
 *
 * Which thread runs next is not decided here. That is the dispatch decision
 * of section 11.2 and it lives in kern_sched.c, which also owns thread
 * states: it is the only code that knows whether an outgoing thread is
 * going back on a run queue, blocking, or dead.
 */

#include <sys/config.h>
#include <sys/object.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/systm.h>
#include <sys/thread.h>

static struct thread *current;

/* Written by the linker script, at the low end of the boot stack. */
extern char _stack_bottom[];

sys_thread_id_t
thread_id(const struct thread *t)
{
    return t->t_id;
}

/*
 * Resolve a reply token. A generation that no longer matches means the
 * thread that sent the request is gone and its slot has been reused, which
 * is exactly the hazard section 12.2 says the generation closes.
 */
struct thread *
thread_by_id(sys_thread_id_t id)
{
    return pool_lookup(thread_pool(), id);
}

struct thread *
thread_current(void)
{
    return current;
}

void
thread_set_current(struct thread *t)
{
    current = t;
}

/*
 * A priority outside the range would shift the ready bit off the end of the
 * bitmap, which is undefined rather than merely wrong. Checked on both the
 * paths that set one.
 */
static void
check_prio(const char *name, unsigned int prio)
{
    if (prio > SYS_PRIO_MAX) {
        panic("thread %s: priority %u above SYS_PRIO_MAX (%u)", name, prio,
          (unsigned int)SYS_PRIO_MAX);
    }
}

static void
check_guard(const struct thread *t)
{
    if (*t->t_guard != THREAD_GUARD) {
        panic("thread %s: kernel stack overflowed", t->t_name);
    }
}

/*
 * Where a thread whose entry function returned lands, reached from the
 * trampoline in swtch.S. Falling off the end of an entry function and
 * exiting deliberately are the same thing.
 */
void
thread_exited(void)
{
    thread_exit();
}

/*
 * Section 10.4 has exit advance a completion timeline that a joiner reads.
 * There are no timelines yet, so all that happens here is that the thread
 * stops being scheduled. The pool slot is not returned either, because
 * reaping belongs with that same later work.
 */
/*
 * A deadline passed while this thread was still blocked.
 *
 * It has to come off whatever it was blocked on before being made
 * runnable, not merely be made runnable. Leaving it on the list lets that
 * object release it a second time, and a thread cannot be made runnable
 * twice. Symmetric with the signal path, which has always done both.
 */
void
thread_wake_timeout(struct thread *t)
{
    if (t->t_unwait != NULL && t->t_blocked_on != NULL) {
        t->t_unwait(t->t_blocked_on, t);
    }

    t->t_waitres = SYS_ERR_TIMEOUT;
    sched_ready(t);
}

void
thread_exit(void)
{
    current->t_state = THREAD_DEAD;
    sched_block();

    panic("thread %s: scheduled again after exiting", current->t_name);
}

void
thread_bootstrap(const char *name, uint8_t prio, struct proc *p)
{
    struct thread *t;

    t = pool_alloc(thread_pool(), NULL);
    if (t == NULL) {
        panic("thread: no slot for the boot context");
    }

    check_prio(name, prio);

    t->t_id = pool_id_of(thread_pool(), t);
    t->t_name = name;
    t->t_prio = prio;
    t->t_base_prio = prio;
    t->t_proc = p;
    t->t_state = THREAD_RUNNING;

    /*
     * The guard goes at the bottom of the boot stack, not into the object,
     * because that is the stack this context actually runs on.
     */
    t->t_guard = (uint64_t *)(void *)_stack_bottom;
    *t->t_guard = THREAD_GUARD;

    /*
     * No stack is prepared. This context is already running on the stack
     * the boot code set up, and t_sp is written by the first switch away
     * from it.
     */
    current = t;
}

struct thread *
thread_create(const char *name, uint8_t prio, void (*entry)(void *),
  void *arg)
{
    struct thread *t;

    check_prio(name, prio);

    t = pool_alloc(thread_pool(), NULL);
    if (t == NULL) {
        panic("thread %s: pool exhausted", name);
    }

    t->t_id = pool_id_of(thread_pool(), t);
    t->t_name = name;
    t->t_prio = prio;
    t->t_base_prio = prio;
    t->t_proc = thread_current() != NULL ? thread_current()->t_proc : NULL;
    t->t_state = THREAD_NEW;
    t->t_guard = (uint64_t *)(void *)t->t_stack;
    *t->t_guard = THREAD_GUARD;

    md_thread_init(t, entry, arg);

    return t;
}

void
thread_switch_to(struct thread *next)
{
    struct thread *prev = current;

    if (next == prev) {
        return;
    }
    if (next->t_state == THREAD_UNUSED) {
        panic("thread: switch to an unbuilt thread");
    }

    check_guard(prev);
    check_guard(next);

    current = next;

    md_switch(&prev->t_sp, next->t_sp);

    /*
     * Reached when something switches back to prev, which by then is
     * current again. Everything above ran in the other thread's context.
     */
    check_guard(prev);
}
