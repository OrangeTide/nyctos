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
#include <sys/channel.h>
#include <sys/intr.h>
#include <sys/cohort.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/vm.h>

static struct thread *current;

#if SYS_TIER == 2
/* The space the hardware is currently translating through. */
static uintptr_t current_as_root;
#endif

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
    thread_exit(0);
}

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
thread_exit(int32_t code)
{
    struct proc *p = current->t_proc;

    current->t_exit_code = code;

    /*
     * The status is written before the timeline advances, and both
     * before the thread leaves the run set. A joiner released by the
     * advance can outrank this thread and run before it reaches
     * sched_block, so everything a joiner reads has to be true
     * already; and sched_block does not return, so nothing owed to
     * one can be left until after it.
     */
    if (timeline_signal(&current->t_completion, SYS_COMPLETION_EXITED)
      != SYS_OK) {
        panic("thread %s: exited twice", current->t_name);
    }
    if (p != NULL) {
        proc_thread_exited(p, code);
    }

    current->t_state = THREAD_DEAD;
    sched_block();

    panic("thread %s: scheduled again after exiting", current->t_name);
}

void
thread_fault(uint32_t kind, uintptr_t addr, uintptr_t pc)
{
    /*
     * Recorded before the exit, because the exit is what releases a
     * joiner and a joiner released by it reads these.
     */
    current->t_fault = kind;
    current->t_fault_addr = addr;
    current->t_fault_pc = pc;

    /*
     * A member of a cohort takes its cohort with it, per section 18.2,
     * and here rather than where the process ends. A process ends when
     * its last thread does; a domain ends when any thread of any member
     * writes something no one can vouch for, which is now. Reading it
     * off the process at exit missed a fault in a member with more than
     * one thread entirely: the early return for a process with threads
     * left came first, the record was overwritten by whichever thread
     * did leave last, and the program exited zero.
     */
    if (current->t_proc != NULL && current->t_proc->p_cohort != NULL) {
        cohort_fault_halt(current->t_proc->p_cohort, current->t_name,
          kind, addr, pc);
    }

    thread_exit(SYS_EXIT_FAULT);
}

/*
 * The name of a fault kind, for a message a person reads. Every kind of
 * section 16.1 is here, including the one no port raises yet, so that a
 * kind arriving from a mapping that has been extended prints as itself
 * rather than as a number.
 */
const char *
fault_name(uint32_t kind)
{
    switch (kind) {
    case SYS_FAULT_NONE:        return "none";
    case SYS_FAULT_PAGE:        return "page";
    case SYS_FAULT_ACCESS:      return "access";
    case SYS_FAULT_ALIGN:       return "alignment";
    case SYS_FAULT_INSTRUCTION: return "instruction";
    case SYS_FAULT_ARITH:       return "arithmetic";
    case SYS_FAULT_DEBUG:       return "debug";
    case SYS_FAULT_EXTERNAL:    return "external abort";
    default:                    return "?";
    }
}

/*
 * Attach a thread to a process, keeping the count of live threads in
 * step. A process finishes when its last thread does, per section
 * 10.4, so the count is what decides that and nothing may set t_proc
 * behind it. A thread created by one process and handed to another
 * passes through both counts, which is why this subtracts before it
 * adds rather than assuming the thread is new.
 */
void
thread_set_proc(struct thread *t, struct proc *p)
{
    if (t->t_proc != NULL) {
        struct thread **link = &t->t_proc->p_threads;

        if (t->t_proc->p_nthreads == 0) {
            panic("proc %s: thread count below zero", t->t_proc->p_name);
        }

        while (*link != NULL && *link != t) {
            link = &(*link)->t_sibling;
        }
        if (*link != t) {
            panic("thread %s: not in its own process's list", t->t_name);
        }
        *link = t->t_sibling;
        t->t_sibling = NULL;

        t->t_proc->p_nthreads--;
        proc_thread_reaped(t->t_proc);
    }

    t->t_proc = p;

    if (p != NULL) {
        t->t_sibling = p->p_threads;
        p->p_threads = t;
        p->p_nthreads++;

        /*
         * A thread is a reason its process still exists, alongside
         * the handles naming it. It stays one after the thread dies
         * and until the thread is reaped, because until then there is
         * a slot pointing at the process.
         */
        proc_ref(p);
    }
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
    timeline_init(&t->t_completion, name);
    timeline_embed(&t->t_completion, SYS_OBJ_THREAD, t);
    thread_set_proc(t, p);
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

/*
 * The same, returning NULL rather than panicking when the pool is
 * empty. A kernel caller building the system has nowhere to report
 * that and nothing useful to do about it, but an operation a program
 * invoked has both: it returns SYS_ERR_NO_MEMORY and the program
 * decides.
 */
struct thread *
thread_alloc(const char *name, uint8_t prio, void (*entry)(void *),
  void *arg)
{
    struct thread *t;

    check_prio(name, prio);

    t = pool_alloc(thread_pool(), NULL);
    if (t == NULL) {
        return NULL;
    }

    t->t_id = pool_id_of(thread_pool(), t);
    t->t_name = name;
    t->t_prio = prio;
    t->t_base_prio = prio;
    t->t_proc = NULL;
    timeline_init(&t->t_completion, name);
    timeline_embed(&t->t_completion, SYS_OBJ_THREAD, t);
    t->t_state = THREAD_NEW;
    thread_set_proc(t,
      thread_current() != NULL ? thread_current()->t_proc : NULL);
    t->t_guard = (uint64_t *)(void *)t->t_stack;
    *t->t_guard = THREAD_GUARD;

    md_thread_init(t, entry, arg);

    return t;
}

struct thread *
thread_create(const char *name, uint8_t prio, void (*entry)(void *),
  void *arg)
{
    struct thread *t = thread_alloc(name, prio, entry, arg);

    if (t == NULL) {
        panic("thread %s: pool exhausted", name);
    }

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

#if SYS_TIER == 2
    /*
     * Into the incoming thread's address space, if it is not already
     * the current one. Every space contains the same kernel, so the
     * switch is safe from anywhere in the kernel: the instruction
     * after it is at the same address with the same contents.
     */
    {
        struct addrspace *as = proc_as(next->t_proc);

        if (as->as_root != current_as_root) {
            md_as_activate(as);
            vm_count_switch();
            current_as_root = as->as_root;
        }
    }
#endif

    check_guard(prev);
    check_guard(next);

    current = next;

    /*
     * What the last switch left behind, freed now that this is running
     * on a stack that is not it, and what this switch is leaving for
     * good, parked for the next one to free.
     */
    thread_park_if_dead(prev);

    md_switch(&prev->t_sp, next->t_sp);

    /*
     * Reached when something switches back to prev, which by then is
     * current again. Everything above ran in the other thread's context.
     */
    check_guard(prev);
}

/*
 * Reaping a thread, which cannot happen where the thread ends.
 *
 * A thread's kernel stack is inside its pool slot, so returning the slot
 * frees the memory the code doing the returning is standing on. The
 * thread that exits therefore cannot reap itself, and neither can
 * anything running before the switch away from it.
 *
 * So a dead thread with nothing left to ask about it is parked here and
 * collected at the start of the next switch, which runs on the incoming
 * thread's stack. One deep, because a thread is parked only while
 * switching away from it and a switch cannot begin before the last one
 * finished.
 */
static struct thread *reap_parked;

static void
thread_reap(struct thread *t)
{
    struct proc *p = t->t_proc;

    if (t->t_state != THREAD_DEAD) {
        panic("thread %s: reaped while still alive", t->t_name);
    }

    t->t_proc = NULL;

    if (!pool_free(thread_pool(), t->t_id)) {
        panic("thread: the pool refused a slot it handed out");
    }

    /*
     * After the slot, because this can reap the process, and the
     * process cannot go while a slot still points at it.
     */
    if (p != NULL) {
        proc_thread_reaped(p);
    }
}

/*
 * Collect whatever the last switch left, on a stack that is not it.
 * Called at the top of the switch and nowhere else.
 */
void
thread_reap_parked(void)
{
    struct thread *t = reap_parked;

    if (t == NULL) {
        return;
    }
    reap_parked = NULL;
    thread_reap(t);
}

/*
 * Whether this thread is finished with and nothing can ask about it. A
 * dead thread with handles outstanding stays: section 10.4 keeps its
 * status readable through them.
 */
static bool
thread_reapable(const struct thread *t)
{
    return t->t_state == THREAD_DEAD && t->t_refs == 0;
}

void
thread_ref(struct thread *t)
{
    t->t_refs++;
}

void
thread_unref(struct thread *t)
{
    if (t->t_refs == 0) {
        panic("thread %s: unreferenced more often than referenced",
          t->t_name);
    }
    t->t_refs--;

    /*
     * Not the running thread, and not a thread the switch has parked:
     * either way it is not on a stack anything is using, because a
     * thread reaches THREAD_DEAD only after it has switched away for
     * the last time and been parked or passed over.
     */
    if (thread_reapable(t) && t != current && t != reap_parked) {
        thread_reap(t);
    }
}

/* Park a thread the switch is leaving for the last time. */
void
thread_park_if_dead(struct thread *t)
{
    if (!thread_reapable(t)) {
        return;
    }
    if (reap_parked != NULL) {
        panic("thread %s: two threads parked for reaping at once",
          t->t_name);
    }
    reap_parked = t;
}

sys_err_t
thread_cancel(struct thread *t)
{
    intr_state_t s = md_intr_disable();

    if (t->t_state != THREAD_BLOCKED) {
        md_intr_restore(s);
        return SYS_ERR_WOULDBLOCK;
    }

    /*
     * Deferred, because releasing this thread is several steps and
     * one of them can make something else runnable: telling a server
     * its client has gone wakes the server, and a server that outranks
     * whoever is cancelling would run before the rest of this had
     * happened. The deferral is what makes a run of readies one step,
     * which is what it was built for.
     */
    sched_defer();

    /*
     * Off the object's list first, and off the deadline list with it.
     * Section 10.6's closed unblock list is what makes this possible
     * without knowing which object it is: whatever blocked the thread
     * left behind the one function that takes it off again.
     *
     * Merely making it runnable would leave it on the list, and the
     * object would go on to release a thread that has already been
     * released. That is the same reason the deadline path does both,
     * which is where this hook came from.
     */
    /*
     * What the blocking call was holding on this thread's behalf. A
     * client cancelled between the rendezvous and the reply has its
     * buffer in the server, and the pages are gone from its own memory
     * until something puts them back: releasing it without that is a
     * program running again with a hole where its buffer was. M4 left
     * a panic on the far side of that block saying this is where it
     * would be found. It is.
     *
     * Before the unwait, because taking a thread off what it was
     * blocked on is what forgets which object that was, and the
     * channel needs to know in order to tell the server at the other
     * end that this client has gone.
     */
    channel_thread_cancelled(t);

    if (t->t_unwait != NULL && t->t_blocked_on != NULL) {
        t->t_unwait(t->t_blocked_on, t);
    }
    time_sleep_cancel(t);

    t->t_waitres = SYS_ERR_INTERRUPTED;
    sched_ready(t);

    sched_undefer();
    md_intr_restore(s);

    return SYS_OK;
}
