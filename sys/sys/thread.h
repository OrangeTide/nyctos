/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Threads and the context switch.
 *
 * A thread here is a kernel thread: an execution context with its own
 * kernel stack and nothing else. Section 10.2 of design.md says the creator
 * supplies the stack, and that is about the *user* stack a thread runs on
 * once processes exist. The kernel stack is a different thing, supplied by
 * the port for every thread that has one, and section 28 suggests keeping
 * it in the same allocation as the object. That is what happens here: the
 * stack is the tail of the structure.
 *
 * The guard word sits immediately below the stack, so that an overflow runs
 * into it before it reaches any field. On Tier 0 there is no hardware to
 * catch the write, which is exactly what section 10.2 says about the weaker
 * tier being unable to catch its own violations. A word that can be checked
 * on every switch is the most that can be done here, and it turns silent
 * corruption into a message.
 *
 * There is no scheduler yet. A thread runs until it names its successor.
 */

#ifndef _SYS_THREAD_H_
#define _SYS_THREAD_H_

#include <sys/config.h>
#include <sys/pool.h>

#include <stdint.h>

#define THREAD_GUARD    ((uint64_t)0x9e3779b97f4a7c15ULL)

enum thread_state {
    THREAD_UNUSED = 0,
    THREAD_READY,
    THREAD_RUNNING,
};

struct thread {
    /*
     * Saved kernel stack pointer, and the only field md_switch knows about.
     * Everything below the stack pointer at a switch is the saved context.
     */
    void               *t_sp;

    sys_id_t            t_id;
    enum thread_state   t_state;
    const char         *t_name;

    /*
     * What this thread is blocked on, per the invariant in section 5. It
     * stays NULL until there is something to block on; the field is here
     * now because section 28 asks for exactly one of them and adding it
     * later would mean revisiting every path that sets a state.
     */
    void               *t_blocked_on;

    uint64_t            t_guard;
    uint8_t             t_stack[SYS_KSTACK_SIZE] __attribute__((aligned(16)));
};

/*
 * Adopt the context already running as a thread, so that the first switch
 * has somewhere to save to. Its stack field is unused: it is already
 * running on the boot stack and keeps it.
 */
void thread_bootstrap(const char *name);

struct thread *thread_create(const char *name, void (*entry)(void *),
  void *arg);

/* Switch to next, explicitly. There is no scheduler to consult. */
void thread_switch_to(struct thread *next);

struct thread *thread_current(void);

/* Machine-dependent, in sys/arch/<arch>. */
void md_thread_init(struct thread *t, void (*entry)(void *), void *arg);
void md_switch(void **old_sp, void *new_sp);

#endif /* _SYS_THREAD_H_ */
