/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <sys/config.h>
#include <sys/object.h>
#include <sys/sched.h>
#include <sys/systm.h>
#include <sys/thread.h>

/*
 * Three properties of the dispatch decision, none of which needs a timer.
 *
 * That the highest runnable thread runs, whatever order threads were made
 * runnable in. That within one level the policy is FIFO, so equal priority
 * does not round-robin: section 11.1 makes that an opt-in with an explicit
 * quantum, and there is no quantum here. And that a thread becoming
 * runnable at a higher priority takes the core at once rather than waiting
 * for the running thread to finish, which is what section 11.3 means by
 * preemption and is separate from any quantum.
 *
 * All three come out of one arrangement. The boot context runs at the top
 * of the range so that readying a worker does not preempt it mid-setup,
 * every worker is made runnable before any of them runs, and then boot
 * blocks and the queue drains in priority order.
 */

static struct thread *t_waker;

static void
worker(void *arg)
{
    const struct thread *self = thread_current();
    int i;

    (void)arg;

    for (i = 0; i < 2; i++) {
        cnprintf("    %s (prio %u)\n", self->t_name, self->t_prio);
    }

    thread_exit();
}

/*
 * Above everything else runnable. Nothing readies it until a lower-priority
 * thread does, at which point that thread must lose the core at once.
 */
static void
waker(void *arg)
{
    (void)arg;

    cnputs("    waker (prio 25): preempted the thread that readied me\n");
    thread_exit();
}

static void
provoker(void *arg)
{
    (void)arg;

    cnputs("    provoker (prio 7): readying a higher-priority thread\n");
    sched_ready(t_waker);
    cnputs("    provoker (prio 7): running again, so waker has finished\n");
    thread_exit();
}

void
kernel_main(void)
{
    cnputs("Nyctos/" ARCH_NAME " kernel booting\n");

    object_init();
    object_check();
    object_report();

    sched_init();
    thread_bootstrap("boot", SYS_PRIO_MAX);

    /*
     * Made runnable low to high, so an implementation that ran them in the
     * order they arrived would produce the opposite of the expected output
     * rather than accidentally the same one.
     */
    sched_ready(thread_create("low", 5, worker, NULL));
    sched_ready(thread_create("mid", 12, worker, NULL));
    sched_ready(thread_create("high", 20, worker, NULL));

    /* Equal priority, so the earlier one should finish before the later. */
    sched_ready(thread_create("first", 9, worker, NULL));
    sched_ready(thread_create("second", 9, worker, NULL));

    t_waker = thread_create("waker", 25, waker, NULL);
    sched_ready(thread_create("provoker", 7, provoker, NULL));

    cnputs("sched: six runnable, boot blocking\n");
    cnputs("sched: expect 20, 12, 9, 9, then 7 preempted by 25, then 5\n");
    sched_block();

    panic("boot: scheduled again after blocking");
}
