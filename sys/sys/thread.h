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
 * A guard word sits immediately below every thread stack, so that an
 * overflow runs into it before it reaches anything else. On Tier 0 there is
 * no hardware to catch the write, which is exactly what section 10.2 says
 * about the weaker tier being unable to catch its own violations. A word
 * checked on every switch is the most that can be done here, and it turns
 * silent corruption into a message.
 *
 * The guard is reached through a pointer rather than read out of the object
 * directly, because the boot context runs on the stack the linker script
 * laid out rather than on one of these. Without the indirection its guard
 * would sit in an object that nothing writes past, and would never fire
 * however badly the boot stack overflowed.
 *
 * There is no scheduler yet. A thread runs until it names its successor.
 */

#ifndef _SYS_THREAD_H_
#define _SYS_THREAD_H_

#include <sys/config.h>
#include <sys/pool.h>

#include <stddef.h>
#include <stdint.h>

#define THREAD_GUARD    ((uint64_t)0x9e3779b97f4a7c15ULL)

enum thread_state {
    THREAD_UNUSED = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_DEAD,
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
     * The effective priority, which is the donated one once donation
     * exists: section 11.2 has the scheduler see that value and nothing
     * else, so there is no second base-priority field to keep in step.
     */
    uint8_t             t_prio;

    /* Link for the FIFO at this level. Owned by the scheduler. */
    struct thread      *t_qnext;

    /*
     * What this thread is blocked on, per the invariant in section 5. It
     * stays NULL until there is something to block on; the field is here
     * now because section 28 asks for exactly one of them and adding it
     * later would mean revisiting every path that sets a state.
     */
    void               *t_blocked_on;

    /*
     * The lowest word of this thread stack region, which is the guard. For
     * a thread created here that is the base of t_stack; for the boot
     * context it is the bottom of the boot stack, which the linker script
     * names. Usable stack is therefore SYS_KSTACK_SIZE minus this word.
     */
    uint64_t           *t_guard;

    uint8_t             t_stack[SYS_KSTACK_SIZE] __attribute__((aligned(16)));
};

/*
 * The guard is the lowest word of the stack itself, so nothing can come
 * between them and no field order can separate them. What still has to
 * hold is the alignment the frame arithmetic in md_thread_init depends on,
 * and that is exactly the kind of property a later edit breaks silently:
 * an earlier version of this structure put the guard in its own field and
 * grew eight bytes of padding between it and the stack the moment another
 * pointer was added.
 */
_Static_assert(SYS_KSTACK_SIZE % 16 == 0,
  "kernel stack size must keep the stack pointer 16-byte aligned");
_Static_assert(sizeof(struct thread) % 16 == 0,
  "thread object size must keep pool slots 16-byte aligned");

/*
 * Adopt the context already running as a thread, so that the first switch
 * has somewhere to save to. It keeps the boot stack it is already running
 * on, so the stack inside its object is never used; the pool slots are a
 * uniform size, so nothing is saved by making that a special case.
 */
void thread_bootstrap(const char *name, uint8_t prio);

struct thread *thread_create(const char *name, uint8_t prio,
  void (*entry)(void *), void *arg);

/*
 * Leave the run set for good. Section 10.4 has exit advance a completion
 * timeline that a joiner reads; there are no timelines yet, so for now this
 * only stops the thread being scheduled. The slot is not returned to the
 * pool, because reaping is part of the same later work.
 */
void thread_exit(void) __attribute__((noreturn));

/*
 * The context switch itself. Called by the scheduler and by nothing else:
 * choosing the next thread is the dispatch decision of section 11.2, and a
 * caller naming its own successor would be going around it.
 */
void thread_switch_to(struct thread *next);

struct thread *thread_current(void);

/* Machine-dependent, in sys/arch/<arch>. */
void md_thread_init(struct thread *t, void (*entry)(void *), void *arg);
void md_switch(void **old_sp, void *new_sp);

#endif /* _SYS_THREAD_H_ */
