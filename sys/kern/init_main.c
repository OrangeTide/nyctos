/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <sys/config.h>
#include <sys/handle.h>
#include <sys/object.h>
#include <sys/op.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/timeline.h>

#include <stdbool.h>

static const char *
errname(sys_err_t e)
{
    switch (e) {
    case SYS_OK:                return "OK";
    case SYS_ERR_INVALID_ARG:   return "INVALID_ARG";
    case SYS_ERR_NO_MEMORY:     return "NO_MEMORY";
    case SYS_ERR_PERM_DENIED:   return "PERM_DENIED";
    case SYS_ERR_STALE:         return "STALE";
    case SYS_ERR_WRONG_TYPE:    return "WRONG_TYPE";
    case SYS_ERR_NOT_SUPPORTED: return "NOT_SUPPORTED";
    case SYS_ERR_OWNER_DEAD:    return "OWNER_DEAD";
    case SYS_ERR_INTERRUPTED:   return "INTERRUPTED";
    default:                    return "?";
    }
}

static void
expect(const char *what, sys_err_t got, sys_err_t want)
{
    if (got != want) {
        panic("%s: expected %s, got %s", what, errname(want), errname(got));
    }
    cnprintf("    %-38s %s\n", what, errname(got));
}

static sys_err_t
run(uint16_t opcode, sys_handle_t target, void *args, uint32_t arg_len)
{
    struct sys_op op;
    sys_err_t err;

    memset(&op, 0, sizeof(op));
    op.opcode = opcode;
    op.target = target;
    op.arg_ptr = (uintptr_t)args;
    op.arg_len = arg_len;
    op.status = SYS_ERR_INTERRUPTED;    /* so a missing write is visible */

    err = sys_op_exec(&op);

    if (op.status != err) {
        panic("op %u: status field and return value disagree", opcode);
    }

    return err;
}

/*
 * Everything below goes through the section 23 entry point rather than
 * calling the handle table directly, so what is exercised is the path a
 * user trap will take: opcode dispatch, argument checking, the status
 * write, and the return value agreeing with it.
 */
static sys_err_t
fence(struct timeline *tl, uint64_t value, bool signalling)
{
    struct sys_op_fence_args args;
    sys_handle_t h;
    sys_err_t err;

    /*
     * Reached through a handle so the rights check is exercised too, which
     * is what a caller outside the kernel will have to do.
     */
    err = handle_insert(&proc_current()->p_handles, tl, SYS_OBJ_TIMELINE,
      signalling ? SYS_RIGHT_SIGNAL : SYS_RIGHT_WAIT, &h);
    if (err != SYS_OK) {
        panic("fence: no handle slot");
    }

    args.value = value;
    err = run(signalling ? SYS_OP_FENCE_SIGNAL : SYS_OP_FENCE_WAIT, h,
      &args, sizeof(args));

    handle_close(&proc_current()->p_handles, h);

    return err;
}

static void
handle_checks(void)
{
    struct proc *p = proc_current();
    struct sys_op_dup_args dup;
    sys_handle_t h, weak;
    void *obj;

    /*
     * Something to point a handle at. A thread is the only object type that
     * exists, which is convenient rather than meaningful.
     */
    expect("insert a thread handle",
      handle_insert(&p->p_handles, thread_current(), SYS_OBJ_THREAD,
        SYS_RIGHT_DUPLICATE | SYS_RIGHT_READ | SYS_RIGHT_WRITE, &h), SYS_OK);

    expect("lookup with a right it has",
      handle_lookup(&p->p_handles, h, SYS_OBJ_THREAD, SYS_RIGHT_READ, &obj),
      SYS_OK);
    expect("lookup with a right it lacks",
      handle_lookup(&p->p_handles, h, SYS_OBJ_THREAD, SYS_RIGHT_SEND, &obj),
      SYS_ERR_PERM_DENIED);
    expect("lookup as the wrong object type",
      handle_lookup(&p->p_handles, h, SYS_OBJ_CHANNEL, 0, &obj),
      SYS_ERR_WRONG_TYPE);

    /* Attenuation: less is allowed, more is refused rather than clamped. */
    memset(&dup, 0, sizeof(dup));
    dup.new_rights = SYS_RIGHT_READ;
    expect("dup attenuating to fewer rights",
      run(SYS_OP_HANDLE_DUP, h, &dup, sizeof(dup)), SYS_OK);
    weak = dup.out_handle;

    expect("the weaker handle lacks what it dropped",
      handle_lookup(&p->p_handles, weak, SYS_OBJ_THREAD, SYS_RIGHT_WRITE,
        &obj), SYS_ERR_PERM_DENIED);

    memset(&dup, 0, sizeof(dup));
    dup.new_rights = SYS_RIGHT_READ | SYS_RIGHT_WRITE;
    expect("dup amplifying from the weaker handle",
      run(SYS_OP_HANDLE_DUP, weak, &dup, sizeof(dup)), SYS_ERR_PERM_DENIED);

    expect("dup with no argument structure",
      run(SYS_OP_HANDLE_DUP, h, NULL, 0), SYS_ERR_INVALID_ARG);

    /* Close, and the generation makes the old value stale rather than reused. */
    expect("close the original",
      run(SYS_OP_HANDLE_CLOSE, h, NULL, 0), SYS_OK);
    expect("the closed handle is stale",
      handle_lookup(&p->p_handles, h, SYS_OBJ_NONE, 0, &obj), SYS_ERR_STALE);
    expect("closing it twice",
      run(SYS_OP_HANDLE_CLOSE, h, NULL, 0), SYS_ERR_STALE);
    expect("the duplicate still works",
      handle_lookup(&p->p_handles, weak, SYS_OBJ_THREAD, SYS_RIGHT_READ,
        &obj), SYS_OK);

    expect("close the duplicate",
      run(SYS_OP_HANDLE_CLOSE, weak, NULL, 0), SYS_OK);

    if (p->p_handles.ht_used != 0) {
        panic("handles: %u left in the table", p->p_handles.ht_used);
    }
}


/*
 * The four waiters of the figure in section 9.1, which the text says
 * exhaust the cases an implementation must handle.
 *
 *   A blocks on a value that arrives.
 *   B asks for a value already passed and returns at once.
 *   C blocks on a value that never arrives and is released by the failure.
 *   D arrives after the failure and is failed at once.
 *
 * A and B together are the property that matters most: there is no window
 * in which a waiter can miss a value, so no registration protocol. C and D
 * are the error state, and the difference between them is that the error is
 * a property the timeline keeps rather than an event you had to be present
 * for.
 */

#define PRIO_BOOT       20
#define PRIO_WAITER     25

static struct timeline *tl_a;   /* signalled normally */
static struct timeline *tl_c;   /* abandoned by its signaler */
static sys_handle_t     h_c_signal;

static void
waiter_a(void *arg)
{
    (void)arg;
    cnputs("    A: blocking on 10\n");
    expect("A: released when 10 arrives", fence(tl_a, 10, false), SYS_OK);
    thread_exit();
}

static void
waiter_c(void *arg)
{
    (void)arg;
    cnputs("    C: blocking on 99, which nobody will signal\n");
    expect("C: released by the abandonment", fence(tl_c, 99, false),
      SYS_ERR_OWNER_DEAD);
    thread_exit();
}

static void
timeline_checks(void)
{
    struct proc *p = proc_current();
    struct thread *a, *c;
    sys_handle_t h_a_signal;
    sys_err_t err;

    tl_a = timeline_create("a");
    tl_c = timeline_create("c");
    if (tl_a == NULL || tl_c == NULL) {
        panic("timelines: pool exhausted");
    }

    /*
     * Held open for the whole test, and load-bearing rather than
     * incidental. fence() below inserts a handle per call and closes it
     * again, so without a signal right outliving those calls the first
     * signal would take the count from zero to one and back to zero, and
     * the timeline would abandon itself between one line and the next.
     */
    err = handle_insert(&p->p_handles, tl_a, SYS_OBJ_TIMELINE,
      SYS_RIGHT_WAIT | SYS_RIGHT_SIGNAL, &h_a_signal);
    expect("a signal-righted handle on A", err, SYS_OK);

    err = handle_insert(&p->p_handles, tl_c, SYS_OBJ_TIMELINE,
      SYS_RIGHT_WAIT | SYS_RIGHT_SIGNAL, &h_c_signal);
    expect("a signal-righted handle on C", err, SYS_OK);

    /* B: a value already passed satisfies a wait with no blocking at all. */
    expect("signal A to 5", fence(tl_a, 5, true), SYS_OK);
    expect("B: waiting on 3, already passed", fence(tl_a, 3, false), SYS_OK);
    expect("B: waiting on 5, exactly reached", fence(tl_a, 5, false), SYS_OK);

    expect("signalling backwards to 4", fence(tl_a, 4, true),
      SYS_ERR_INVALID_ARG);
    expect("signalling the value it is already at", fence(tl_a, 5, true),
      SYS_ERR_INVALID_ARG);

    /*
     * A and C run above this thread, so readying one preempts immediately
     * and it is already blocked on its timeline when control comes back.
     * That is what makes the releases below observable in order rather
     * than all at the end.
     */
    a = thread_create("A", PRIO_WAITER, waiter_a, NULL);
    c = thread_create("C", PRIO_WAITER, waiter_c, NULL);
    sched_ready(a);
    sched_ready(c);

    expect("signal A to 10, releasing its waiter", fence(tl_a, 10, true),
      SYS_OK);

    /*
     * C's only signal right goes. Nobody can reach 99 now, so its waiter is
     * released with an error rather than left blocked for ever.
     */
    cnputs("    dropping the last signal right on C\n");
    expect("close C's signal handle",
      run(SYS_OP_HANDLE_CLOSE, h_c_signal, NULL, 0), SYS_OK);

    /* D: arriving after the failure is failed just as promptly. */
    expect("D: waiting on an abandoned timeline", fence(tl_c, 99, false),
      SYS_ERR_OWNER_DEAD);
    expect("D: waiting on a value it did reach", fence(tl_c, 0, false),
      SYS_OK);
    expect("signalling an abandoned timeline", fence(tl_c, 100, true),
      SYS_ERR_OWNER_DEAD);

    /*
     * A leaves the same way C did. Asserting the table drains afterwards is
     * what makes a handle held on purpose distinguishable from one
     * forgotten, which reading it alone does not.
     */
    expect("close A signal handle",
      run(SYS_OP_HANDLE_CLOSE, h_a_signal, NULL, 0), SYS_OK);

    if (p->p_handles.ht_used != 0) {
        panic("timelines: %u handles left in the table",
          p->p_handles.ht_used);
    }
}
void
kernel_main(void)
{
    struct proc *kproc;

    cnputs("Nyctos/" ARCH_NAME " kernel booting\n");

    object_init();
    object_check();
    object_report();

    sched_init();
    kproc = proc_bootstrap("kernel");
    thread_bootstrap("boot", PRIO_BOOT, kproc);

    cnputs("ops: the entry point\n");
    expect("nop", run(SYS_OP_NOP, 0, NULL, 0), SYS_OK);
    expect("an opcode that is defined but unbuilt",
      run(SYS_OP_IPC_SEND, 0, NULL, 0), SYS_ERR_NOT_SUPPORTED);
    expect("an opcode that is not defined at all",
      run(SYS_OP_COUNT, 0, NULL, 0), SYS_ERR_INVALID_ARG);

    cnputs("handles: authority, rights and staleness\n");
    handle_checks();

    cnputs("timelines: the four waiters of section 9.1\n");
    timeline_checks();

    cnputs("stage 5 complete\n");
    machine_halt(0);
}
