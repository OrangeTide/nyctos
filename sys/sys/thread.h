/* thread.h : thread structure, states and the context switch.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares struct thread with its guard word, the thread_state and thread_wait
 * enumerations, and the thread lifecycle, reference, cancel and switch
 * functions. Also declares md_thread_init and md_switch, the port hooks for
 * thread setup and context switching. Included by the port thread sources, the
 * thread, scheduler, timeline and trace sources, and sys/object.h, sys/sched.h
 * and sys/trace.h.
 *
 */

#ifndef _SYS_THREAD_H_
#define _SYS_THREAD_H_

#include <sys/config.h>
#include <sys/op.h>
#include <sys/pool.h>
#include <sys/timeline.h>

#include <stdbool.h>

#include <stddef.h>
#include <stdint.h>

#define THREAD_GUARD    ((uint64_t)0x9e3779b97f4a7c15ULL)

/*
 * A subset of the states in section 10.6, carrying only what exists.
 *
 * READY means specifically "on a run queue", which is what lets the
 * scheduler reject a thread being made runnable twice. That distinction is
 * why NEW exists: a thread that has been built but not yet handed to the
 * scheduler is not runnable, and calling it READY would make the two
 * indistinguishable.
 */
enum thread_state {
    THREAD_UNUSED = 0,  /* a pool slot that was never built */
    THREAD_NEW,         /* built, not yet handed to the scheduler */
    THREAD_READY,       /* on a run queue */
    THREAD_RUNNING,
    THREAD_BLOCKED,     /* off the run queues, waiting for something */
    THREAD_DEAD,
};

/*
 * What a blocked thread is waiting for, per section 12.1.
 *
 * THREAD_BLOCKED says a thread is off the run queues and t_blocked_on says
 * which object it is off them for, but neither distinguishes the two sides
 * of a rendezvous, and section 12.1 makes that distinction load-bearing. A
 * client accumulating time in SEND is waiting for server capacity and the
 * answer is more receiving threads; a client accumulating time in REPLY is
 * waiting for the work itself and more threads will not help. Section 22.2
 * reports the two separately or it reports nothing useful about a
 * saturated server.
 */
enum thread_wait {
    THREAD_WAIT_NONE = 0,
    THREAD_WAIT_SEND,       /* queued on a channel, no server has taken it */
    THREAD_WAIT_REPLY,      /* a server holds the message and is working */
    THREAD_WAIT_RECV,       /* a server waiting with no message available */
    THREAD_WAIT_FENCE,      /* blocked on a timeline value */
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

    /* Authority is per-process; see sys/sys/proc.h. */
    struct proc        *t_proc;

    /*
     * The effective priority, which is the donated one once donation
     * exists: section 11.2 has the scheduler see that value and nothing
     * else, so there is no second base-priority field to keep in step.
     */
    uint8_t             t_prio;

    /*
     * What this thread would run at with nothing donated to it. Section
     * 11.2 has the scheduler see only the effective priority above, so
     * there is no second queue and no re-sorting pass: donation moves a
     * thread between queues when it happens, and the dispatch decision
     * never learns that donation exists.
     */
    uint8_t             t_base_prio;

    /* Link for the FIFO at this level. Owned by the scheduler. */
    struct thread      *t_qnext;

    /*
     * Which core this thread runs on, per section 11.4. Fixed when the
     * thread is created and changed only by an explicit migration:
     * nothing moves on its own, so where a thread runs is a fact about
     * the program rather than about load.
     */
    uint32_t            t_core;

    /*
     * Link for the list of threads another core has made runnable on
     * this one's. Separate from the queue link above because the two
     * are live at once: a thread is on the arriving list until the core
     * that owns it takes it off and puts it on a queue.
     */
    struct thread      *t_inext;

    /*
     * Which cores this thread has ever been queued on, one bit each.
     * Section 11.4 has nothing move on its own, and the way to check
     * that is to ask where a thread has been rather than where it is.
     */
    uint32_t            t_linked_on;

    /*
     * When an interrupt handler made this thread runnable, and how
     * many preemption points had been passed by then. Zero means it
     * was made runnable by something else. Section 13.3's second term
     * is the distance from here to this thread running in user mode.
     */
    uint64_t            t_irq_stamp;
    uint64_t            t_irq_points;

    /*
     * Preemption points this thread has passed, as opposed to the core
     * it is on. Section 13.3's second term is the length of one path,
     * and a per-core count includes whatever else that core ran in the
     * interval: a batch executing in another thread put sixteen points
     * into a figure that was supposed to be about a dispatch.
     */
    uint64_t            t_points;

    /*
     * When a client raised this thread's priority at a rendezvous, how
     * many preemption points had been passed by then, and whether the
     * client was on another core. Section 12.3's donation, timed from
     * the raise to this thread running.
     */
    uint64_t            t_don_stamp;
    uint64_t            t_don_points;
    uint32_t            t_don_core;
    bool                t_don_across;

    /*
     * The channel this thread's own request is being served on, and
     * the next request being served on the same one. Section 12.3's
     * chain is walked along these.
     *
     * Client-side since M22 stage four. These used to be the server's,
     * one channel and one link for a thread that can be inside
     * requests on several, and the lists ran into each other. A client
     * is served on one channel at a time, so one of each per thread is
     * a full account of the entries.
     */
    struct channel     *t_serve_ch;
    struct thread      *t_serve_next;

    /*
     * Wait state. A thread is blocked on at most one object at a time per
     * section 5, so one link and one awaited value is all any of this ever
     * needs, however many kinds of thing become waitable.
     */
    struct thread      *t_wnext;
    uint64_t            t_waitval;
    sys_err_t           t_waitres;
    enum thread_wait    t_wait;

    /*
     * Deadline, and the link for the list ordered by it. A thread can be
     * on a waiter list and on that list at once, which is not a second
     * thing it is blocked on: the deadline is when to give up on the one
     * thing it is blocked on, per section 14.4.
     */
    struct thread      *t_dnext;
    uint64_t            t_deadline;

    /*
     * Message state. A sender parks its request here and waits for a
     * reply to be written back; a server records which client it owes one
     * to. The blocked sender is itself the queue slot, which is why
     * nothing here is a queue.
     */
    struct thread      *t_snext;        /* sender or receiver list link */
    /*
     * The message this thread is carrying, in the kernel's own
     * memory.
     *
     * A rendezvous copies between two processes, and with an address
     * space each only one of them is addressable at a time: the
     * sender's message cannot be read while the receiver runs, and
     * the receiver's buffer cannot be written while the sender does.
     * Staging it here gives every copy one end in memory that is
     * mapped in every space and the other in whichever space is
     * current.
     *
     * It costs a copy of the header and the inline payload each way,
     * which is what a small message is. Tier 0 does the same thing
     * for the same code rather than keeping a second path that only
     * one tier takes.
     */
    struct sys_msg      t_stage;

    /*
     * The range of this thread's memory that is mapped into another
     * process for the duration of a call, per section 6.1, and which
     * process has it. Zero when this thread has no call outstanding that
     * carried a buffer, and at Tier 0 always, where a buffer needs no
     * mapping because there is only one memory.
     *
     * It is recorded here, on the thread whose memory it is, rather than
     * on the receiver: any thread holding this one's id may reply, and
     * the pages have to go home whichever of them does.
     */
    uintptr_t           t_lend_va;
    size_t              t_lend_len;
    struct proc        *t_lend_to;

    /*
     * Handles naming this thread. Its slot goes back to the pool when
     * this reaches zero and the thread has exited, for the reason the
     * process has the same field: an exit somebody still holds a
     * handle to is an exit that can still be asked about.
     */
    uint32_t            t_refs;

    /*
     * Whether the switch has finished with this thread's stack.
     *
     * A thread writes THREAD_DEAD and then calls sched_block, and
     * between the two it is dead and still running on its own
     * kernel stack. A reference dropped from another core in that
     * window used to collect it there and then, which hands the
     * slot back while a core is still on it. The switch sets this
     * when it leaves the thread, and nothing is collected before.
     */
    bool                t_left_stack;

    /* The next thread of the same process, per proc.p_threads. */
    struct thread      *t_sibling;

    /*
     * Whether this thread is inside the buffer move, so that what the
     * ports write can be charged to it. Per thread and not a global
     * flag: the timer interrupt can dispatch on its way out, so a
     * thread can be preempted in the middle of the move, and a global
     * flag would charge whatever ran next to the buffer path.
     */
    uint32_t            t_charging;

    struct sys_msg     *t_msg;          /* the request being carried */
    struct sys_msg     *t_reply;        /* where the reply is to be written */
    /*
     * The client of the receive that has just completed on this
     * thread, and not the record of what it owes.
     *
     * The two were one field until M22 stage two. This one is read
     * after a block to say which kind of wake it was, a sender having
     * filled it in and a pulse having left it zero, and it is what a
     * receive hands back as the peer. What is owed is the list below,
     * because a thread can owe several and this names one.
     */
    sys_thread_id_t     t_client;

    /*
     * How many clients this thread owes a reply to, which `t_client`
     * cannot say because it is one identifier.
     *
     * A receiving thread may hold a request and receive another:
     * section 12.6 describes a server that hands the client id to a
     * worker and keeps receiving, section 12.2 lets any thread of the
     * server send the reply, and the console does it in every session
     * with a line typed at it. Each of those clients is blocked with
     * `t_donee` pointing here, so this is the count of threads in that
     * state and is checked against a walk of them.
     *
     * Raised where a rendezvous makes this thread a server and lowered
     * where a client stops being owed, which is the three places
     * `t_donee` goes back to NULL.
     */
    uint32_t            t_owed;

    /*
     * And the clients themselves, newest first.
     *
     * No pool and no bound of its own: a client waiting for a reply is
     * blocked and is its own record, which is section 12's principle
     * that the blocked sender is the queue slot. A thread is on at most
     * one of these lists at a time, so one link per thread is enough
     * for every server in the machine, and the length of the longest
     * is bounded by the thread pool.
     *
     * `t_owed` is the length of this list and is kept beside it rather
     * than derived from it, so that the two can be checked against each
     * other and against a walk of every thread's `t_donee`.
     */
    struct thread      *t_owed_head;
    struct thread      *t_owed_next;
    struct thread      *t_donor;        /* who raised this thread priority */

    /*
     * And the other end of that: whose priority this thread raised, so
     * that a thread leaving a call by a route other than the reply can
     * put it back.
     *
     * The server records its donor and nothing recorded the donee,
     * because the only way a donation ever ended was the reply, and the
     * reply runs in the server and already has both. Cancellation ends
     * one from the other side, where the client is the only thread with
     * anything to go on.
     */
    struct thread      *t_donee;
    uint64_t            t_cookie;       /* connection the request arrived on */
    bool                t_replied;

    /*
     * What this thread is blocked on, per the invariant in section 5. It
     * stays NULL until there is something to block on; the field is here
     * now because section 28 asks for exactly one of them and adding it
     * later would mean revisiting every path that sets a state.
     */
    void               *t_blocked_on;

    /*
     * How to take this thread off whatever it is blocked on, for a wake
     * that did not come from that object. A deadline expiring is the first
     * such wake and cancellation will be the next, and both have to leave
     * the thread off the list rather than merely runnable: otherwise the
     * object can go on to release a thread that has already been released.
     */
    void              (*t_unwait)(void *obj, struct thread *t);

    /*
     * The completion timeline of section 10.4, and the code a monotonic
     * counter cannot carry. The timeline is part of the thread rather
     * than an entry in the timeline pool, per section 8, so obtaining
     * it cannot fail and the accounting for it is exact.
     *
     * Nothing here is reaped. The object keeps its slot after exit,
     * which is what makes the status readable afterwards and is also
     * why M7 rather than this milestone owns the freezing of a
     * completion timeline into the handle slots that still hold it.
     */
    /*
     * Where this thread starts in user mode, for one the program
     * created. Section 10.2 has the creator supply the stack, so the
     * kernel installs all three and the thread begins with no code of
     * its own having run: a first thread has crt1 to set its stack up,
     * and a created thread has nothing.
     */
    uintptr_t           t_uentry;
    uintptr_t           t_uarg;
    uintptr_t           t_ustack;


    /*
     * What ended this thread, if the hardware did, per section 16.2. The
     * kind is SYS_FAULT_NONE for a thread that exited by asking to, which
     * is what makes the two tellable apart by a joiner that reads the
     * status rather than by one that knows what the program was doing.
     */
    uint32_t            t_fault;

    /*
     * Whether this thread has ever reached user mode, set by the
     * trampoline that takes it there. A thread that has not is the
     * one a kill could not reach before M21 stage two, and it is
     * not the same as a thread that has not entered the kernel: a
     * program that computes and calls nothing has run and has no
     * preemption points to show for it.
     */
    bool                t_user_entered;

    /*
     * Whether this thread is ending because it read the kill mark,
     * set by each of the three places that read it and by nothing
     * else, per M21 stage seven's review.
     *
     * It exists so that a thread which ended while its process was
     * marked but for a reason of its own can be counted where that
     * happens rather than worked out by subtraction. A number
     * arrived at by taking the doors away from the total adds back
     * up to the total whatever either of them is.
     */
    bool                t_left_by_door;
    uintptr_t           t_fault_addr;
    uintptr_t           t_fault_pc;

    struct timeline     t_completion;
    int32_t             t_exit_code;

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
void thread_bootstrap(const char *name, uint8_t prio, struct proc *p);

/*
 * Attach a thread to a process, keeping the count of live threads in
 * step. A process finishes when its last thread does, per section 10.4,
 * so nothing may write t_proc behind this.
 */
void thread_set_proc(struct thread *t, struct proc *p);

/*
 * Build a thread. thread_alloc returns NULL when the pool is empty and
 * thread_create panics, which is the difference between an operation a
 * program invoked and a kernel caller assembling the system.
 */
struct thread *thread_alloc(const char *name, uint8_t prio,
  void (*entry)(void *), void *arg);

struct thread *thread_create(const char *name, uint8_t prio,
  void (*entry)(void *), void *arg);

/* Forward reference; a thread belongs to the process that created it. */
struct proc;

/*
 * Leave the run set for good, recording code as the exit status.
 *
 * Section 10.4 has exit advance a completion timeline that a joiner
 * reads, and it now does: the thread advances its own, and the last
 * thread of a process advances the process one as well. Nothing is
 * reaped and no slot is reused, which is M7 and is what keeps the
 * status readable through a handle held afterwards.
 */
void thread_exit(int32_t code) __attribute__((noreturn));

/*
 * End the running thread because the hardware raised something, per
 * section 16.2. It is thread_exit with a reason attached: the same
 * timeline advances, the same joiner is released, the same donated
 * priority unwinds. A fault is an exit whose code the thread did not
 * choose.
 */
void thread_fault(uint32_t kind, uintptr_t addr, uintptr_t pc)
  __attribute__((noreturn));

/* The name of a fault kind, for a message a person reads. */
const char *fault_name(uint32_t kind);

/*
 * A handle naming this thread was made, or destroyed. The second one
 * reaps the thread if it has already exited and nothing else can ask
 * about it.
 */
void thread_ref(struct thread *t);
void thread_unref(struct thread *t);

/*
 * Called by the switch: collect what the last one left, and park what
 * this one is leaving for good. Reaping a thread frees the stack it
 * runs on, so neither can happen in the thread it happens to.
 */
void thread_reap_parked(void);
void thread_park_if_dead(struct thread *t);

/* Release a thread whose deadline passed while it was still blocked. */
void thread_wake_timeout(struct thread *t);

/*
 * Release a thread from whatever it is blocked on, per section 10.5, so
 * that its pending call returns SYS_ERR_INTERRUPTED.
 *
 * SYS_ERR_WOULDBLOCK if it is not blocked. Prompt, in the sense the
 * section means: the thread is off the object's list and runnable when
 * this returns, and nothing is left for it to be released from twice.
 */
sys_err_t thread_cancel(struct thread *t);

/*
 * The context switch itself. Called by the scheduler and by nothing else:
 * choosing the next thread is the dispatch decision of section 11.2, and a
 * caller naming its own successor would be going around it.
 */
void thread_switch_to(struct thread *next);

struct thread *thread_current(void);

/*
 * The reply token of section 5: an identity that confers nothing, as
 * opposed to a handle, which is authority. A server learns this by serving
 * a client and acquires no power over it by doing so.
 */
sys_thread_id_t thread_id(const struct thread *t);
struct thread *thread_by_id(sys_thread_id_t id);

/* Effective priority changed; move the thread if it is queued. */
void sched_reprioritise(struct thread *t, uint8_t prio);

/*
 * Used only by the idle path. Section 11.6 has a core either running a
 * thread or idle, so while it idles there is no current thread, and an
 * interrupt handler that readies the thread which just blocked must not
 * find it still marked as running.
 */
void thread_set_current(struct thread *t);

/* Machine-dependent, in sys/arch/<arch>. */
void md_thread_init(struct thread *t, void (*entry)(void *), void *arg);
void md_switch(void **old_sp, void *new_sp);

/* Which cores this thread has ever been queued on, one bit each. */
uint32_t thread_linked_cores(const struct thread *t);

/* How many things are keeping this thread in existence. */
uint32_t thread_refs(const struct thread *t);

#endif /* _SYS_THREAD_H_ */
