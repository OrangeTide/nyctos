/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Threads, without a scheduler.
 *
 * Stage 2 of milestone 1 builds the context switch and nothing above it. A
 * thread runs until it names its successor, which is enough to get register
 * save and restore debugged in the simplest setting it will ever be in.
 * Stage 3 puts a ready queue and a dispatch decision in front of this and
 * thread_switch_to stops being something callers name a target for.
 */

#include <sys/object.h>
#include <sys/systm.h>
#include <sys/thread.h>

static struct thread *current;

struct thread *
thread_current(void)
{
    return current;
}

static void
check_guard(const struct thread *t)
{
    if (t->t_guard != THREAD_GUARD) {
        panic("thread %s: kernel stack overflowed", t->t_name);
    }
}

/*
 * The entry function returning has no meaning yet. Exit, and the completion
 * timeline that reports it, arrive in stage 9; until then a thread that
 * falls off the end is a bug in the thread.
 */
void
thread_exited(void)
{
    panic("thread %s: returned from its entry function", current->t_name);
}

void
thread_bootstrap(const char *name)
{
    struct thread *t;

    t = pool_alloc(thread_pool(), NULL);
    if (t == NULL) {
        panic("thread: no slot for the boot context");
    }

    t->t_id = pool_id_of(thread_pool(), t);
    t->t_name = name;
    t->t_state = THREAD_RUNNING;
    t->t_guard = THREAD_GUARD;

    /*
     * No stack is prepared. This context is already running on the stack
     * the boot code set up, and t_sp is written by the first switch away
     * from it.
     */
    current = t;
}

struct thread *
thread_create(const char *name, void (*entry)(void *), void *arg)
{
    struct thread *t;

    t = pool_alloc(thread_pool(), NULL);
    if (t == NULL) {
        panic("thread %s: pool exhausted", name);
    }

    t->t_id = pool_id_of(thread_pool(), t);
    t->t_name = name;
    t->t_state = THREAD_READY;
    t->t_guard = THREAD_GUARD;

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

    prev->t_state = THREAD_READY;
    next->t_state = THREAD_RUNNING;
    current = next;

    md_switch(&prev->t_sp, next->t_sp);

    /*
     * Reached when something switches back to prev, which by then is
     * current again. Everything above ran in the other thread's context.
     */
    check_guard(prev);
}
