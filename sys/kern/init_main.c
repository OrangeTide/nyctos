/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <sys/measure.h>
#include <sys/lock.h>
#include <sys/blk.h>
#include <sys/bootarch.h>
#include <sys/batchreg.h>
#include <sys/channel.h>
#include <sys/cohort.h>
#include <sys/cpu.h>
#include <sys/config.h>
#include <sys/connection.h>
#include <sys/fs.h>
#include <sys/grant.h>
#include <sys/handle.h>
#include <sys/ledger.h>
#include <sys/intr.h>
#include <sys/pool.h>
#include <sys/object.h>
#include <sys/page.h>
#include <sys/op.h>
#include <sys/page.h>
#include <sys/peer.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/syspage.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/time.h>
#include <sys/timeline.h>
#include <sys/trace.h>
#include <sys/vm.h>

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
    case SYS_ERR_TIMEOUT:       return "TIMEOUT";
    case SYS_ERR_WOULDBLOCK:    return "WOULDBLOCK";
    case SYS_ERR_NO_ENT:        return "NO_ENT";
    case SYS_ERR_FAULT:         return "FAULT";
    case SYS_ERR_DEADLOCK:      return "DEADLOCK";
    default:                    return "?";
    }
}

/* A hex address as a string, for a command line that carries one. */
static void
hex_into(char *out, uintptr_t v)
{
    size_t n = 0;
    int i;

    for (i = 60; i >= 0; i -= 4) {
        unsigned int d = (unsigned int)((v >> i) & 0xf);

        if (n == 0 && d == 0 && i != 0) {
            continue;
        }
        out[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    out[n] = '\0';
}

/*
 * A copy of a handle to give away, keeping the original. Spawn moves what
 * it is given, per section 7.4, so a parent that means to keep something
 * duplicates first; this is that, with the rights the child needs and no
 * more.
 */
static sys_handle_t
share(sys_handle_t h)
{
    sys_handle_t copy;

    if (handle_dup(&proc_current()->p_handles, h,
      SYS_RIGHT_SEND | SYS_RIGHT_DUPLICATE | SYS_RIGHT_TRANSFER,
      &copy) != SYS_OK) {
        panic("namespace: cannot share handle %lu", (unsigned long)h);
    }

    return copy;
}

/* A mount name into a namespace entry, which is a fixed slot. */
static void
strncpy_ns(char *dst, const char *src)
{
    size_t i;

    for (i = 0; i + 1 < SYS_NS_NAME_MAX && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void
expect(const char *what, sys_err_t got, sys_err_t want)
{
    if (got != want) {
        panic("%s: expected %s, got %s", what, errname(want), errname(got));
    }
    cnprintf("    %-38s %s\n", what, errname(got));
}

static long
run_batch(sys_op_t *ops, unsigned long n)
{
    return syscall_handler(ops, n);
}

static sys_err_t
run_flagged(uint16_t opcode, sys_handle_t target, void *args,
  uint32_t arg_len, uint32_t flags)
{
    struct sys_op op;
    struct batch_regs regs;
    intr_state_t s = 0;
    sys_err_t err;
    bool held;

    /*
     * One entry is an execution of one entry, so it gets a file like
     * any other. Passing nothing would make a register read from a
     * single operation a case the operation had to know about, and
     * there is no reason for one entry to be a different kind of
     * thing from the first entry of two.
     */
    regs_init(&regs);

    memset(&op, 0, sizeof(op));
    op.opcode = opcode;
    op.target = target;
    op.arg_ptr = (uintptr_t)args;
    op.arg_len = arg_len;
    op.flags = flags;
    op.status = SYS_ERR_INTERRUPTED;    /* so a missing write is visible */

    /*
     * Under the kernel lock, because this is an entry and an entry
     * takes it. Every other entry in the system arrives from a
     * vector, which takes the lock on the way in and drops it on the
     * way out; these arrive from a kernel thread, which holds
     * nothing, and until now they ran the same operations with the
     * exclusion missing.
     *
     * What that cost was a join that hung. The wait on a completion
     * reads the timeline, finds it short of the value, and puts
     * itself on the waiter list, and the advance on the other core
     * writes the value and walks that list. Those two interleave in
     * one order that loses the wake: the reader has read, the writer
     * has walked, and then the reader arrives on a list nobody is
     * going to look at again. Measured at about one burst run in
     * twelve, a thirty second timeout on a process that had already
     * exited, with the timeline sitting at the value being waited
     * for.
     *
     * Taken here rather than inside each blocking call, because the
     * question is not which call blocks: it is that an entry runs
     * under the lock, and these were entries that did not.
     */
    held = klock_held();
    if (!held) {
        s = klock_enter();
    }

    err = sys_op_exec(&op, &regs);

    if (!held) {
        klock_exit(s);
    }

    if (op.status != err) {
        panic("op %u: status field and return value disagree", opcode);
    }

    return err;
}

/*
 * The same, with no flags, which is what almost every check wants. The
 * flags exist for M15's deadline, which is read only when the entry
 * says it is there.
 */
static sys_err_t
run(uint16_t opcode, sys_handle_t target, void *args, uint32_t arg_len)
{
    return run_flagged(opcode, target, args, arg_len, 0);
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
    args.deadline = SYS_DEADLINE_NEVER;
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

/*
 * The boot thread runs below everything it creates. Four stages running,
 * a test hung or silently did nothing because boot sat above a thread it
 * was waiting for and yielded to itself for ever. Lowest priority means a
 * yield here always hands the core over, and boot only resumes when
 * nothing else can run.
 *
 * It is one rather than a comfortable middle value so that the rule is
 * hard to break by accident. Documenting it was not enough: the fifth
 * occurrence was a thread deliberately created below a boot thread that
 * had been moved down to ten for exactly this reason.
 */
#define PRIO_BOOT       1
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
    thread_exit(0);
}

static void
waiter_c(void *arg)
{
    (void)arg;
    cnputs("    C: blocking on 99, which nobody will signal\n");
    expect("C: released by the abandonment", fence(tl_c, 99, false),
      SYS_ERR_OWNER_DEAD);
    thread_exit(0);
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

/*
 * The interrupt path of section 13.2, end to end: a device raises an
 * interrupt, the handler advances a timeline and returns, and a thread
 * waiting on that timeline runs. The handler does no driver work at all,
 * which is the whole of what the section asks for.
 *
 * The timeline is advanced to the number of interrupts seen, so a waiter
 * asking for the third interrupt is asking for a value rather than for an
 * event. That is what makes a late waiter safe in the way section 9.1
 * describes: if the interrupt has already happened the value has already
 * been passed, and the wait returns at once instead of hanging for
 * something that is over.
 */
static struct timeline *tl_irq;

static void
irq_waiter(void *arg)
{
    uint64_t n = (uint64_t)(uintptr_t)arg;

    cnprintf("    waiting for interrupt %lu\n", (unsigned long)n);
    expect("released by the interrupt", timeline_wait(tl_irq, n, SYS_DEADLINE_NEVER), SYS_OK);
    thread_exit(0);
}

static void
interrupt_checks(void)
{
    struct thread *w;
    unsigned int irq;

    tl_irq = timeline_create("device");
    if (tl_irq == NULL) {
        panic("interrupts: no timeline");
    }

    irq = md_timer_irq();
    expect("the timer is already claimed by the kernel",
      irq_attach(irq, tl_irq), SYS_ERR_INVALID_ARG);

    /* Runs above this thread, so it is already blocked when we return. */
    w = thread_create("waiter", PRIO_WAITER, irq_waiter, (void *)(uintptr_t)1);
    sched_ready(w);

    cnputs("    arming a one-shot for 10ms\n");
    if (timer_create("wake", tl_irq, time_now() + time_from_ms(10), 0)
      == NULL) {
        panic("interrupts: no timer");
    }

    /*
     * Blocking here leaves nothing runnable at all, so the core has to sit
     * in the idle state of section 11.6 until the timer ends it. A busy
     * wait would keep this thread runnable and never reach that path,
     * which is exactly the mistake the first version of this test made.
     *
     * Getting past this means the whole chain worked: hardware, vector,
     * handler, timeline, wake, and an idle core that an interrupt left.
     */
    expect("released after the core idled", timeline_wait(tl_irq, 1, SYS_DEADLINE_NEVER),
      SYS_OK);

    cnprintf("    the timeline reached %lu\n",
      (unsigned long)tl_irq->tl_value);

    /* A value already passed satisfies a wait with no interrupt needed. */
    expect("waiting for an interrupt already seen",
      timeline_wait(tl_irq, 1, SYS_DEADLINE_NEVER), SYS_OK);
}

/*
 * Timers, and the three properties section 14.2 states precisely enough to
 * check: that activation N happens at first + N * period whatever the
 * delivery delay, that the counter always holds the number of activations
 * the clock implies, and that catching up after a stall is one update
 * rather than a backlog.
 */
static sys_handle_t h_srv_recv;
static struct timeline *tl_shot;
static struct timeline *tl_tick;

static void
timer_checks(void)
{
    struct proc *p = proc_current();
    struct timer *shot, *tick;
    sys_time_t period, first, now;
    uint64_t n, before, after;
    sys_handle_t h;
    sys_err_t err;

    tl_shot = timeline_create("oneshot");
    tl_tick = timeline_create("periodic");
    if (tl_shot == NULL || tl_tick == NULL) {
        panic("timers: no timeline");
    }

    /* A one-shot advances its timeline once, and then it is spent. */
    shot = timer_create("shot", tl_shot, time_now() + time_from_ms(10), 0);
    if (shot == NULL) {
        panic("timers: pool exhausted");
    }
    expect("one-shot fires", timeline_wait(tl_shot, 1, SYS_DEADLINE_NEVER),
      SYS_OK);
    cnprintf("    one-shot left the counter at %lu\n",
      (unsigned long)tl_shot->tl_value);

    /*
     * A periodic timer, waited on activation by activation. Each one is
     * checked against the time it should have happened rather than against
     * the previous delivery, which is the difference the section insists
     * on: measuring from the last delivery would let jitter accumulate and
     * the check would pass anyway.
     */
    period = time_from_ms(5);
    first = time_now() + period;
    tick = timer_create("tick", tl_tick, first, period);
    if (tick == NULL) {
        panic("timers: pool exhausted");
    }

    for (n = 1; n <= 3; n++) {
        expect("activation arrives",
          timeline_wait(tl_tick, n, SYS_DEADLINE_NEVER), SYS_OK);
        now = time_now();
        if (now < first + (n * period)) {
            panic("timer: activation %lu arrived before its phase",
              (unsigned long)n);
        }
    }
    cnputs("    three activations, none of them early\n");

    /*
     * Stall for several periods without waiting. The counter should be
     * where the clock says, reached in one update, with nothing queued to
     * deliver afterwards.
     */
    before = tl_tick->tl_value;
    now = time_now();
    while (time_now() < now + time_from_ms(26)) {
        /* deliberately not waiting */
    }
    after = tl_tick->tl_value;

    cnprintf("    stalled 26ms across a 5ms period: counter %lu to %lu\n",
      (unsigned long)before, (unsigned long)after);
    if (after < before + 4) {
        panic("timer: counter did not catch up with the clock");
    }
    expect("the next activation after a stall",
      timeline_wait(tl_tick, after + 1, SYS_DEADLINE_NEVER), SYS_OK);

    /* Destroying a timer abandons its timeline, per section 14.2. */
    timer_destroy(tick);
    expect("waiting on a destroyed timer",
      timeline_wait(tl_tick, after + 100, SYS_DEADLINE_NEVER),
      SYS_ERR_OWNER_DEAD);

    /*
     * Deadlines are for failure detection, not activation, so a wait that
     * nothing will satisfy ends by giving up rather than by being woken.
     */
    err = handle_insert(&p->p_handles, tl_shot, SYS_OBJ_TIMELINE,
      SYS_RIGHT_WAIT, &h);
    expect("a wait-only handle", err, SYS_OK);
    expect("a deadline that has already passed",
      timeline_wait(tl_shot, 99, SYS_DEADLINE_NOW), SYS_ERR_TIMEOUT);
    expect("a deadline 10ms out on a value nobody will signal",
      timeline_wait(tl_shot, 99, time_now() + time_from_ms(10)),
      SYS_ERR_TIMEOUT);
    expect("close the wait-only handle",
      run(SYS_OP_HANDLE_CLOSE, h, NULL, 0), SYS_OK);
}

/*
 * Message passing, and the priority inheritance that comes with it.
 *
 * Two properties are worth demonstrating and neither is visible from a
 * single exchange. That a receive queue is ordered by sender priority
 * rather than arrival, since FIFO there would be an inversion no donation
 * could repair. And that a server runs at its client's priority for the
 * duration of a request and reverts on reply, which is what stops a
 * low-priority server holding up a high-priority client.
 */
static struct channel *ch_srv;
static uint8_t server_prio_seen;
static int served;

static void
server(void *arg)
{
    struct sys_op_recv_args rx;
    struct sys_op_reply_args tx;
    struct thread *self = thread_current();

    (void)arg;

    for (;;) {
        memset(&rx, 0, sizeof(rx));
        if (run(SYS_OP_IPC_RECV, h_srv_recv, &rx, sizeof(rx)) != SYS_OK) {
            panic("server: receive failed");
        }

        /* The priority this thread is running at while serving. */
        server_prio_seen = self->t_prio;
        served++;

        cnprintf("    server (base %u) serving '%s' at priority %u\n",
          self->t_base_prio, (const char *)rx.message.inline_data,
          self->t_prio);

        memset(&tx, 0, sizeof(tx));
        tx.peer = rx.peer;
        tx.reply.inline_len = 3;
        memcpy(tx.reply.inline_data, "ok", 3);
        if (run(SYS_OP_IPC_REPLY, 0, &tx, sizeof(tx)) != SYS_OK) {
            panic("server: reply failed");
        }

        if (self->t_prio != self->t_base_prio) {
            panic("server: still donated to after replying");
        }
    }
}

static void
client(void *arg)
{
    struct sys_op_send_args tx;
    const char *name = thread_current()->t_name;

    memset(&tx, 0, sizeof(tx));
    tx.request.inline_len = 10;
    memcpy(tx.request.inline_data, name, 10);

    if (run(SYS_OP_IPC_SEND, (sys_handle_t)(uintptr_t)arg, &tx, sizeof(tx))
      != SYS_OK) {
        panic("client: send failed");
    }
    cnprintf("    %s got reply '%s'\n", name,
      (const char *)tx.reply.inline_data);

    thread_exit(0);
}

/*
 * The run-time gate of section 22.4, which is the half of it that a
 * compile-time switch cannot demonstrate.
 *
 * A build carrying the console can have it dormant, so that an image ships
 * with the facility present and unusable and has it turned on to diagnose
 * something without becoming a different image. Refused and absent return
 * the same error deliberately: a caller behaves the same either way and
 * should not be able to tell which build it is running on.
 */
static void
debug_console_checks(void)
{
#if SYS_DEBUG_CONSOLE
    struct sys_op_debug_args args;
    static const char probe[] = "    (this line is written by the probe)\n";

    bool was_live = debug_console_is_live();

    memset(&args, 0, sizeof(args));
    args.buf = (uintptr_t)probe;
    args.len = sizeof(probe) - 1;

    cnprintf("    this build boots with it %s\n",
      was_live ? "live" : "dormant");

    /*
     * Set the state each case wants rather than assuming the one the
     * build chose, so this tests the gate and not SYS_DEBUG_CONSOLE_INIT.
     */
    debug_console_set(true);
    expect("the console writes while live",
      run(SYS_OP_DEBUG_WRITE, 0, &args, sizeof(args)), SYS_OK);

    debug_console_set(false);
    expect("and is refused while dormant",
      run(SYS_OP_DEBUG_WRITE, 0, &args, sizeof(args)),
      SYS_ERR_NOT_SUPPORTED);

    debug_console_set(true);
    expect("and writes again once live",
      run(SYS_OP_DEBUG_WRITE, 0, &args, sizeof(args)), SYS_OK);

    debug_console_set(was_live);
#else
    cnputs("    compiled out, so there is nothing to gate\n");
#endif
}

static void
ipc_checks(void)
{
    struct proc *p = proc_current();
    struct thread *srv, *lo, *hi;
    sys_handle_t send_h;
    sys_err_t err;

    ch_srv = channel_create("srv", p);
    if (ch_srv == NULL) {
        panic("ipc: no channel");
    }

    err = handle_insert(&p->p_handles, ch_srv, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV, &h_srv_recv);
    expect("a receive handle on the channel", err, SYS_OK);
    err = handle_insert(&p->p_handles, ch_srv, SYS_OBJ_CHANNEL,
      SYS_RIGHT_SEND, &send_h);
    expect("a send handle on the channel", err, SYS_OK);

    /*
     * The server runs below both clients, so without donation it would not
     * run until they had both given up the core, and neither can.
     */
    /*
     * Both clients send before the server exists, so both are queued on
     * the channel when it first receives. That is the only arrangement in
     * which the queue order is observable: with a server already waiting,
     * each send completes a rendezvous immediately and nothing is ever
     * queued to be ordered.
     */
    cnputs("    the lower priority client sends first, then the higher\n");
    lo = thread_create("client-lo", 21, client, (void *)(uintptr_t)send_h);
    hi = thread_create("client-hi", 24, client, (void *)(uintptr_t)send_h);
    sched_ready(lo);
    sched_ready(hi);

    /* Below both clients, so without donation it would not run at all. */
    srv = thread_create("server", 15, server, NULL);
    sched_ready(srv);

    /* Let everything settle. */
    while (served < 2) {
        sched_yield();
    }

    cnprintf("    the server ran at %u while serving, base 15\n",
      server_prio_seen);
    if (server_prio_seen <= 15) {
        panic("ipc: the server never inherited a client priority");
    }
}

/*
 * A server that owes two clients at different priorities, per M22
 * stage three.
 *
 * Section 12.3 gives a server the priority of whoever it is serving,
 * and until this stage the way back from that was to the base
 * priority, on the reasoning that a thread which has replied owes
 * nobody. A thread can owe several. Answering one of them says nothing
 * about the rest, and the thread went back to base while a client it
 * still owed waited at a priority it had given away.
 *
 * The construction is two clients queued before the server receives,
 * so that the queue order is the priority order and the higher one is
 * served first. The server holds that request, receives the lower, and
 * answers the lower. What it is running at after that is the question.
 *
 * A third thread makes the answer matter rather than merely be a
 * number. It is readied between the two rendezvous, runs above the
 * server's base and below the client it still owes, and the server
 * yields after the reply. At the priority it owes, the yield comes
 * back to the server and that thread has not run; at base, that thread
 * takes the core and the client waiting for its reply waits behind it,
 * which is the inversion section 12.3 exists to prevent.
 */
static struct channel *ch_owe;
static sys_handle_t h_owe_recv;
static uint8_t owe_hi_prio;
static uint8_t owe_lo_prio;
static uint8_t owe_prio_after;
static uint32_t owe_left_owing;
static struct thread *owe_mid;
static volatile int owe_mid_ran;
static int owe_mid_seen;
static volatile int owe_done;

#define OWE_BASE    15
#define OWE_MID     20
#define OWE_LO      21
#define OWE_HI      24

static void
owe_middle(void *arg)
{
    (void)arg;

    owe_mid_ran = 1;
    thread_exit(0);
}

static void
owe_server(void *arg)
{
    struct sys_op_recv_args rx;
    struct sys_op_reply_args tx;
    struct thread *self = thread_current();
    sys_thread_id_t peer_hi, peer_lo;

    (void)arg;

    memset(&rx, 0, sizeof(rx));
    if (run(SYS_OP_IPC_RECV, h_owe_recv, &rx, sizeof(rx)) != SYS_OK) {
        panic("owe: the first receive failed");
    }
    peer_hi = rx.peer;
    if (self->t_prio != OWE_HI) {
        panic("owe: served the higher client at %u, not %u",
          (unsigned int)self->t_prio, (unsigned int)OWE_HI);
    }

    /*
     * Held rather than answered, and the second receive is what makes
     * this thread owe two. Without the hold there is one client at a
     * time and the way back to base is right.
     */
    memset(&rx, 0, sizeof(rx));
    if (run(SYS_OP_IPC_RECV, h_owe_recv, &rx, sizeof(rx)) != SYS_OK) {
        panic("owe: the second receive failed");
    }
    peer_lo = rx.peer;
    if (self->t_owed != 2) {
        panic("owe: owes %u after two receives, not two",
          (unsigned int)self->t_owed);
    }

    /*
     * Ready here, between the receives and the reply, so that the
     * thread is runnable at the moment the priority is given back and
     * the yield below has something to give the core to.
     */
    sched_ready(owe_mid);

    memset(&tx, 0, sizeof(tx));
    tx.peer = peer_lo;
    tx.reply.inline_len = 3;
    memcpy(tx.reply.inline_data, "ok", 3);
    if (run(SYS_OP_IPC_REPLY, 0, &tx, sizeof(tx)) != SYS_OK) {
        panic("owe: the reply to the lower client failed");
    }

    owe_prio_after = self->t_prio;
    owe_left_owing = self->t_owed;

    sched_yield();
    owe_mid_seen = owe_mid_ran;

    memset(&tx, 0, sizeof(tx));
    tx.peer = peer_hi;
    tx.reply.inline_len = 3;
    memcpy(tx.reply.inline_data, "ok", 3);
    if (run(SYS_OP_IPC_REPLY, 0, &tx, sizeof(tx)) != SYS_OK) {
        panic("owe: the reply to the higher client failed");
    }

    if (self->t_prio != self->t_base_prio) {
        panic("owe: still at %u with nobody left to owe",
          (unsigned int)self->t_prio);
    }

    owe_done = 1;
    thread_exit(0);
}

/*
 * Both clients on the queue before the server exists.
 *
 * They are served in priority order only if both are queued when the
 * first receive happens, and readying a thread does not say when it
 * has reached its send: at Tier 2 it reaches it on the other core.
 * Waited for rather than assumed, which is M21 stage three's review
 * written as a rule, and read under the kernel lock because the queue
 * is a list the other core puts threads on.
 *
 * Yielded rather than slept on, because here the boot thread is below
 * both clients and giving up the core is what lets them run.
 */
static void
wait_senders(struct channel *ch, uint32_t want, const char *what)
{
    uint64_t until = time_now() + time_from_ms(2000);
    uint32_t cap = pool_slots(thread_pool());

    for (;;) {
        const struct thread *t;
        intr_state_t is;
        intr_state_t ks = 0;
        bool took = false;
        uint32_t n = 0;

        is = md_intr_disable();
        if (!klock_held()) {
            ks = klock_enter();
            took = true;
        }
        for (t = ch->c_senders; t != NULL; t = t->t_snext) {
            if (++n > cap) {
                panic("%s: the sender queue of %s does not end", what,
                  ch->c_name);
            }
        }
        if (took) {
            klock_exit(ks);
        }
        md_intr_restore(is);

        if (n >= want) {
            return;
        }
        if (time_now() > until) {
            panic("%s: %u queued on %s, not %u", what, (unsigned int)n,
              ch->c_name, (unsigned int)want);
        }
        sched_yield();
    }
}

static void
owe_checks(void)
{
    struct proc *p = proc_current();
    struct thread *srv, *lo, *hi;
    sys_handle_t send_h;
    sys_err_t err;

    cnputs("a server that owes two clients at once\n");

    ch_owe = channel_create("owe", p);
    if (ch_owe == NULL) {
        panic("owe: no channel");
    }
    err = handle_insert(&p->p_handles, ch_owe, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV, &h_owe_recv);
    expect("a receive handle on the channel", err, SYS_OK);
    err = handle_insert(&p->p_handles, ch_owe, SYS_OBJ_CHANNEL,
      SYS_RIGHT_SEND, &send_h);
    expect("a send handle on the channel", err, SYS_OK);

    owe_hi_prio = OWE_HI;
    owe_lo_prio = OWE_LO;

    /*
     * Both queued before the server exists, which is the only
     * arrangement in which the order they are served in is the order
     * of their priorities rather than the order they arrived.
     */
    lo = thread_create("owe-lo", OWE_LO, client, (void *)(uintptr_t)send_h);
    hi = thread_create("owe-hi", OWE_HI, client, (void *)(uintptr_t)send_h);
    owe_mid = thread_create("owe-mid", OWE_MID, owe_middle, NULL);
    sched_ready(lo);
    sched_ready(hi);
    wait_senders(ch_owe, 2, "owe");

    srv = thread_create("owe-srv", OWE_BASE, owe_server, NULL);
    sched_ready(srv);

    while (!owe_done || !owe_mid_ran) {
        sched_yield();
    }

    cnprintf("    %-38s %u and %u, base %u\n",
      "the one it held and the one answered", (unsigned int)owe_hi_prio,
      (unsigned int)owe_lo_prio, (unsigned int)OWE_BASE);
    cnprintf("    %-38s %u, owing %u\n",
      "what it ran at after that reply", (unsigned int)owe_prio_after,
      (unsigned int)owe_left_owing);
    cnprintf("    %-38s %s\n", "and the thread at 20 it yielded to",
      owe_mid_seen ? "took the core" : "did not run");

    /*
     * The number first, because it is what the stage changes, and then
     * the consequence, because a number nothing depends on is a number
     * that can be wrong quietly.
     */
    if (owe_prio_after != owe_hi_prio) {
        panic("owe: left running at %u, not the %u it still owed",
          (unsigned int)owe_prio_after, (unsigned int)owe_hi_prio);
    }
    if (owe_left_owing != 1) {
        panic("owe: owed %u after answering one of two",
          (unsigned int)owe_left_owing);
    }
    if (owe_mid_seen) {
        panic("owe: a thread at %u took the core from a server owing %u",
          (unsigned int)OWE_MID, (unsigned int)owe_hi_prio);
    }

    /*
     * And the channel goes back, so that a check which exists to say
     * something about priorities does not quietly raise the number of
     * channels the boot ends holding. M20 made that number mean
     * something and a check that adds to it is a check taking it away.
     */
    expect("giving the channel back",
      handle_close(&p->p_handles, send_h), SYS_OK);
    expect("and the receive handle with it",
      handle_close(&p->p_handles, h_owe_recv), SYS_OK);
}

/*
 * A thread inside requests on two channels, per M22 stage four.
 *
 * The serving entry used to be the server thread, and a thread has one
 * `t_serve_next` between every list it could be on. A server holding a
 * request on one channel and taking one on another wrote the second
 * list into the field the first was using, so the first ran into the
 * second and unlinking took the thread off one of them. `README.md`
 * carried that as worse if it happened, since nothing in the boot did
 * it. This is the boot doing it.
 *
 * The entry is the request now, and a client is served on one channel
 * at a time, so the two lists are separate and each one ends.
 */
static struct channel *ch_two_a;
static struct channel *ch_two_b;
static sys_handle_t h_two_a;
static sys_handle_t h_two_b;
static uint32_t two_lists;
static uint32_t two_owed;
static volatile int two_done;

/* Whether a channel is serving anything, read under the lock. */
static bool
two_busy(struct channel *ch)
{
    intr_state_t is;
    intr_state_t ks = 0;
    bool took = false;
    bool busy;

    is = md_intr_disable();
    if (!klock_held()) {
        ks = klock_enter();
        took = true;
    }
    busy = ch->c_serving != NULL;
    if (took) {
        klock_exit(ks);
    }
    md_intr_restore(is);

    return busy;
}

/*
 * Both lists read at one moment, under the kernel's exclusion.
 *
 * The clients are blocked and this thread is the only one serving
 * these two channels, so nothing here is racing today. It is read
 * under the lock anyway because that is the rule M20 stage three and
 * M21 stage three both arrived at the hard way: a check that reads a
 * channel's lists with nothing held is a check that can be right about
 * a moment that never existed.
 */
static void
two_look(struct thread *self)
{
    intr_state_t is;
    intr_state_t ks = 0;
    bool took = false;
    struct thread *ea, *eb;

    is = md_intr_disable();
    if (!klock_held()) {
        ks = klock_enter();
        took = true;
    }

    ea = ch_two_a->c_serving;
    eb = ch_two_b->c_serving;
    two_owed = self->t_owed;
    two_lists = (ea != NULL ? 1u : 0u) + (eb != NULL ? 1u : 0u);

    /*
     * Each list holds one entry, that entry is a client, and the
     * thread serving it is this one. Walked to the end rather than
     * read at the head, because a list that runs into another one has
     * a head that looks right.
     */
    if (ea == NULL || ea->t_serve_next != NULL) {
        panic("two: the first list does not hold exactly one request");
    }
    if (eb == NULL || eb->t_serve_next != NULL) {
        panic("two: the second list does not hold exactly one request");
    }
    if (ea == eb) {
        panic("two: one entry is on both lists");
    }
    if (ea->t_donee != self || eb->t_donee != self) {
        panic("two: a list names a request this thread is not serving");
    }
    if (ea->t_serve_ch != ch_two_a || eb->t_serve_ch != ch_two_b) {
        panic("two: an entry names a channel it is not on");
    }

    if (took) {
        klock_exit(ks);
    }
    md_intr_restore(is);
}

static void
two_server(void *arg)
{
    struct sys_msg msg, rep;
    struct thread *self = thread_current();
    sys_thread_id_t peer_a, peer_b;
    uint64_t cookie;

    (void)arg;

    memset(&msg, 0, sizeof(msg));
    if (channel_recv(ch_two_a, &msg, &peer_a, &cookie) != SYS_OK) {
        panic("two: the receive on the first channel failed");
    }

    /*
     * Held, and the second request comes from the other channel. This
     * is the arrangement section 19.3 says a server should not be
     * written in and nothing refuses, and until this stage it was the
     * arrangement that corrupted a list.
     */
    memset(&msg, 0, sizeof(msg));
    if (channel_recv(ch_two_b, &msg, &peer_b, &cookie) != SYS_OK) {
        panic("two: the receive on the second channel failed");
    }

    two_look(self);

    memset(&rep, 0, sizeof(rep));
    rep.handle = SYS_ID_INVALID;
    rep.inline_len = 3;
    memcpy(rep.inline_data, "ok", 3);
    if (channel_reply(peer_a, &rep) != SYS_OK) {
        panic("two: the reply on the first channel failed");
    }
    if (two_busy(ch_two_a)) {
        panic("two: the first list still holds a request answered");
    }
    if (!two_busy(ch_two_b)) {
        panic("two: the second list lost its request to the first");
    }

    memset(&rep, 0, sizeof(rep));
    rep.handle = SYS_ID_INVALID;
    rep.inline_len = 3;
    memcpy(rep.inline_data, "ok", 3);
    if (channel_reply(peer_b, &rep) != SYS_OK) {
        panic("two: the reply on the second channel failed");
    }
    if (two_busy(ch_two_b)) {
        panic("two: the second list still holds a request answered");
    }

    two_done = 1;
    thread_exit(0);
}

static void
two_client(void *arg)
{
    struct channel *ch = arg;
    struct sys_msg req, rep;

    memset(&req, 0, sizeof(req));
    memset(&rep, 0, sizeof(rep));
    req.handle = SYS_ID_INVALID;
    req.inline_len = 4;
    memcpy(req.inline_data, "two", 4);
    if (channel_send(ch, 0, &req, &rep, SYS_DEADLINE_NEVER) != SYS_OK) {
        panic("two: a client was not answered");
    }

    thread_exit(0);
}

static void
two_channel_checks(void)
{
    struct proc *p = proc_current();
    struct thread *srv, *ca, *cb;
    sys_err_t err;

    cnputs("a server thread inside requests on two channels\n");

    ch_two_a = channel_create("two-a", p);
    ch_two_b = channel_create("two-b", p);
    if (ch_two_a == NULL || ch_two_b == NULL) {
        panic("two: no channel");
    }
    err = handle_insert(&p->p_handles, ch_two_a, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV, &h_two_a);
    expect("a handle on the first channel", err, SYS_OK);
    err = handle_insert(&p->p_handles, ch_two_b, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV, &h_two_b);
    expect("a handle on the second channel", err, SYS_OK);

    /*
     * Both clients queued before the server exists, and each on its
     * own channel, so the server takes one from each and the order it
     * takes them in is the order it asks.
     */
    ca = thread_create("two-a-cl", 21, two_client, ch_two_a);
    cb = thread_create("two-b-cl", 21, two_client, ch_two_b);
    sched_ready(ca);
    sched_ready(cb);
    wait_senders(ch_two_a, 1, "two");
    wait_senders(ch_two_b, 1, "two");

    srv = thread_create("two-srv", 15, two_server, NULL);
    sched_ready(srv);

    while (!two_done) {
        sched_yield();
    }

    cnprintf("    %-38s %u, on %u lists\n",
      "requests it held at once", (unsigned int)two_owed,
      (unsigned int)two_lists);
    cnprintf("    %-38s %s\n", "each list walked to its end",
      "one request, naming this thread");

    if (two_owed != 2 || two_lists != 2) {
        panic("two: owed %u on %u lists", (unsigned int)two_owed,
          (unsigned int)two_lists);
    }
    if (channel_serving_two == 0) {
        panic("two: the kernel counted no request on a second channel");
    }

    expect("giving the first channel back",
      handle_close(&p->p_handles, h_two_a), SYS_OK);
    expect("and the second", handle_close(&p->p_handles, h_two_b),
      SYS_OK);
}

/*
 * A client that dies while its request is held, and a channel that
 * must not go while one is, per M22 stage five.
 *
 * Two things follow from the entry being the request. A client that is
 * cancelled between the rendezvous and the reply takes its entry with
 * it, so what the server owes and what the channel is serving both
 * shrink by one and the priority follows the rest. And a channel whose
 * last holder lets go while a request is in flight on it is not free,
 * because the request is standing in it: that is M20's idle test asked
 * about a held request rather than about the thread that took it.
 */
static struct channel *ch_held;
static sys_handle_t h_held_recv;
static uint8_t held_prio_after;
static uint32_t held_owed_after;
static uint32_t held_listed_after;
static struct timeline held_tl;
static volatile int held_ready;
static volatile int held_done;
static uint32_t held_freed_after_close;
static uint32_t held_freed_after_reply;

#define HELD_BASE   15
#define HELD_LO     21
#define HELD_HI     24

static void
held_client(void *arg)
{
    struct channel *ch = arg;
    struct sys_msg req, rep;

    memset(&req, 0, sizeof(req));
    memset(&rep, 0, sizeof(rep));
    req.handle = SYS_ID_INVALID;
    req.inline_len = 5;
    memcpy(req.inline_data, "held", 5);

    /*
     * The error is not checked. One of these is cancelled out of the
     * rendezvous on purpose and comes back interrupted, which is the
     * case the stage is about.
     */
    (void)channel_send(ch, 0, &req, &rep, SYS_DEADLINE_NEVER);

    thread_exit(0);
}

/*
 * What the server owes and what the channel is serving, at one moment
 * and under the kernel's exclusion.
 */
static void
held_look(struct thread *self)
{
    intr_state_t is;
    intr_state_t ks = 0;
    bool took = false;
    uint32_t cap = pool_slots(thread_pool());
    const struct thread *e;
    uint32_t n = 0;

    is = md_intr_disable();
    if (!klock_held()) {
        ks = klock_enter();
        took = true;
    }

    for (e = ch_held->c_serving; e != NULL; e = e->t_serve_next) {
        if (++n > cap) {
            panic("held: the serving list of %s does not end",
              ch_held->c_name);
        }
    }
    held_listed_after = n;
    held_owed_after = self->t_owed;
    held_prio_after = self->t_prio;

    if (took) {
        klock_exit(ks);
    }
    md_intr_restore(is);
}

static void
held_server(void *arg)
{
    struct sys_msg msg, rep;
    struct thread *self = thread_current();
    sys_thread_id_t peer_hi, peer_lo;
    uint64_t cookie;

    (void)arg;

    memset(&msg, 0, sizeof(msg));
    if (channel_recv(ch_held, &msg, &peer_hi, &cookie) != SYS_OK) {
        panic("held: the first receive failed");
    }
    memset(&msg, 0, sizeof(msg));
    if (channel_recv(ch_held, &msg, &peer_lo, &cookie) != SYS_OK) {
        panic("held: the second receive failed");
    }

    if (self->t_owed != 2 || self->t_prio != HELD_HI) {
        panic("held: owes %u at %u before anything died",
          (unsigned int)self->t_owed, (unsigned int)self->t_prio);
    }

    /*
     * The boot thread does the cancelling, and this one has to stop
     * running for it to happen: the boot thread is below everything
     * and a yield from here comes straight back, because this thread
     * is at the priority it owes its highest client. So it blocks on
     * a timeline the canceller signals, which gives the core up
     * rather than handing it round.
     */
    held_ready = 1;
    if (timeline_wait(&held_tl, 1,
      time_now() + time_from_ms(2000)) != SYS_OK) {
        panic("held: the cancel never arrived");
    }

    held_look(self);

    /*
     * And the reply the dead client will never get is refused, which
     * is how a server finds out per section 12.1. Asked here rather
     * than assumed, because a server that still owed it would be
     * answered instead.
     */
    memset(&rep, 0, sizeof(rep));
    rep.handle = SYS_ID_INVALID;
    if (channel_reply(peer_hi, &rep) != SYS_ERR_STALE) {
        panic("held: the reply to the dead client was not refused");
    }

    memset(&rep, 0, sizeof(rep));
    rep.handle = SYS_ID_INVALID;
    rep.inline_len = 3;
    memcpy(rep.inline_data, "ok", 3);
    if (channel_reply(peer_lo, &rep) != SYS_OK) {
        panic("held: the reply to the living client failed");
    }
    if (self->t_prio != self->t_base_prio) {
        panic("held: at %u with nobody left to owe",
          (unsigned int)self->t_prio);
    }

    held_done = 1;
    thread_exit(0);
}

/*
 * The second half: a channel whose last holder goes while a request is
 * being served on it.
 */
static struct channel *ch_last;
static sys_handle_t h_last_recv;
static volatile int last_done;

static void
last_server(void *arg)
{
    struct sys_msg msg, rep;
    sys_thread_id_t peer;
    uint64_t cookie;
    struct proc *p = proc_current();

    (void)arg;

    memset(&msg, 0, sizeof(msg));
    if (channel_recv(ch_last, &msg, &peer, &cookie) != SYS_OK) {
        panic("held: the receive on the last-holder channel failed");
    }

    /*
     * The only handle goes while the request is in flight. Nothing
     * names the channel after this and the slot may not come back,
     * because a thread is blocked on a reply that has to arrive on it.
     */
    if (handle_close(&p->p_handles, h_last_recv) != SYS_OK) {
        panic("held: the last handle would not close");
    }
    held_freed_after_close = channel_freed;

    memset(&rep, 0, sizeof(rep));
    rep.handle = SYS_ID_INVALID;
    rep.inline_len = 3;
    memcpy(rep.inline_data, "ok", 3);
    if (channel_reply(peer, &rep) != SYS_OK) {
        panic("held: the reply on the last-holder channel failed");
    }
    held_freed_after_reply = channel_freed;

    last_done = 1;
    thread_exit(0);
}

static void
held_death_checks(void)
{
    struct proc *p = proc_current();
    struct thread *srv, *lo, *hi, *cl;
    uint32_t freed_before;
    sys_err_t err;

    cnputs("a client that dies while its request is held\n");

    timeline_init(&held_tl, "held-settle");

    ch_held = channel_create("held", p);
    if (ch_held == NULL) {
        panic("held: no channel");
    }
    err = handle_insert(&p->p_handles, ch_held, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV, &h_held_recv);
    expect("a receive handle on the channel", err, SYS_OK);

    lo = thread_create("held-lo", HELD_LO, held_client, ch_held);
    hi = thread_create("held-hi", HELD_HI, held_client, ch_held);
    sched_ready(lo);
    sched_ready(hi);
    wait_senders(ch_held, 2, "held");

    srv = thread_create("held-srv", HELD_BASE, held_server, NULL);
    sched_ready(srv);

    while (!held_ready) {
        sched_yield();
    }

    expect("cancelling the client it held above",
      thread_cancel(hi), SYS_OK);
    expect("and letting the server look",
      timeline_signal(&held_tl, 1), SYS_OK);

    while (!held_done) {
        sched_yield();
    }

    cnprintf("    %-38s %u, on a list of %u\n",
      "what it owed after the death", (unsigned int)held_owed_after,
      (unsigned int)held_listed_after);
    cnprintf("    %-38s %u, the one still owed\n",
      "and what it was left running at",
      (unsigned int)held_prio_after);

    if (held_owed_after != 1 || held_listed_after != 1) {
        panic("held: owed %u on a list of %u after one death",
          (unsigned int)held_owed_after,
          (unsigned int)held_listed_after);
    }
    if (held_prio_after != HELD_LO) {
        panic("held: left at %u, not the %u it still owed",
          (unsigned int)held_prio_after, (unsigned int)HELD_LO);
    }

    expect("giving the channel back",
      handle_close(&p->p_handles, h_held_recv), SYS_OK);

    /*
     * And the channel nothing names while a request is in flight.
     */
    ch_last = channel_create("held-last", p);
    if (ch_last == NULL) {
        panic("held: no second channel");
    }
    err = handle_insert(&p->p_handles, ch_last, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV, &h_last_recv);
    expect("a handle that will be the last one", err, SYS_OK);

    freed_before = channel_freed;
    cl = thread_create("last-cl", HELD_LO, held_client, ch_last);
    sched_ready(cl);
    wait_senders(ch_last, 1, "held");

    srv = thread_create("last-srv", HELD_BASE, last_server, NULL);
    sched_ready(srv);

    while (!last_done) {
        sched_yield();
    }

    cnprintf("    %-38s %s\n",
      "its last handle closed while serving",
      held_freed_after_close == freed_before ? "the slot stayed"
        : "the slot went");
    cnprintf("    %-38s %s\n", "and the reply that finished it",
      held_freed_after_reply == freed_before + 1 ? "gave the slot back"
        : "did not");

    if (held_freed_after_close != freed_before) {
        panic("held: the slot went back with a request in flight");
    }
    if (held_freed_after_reply != freed_before + 1) {
        panic("held: %u slots went back at the reply, not one",
          (unsigned int)(held_freed_after_reply - freed_before));
    }
}


/*
 * A server that dies owing several, per M22 stage six.
 *
 * The record held one client, so an exit released one client. A
 * server that had taken two requests left the second waiting for a
 * deadline, and the only thread that could have answered it was the
 * one that had just gone. The set names them all, so the exit can
 * reach them all.
 *
 * Both clients set a deadline, which is what tells the two outcomes
 * apart. Released by the death they come back with SYS_ERR_STALE at
 * once; left behind they come back with SYS_ERR_TIMEOUT a second
 * later, which is the shape of the defect and the shape of the
 * falsification.
 */
static struct channel *ch_owing;
static sys_handle_t h_owing_recv;
static volatile sys_err_t owing_err[2];
static volatile int owing_back;
static uint64_t owing_took;
static uint64_t owing_most;

#define OWING_BASE  15
#define OWING_LO    21
#define OWING_HI    24
#define OWING_MS    2000

static void
owing_client(void *arg)
{
    unsigned long which = (unsigned long)(uintptr_t)arg;
    struct sys_msg req, rep;

    memset(&req, 0, sizeof(req));
    memset(&rep, 0, sizeof(rep));
    req.handle = SYS_ID_INVALID;
    req.inline_len = 6;
    memcpy(req.inline_data, "dying", 6);

    owing_err[which] = channel_send(ch_owing, 0, &req, &rep,
      time_now() + time_from_ms(OWING_MS));
    owing_back++;

    thread_exit(0);
}

/*
 * Two requests taken and neither answered. The exit is the whole of
 * what this thread does with them.
 */
static void
owing_server(void *arg)
{
    struct sys_msg msg;
    struct thread *self = thread_current();
    sys_thread_id_t peer;
    uint64_t cookie;

    (void)arg;

    memset(&msg, 0, sizeof(msg));
    if (channel_recv(ch_owing, &msg, &peer, &cookie) != SYS_OK) {
        panic("owing: the first receive failed");
    }
    memset(&msg, 0, sizeof(msg));
    if (channel_recv(ch_owing, &msg, &peer, &cookie) != SYS_OK) {
        panic("owing: the second receive failed");
    }
    if (self->t_owed != 2) {
        panic("owing: owes %u of the 2 it took",
          (unsigned int)self->t_owed);
    }

    thread_exit(0);
}

static void
owing_checks(void)
{
    struct proc *p = proc_current();
    struct thread *srv, *lo, *hi;
    uint64_t t0;
    sys_err_t err;

    cnputs("a server that dies owing two\n");

    ch_owing = channel_create("owing", p);
    if (ch_owing == NULL) {
        panic("owing: no channel");
    }
    err = handle_insert(&p->p_handles, ch_owing, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV, &h_owing_recv);
    expect("a receive handle on the channel", err, SYS_OK);

    lo = thread_create("owing-lo", OWING_LO, owing_client,
      (void *)(uintptr_t)0);
    hi = thread_create("owing-hi", OWING_HI, owing_client,
      (void *)(uintptr_t)1);
    sched_ready(lo);
    sched_ready(hi);
    wait_senders(ch_owing, 2, "owing");

    t0 = time_now();
    srv = thread_create("owing-srv", OWING_BASE, owing_server, NULL);
    sched_ready(srv);

    while (owing_back < 2) {
        sched_yield();
    }
    owing_took = (time_now() - t0) * 1000000ULL / time_hz();
    owing_most = channel_owed_death_most;

    cnprintf("    %-38s %s and %s\n", "what the two clients were told",
      errname(owing_err[0]), errname(owing_err[1]));
    cnprintf("    %-38s %lu, in about %lu us\n",
      "and what one death reached at once",
      (unsigned long)owing_most, (unsigned long)owing_took);

    if (owing_err[0] != SYS_ERR_STALE || owing_err[1] != SYS_ERR_STALE) {
        panic("owing: told %s and %s, not both stale",
          errname(owing_err[0]), errname(owing_err[1]));
    }
    if (owing_most < 2) {
        panic("owing: one death reached %lu clients, not 2",
          (unsigned long)owing_most);
    }

    expect("giving the channel back",
      handle_close(&p->p_handles, h_owing_recv), SYS_OK);
}

/*
 * A server that outranks its client, which is the case that found the
 * defect this exists to keep out.
 *
 * channel_send hands the message over and then blocks. Where the server
 * outranks the sender, readying it used to dispatch in between: the server
 * ran, replied, and readied a sender that the preemption had already put
 * on a run queue and that had never reached sched_block. Nothing else in
 * the system arranges this. Donation cannot, because it raises a server to
 * exactly its client's priority and equal priority does not preempt, so
 * only a base priority above the client's reaches it.
 */
static struct channel *ch_hp;
static sys_handle_t h_hp_recv;
static int hp_served;

static void
hp_server(void *arg)
{
    struct sys_op_recv_args rx;
    struct sys_op_reply_args tx;

    (void)arg;

    for (;;) {
        memset(&rx, 0, sizeof(rx));
        if (run(SYS_OP_IPC_RECV, h_hp_recv, &rx, sizeof(rx)) != SYS_OK) {
            panic("hp server: receive failed");
        }

        memset(&tx, 0, sizeof(tx));
        tx.peer = rx.peer;
        tx.reply.inline_len = 3;
        memcpy(tx.reply.inline_data, "ok", 3);
        if (run(SYS_OP_IPC_REPLY, 0, &tx, sizeof(tx)) != SYS_OK) {
            panic("hp server: reply failed");
        }
        hp_served++;
    }
}

static void
hp_client(void *arg)
{
    struct sys_op_send_args tx;
    int i;

    /*
     * Several times, because the failure needed the server to be waiting
     * when the send arrived, and one send does not establish that it will
     * be on the next one.
     */
    for (i = 0; i < 3; i++) {
        memset(&tx, 0, sizeof(tx));
        tx.request.inline_len = 4;
        memcpy(tx.request.inline_data, "hp", 3);
        if (run(SYS_OP_IPC_SEND, (sys_handle_t)(uintptr_t)arg, &tx,
          sizeof(tx)) != SYS_OK) {
            panic("hp client: send failed");
        }
    }

    thread_exit(0);
}

static void
high_priority_server_checks(void)
{
    struct proc *p = proc_current();
    sys_handle_t send_h;
    sys_err_t err;

    ch_hp = channel_create("hp", p);
    if (ch_hp == NULL) {
        panic("hp: no channel");
    }
    err = handle_insert(&p->p_handles, ch_hp, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV, &h_hp_recv);
    expect("a receive handle on the high-priority channel", err, SYS_OK);
    err = handle_insert(&p->p_handles, ch_hp, SYS_OBJ_CHANNEL,
      SYS_RIGHT_SEND, &send_h);
    expect("a send handle on it", err, SYS_OK);

    /* The server above the client, which is the whole point. */
    sched_ready(thread_create("hp-server", 26, hp_server, NULL));
    sched_ready(thread_create("hp-client", 22, hp_client,
      (void *)(uintptr_t)send_h));

    while (hp_served < 3) {
        sched_yield();
    }

    cnprintf("    a server at 26 served a client at 22, %d times\n",
      hp_served);
}

/*
 * The vertical slice: one thread, one channel, one driver, one file read.
 *
 * A client process is handed a connection to a directory at spawn, walks
 * it to a file, reads the file, and prints what it read. The file is
 * served by a different process over the protocol of section 25, and
 * nothing on the path is inside the kernel except the message passing.
 *
 * The client is given exactly one handle and can therefore reach exactly
 * one directory. There is no root to name, no path that starts anywhere
 * but the connection it was handed, and nothing it could have guessed.
 */

#define FS_NODE_ROOT    0
#define FS_NODE_ALT     2

static const char motd_text[] =
    "served from outside the kernel\n";

static struct {
    const char *name;
    const char *data;
    uint32_t    len;
} fs_nodes[] = {
    { "/",     NULL,      0 },
    { "motd",  motd_text, sizeof(motd_text) - 1 },
};

static struct channel *ch_fs;
static sys_handle_t h_fs_recv;

/*
 * Joining a child, through the operations a program will use rather than
 * by reading its process structure: a handle to the process, the
 * completion timeline taken from it, an ordinary fence wait, and the
 * status read from the object afterwards.
 *
 * Watching and spawning have to be one operation, which is why this is
 * two calls and not one. A child that outranks the spawning thread runs
 * and exits inside proc_spawn, so the caller holds a deferral across both,
 * and the exit of a process nobody watches halts the machine.
 */
struct child {
    struct proc    *c_proc;
    sys_handle_t    c_handle;
    sys_handle_t    c_completion;
};

static void
child_watch(struct child *c, struct proc *p)
{
    struct sys_op_completion_args args;
    struct handle_table *ht = &proc_current()->p_handles;

    c->c_proc = p;
    if (handle_insert(ht, p, SYS_OBJ_PROCESS,
      SYS_RIGHT_WAIT | SYS_RIGHT_STAT, &c->c_handle) != SYS_OK) {
        panic("join %s: no handle slot for the child", p->p_name);
    }

    memset(&args, 0, sizeof(args));
    if (run(SYS_OP_OBJECT_COMPLETION, c->c_handle, &args,
      sizeof(args)) != SYS_OK) {
        panic("join %s: no completion timeline", p->p_name);
    }
    c->c_completion = args.out_handle;
}

/*
 * How long a child that only computes is given before its silence is
 * called a hang.
 *
 * It was one second, chosen when every program ran on one core. A read
 * that crosses a core boundary is slower, and under host load the
 * difference was enough to call a working program hung: the deadline
 * had stopped detecting a failure and started measuring the machine
 * this is running on. Section 14.3 says a deadline is for detecting a
 * failure, so it is set where only a failure reaches it.
 */
#define JOIN_MS     30000u

/*
 * How long M15's checks are willing to wait before calling a send
 * failed. Long enough that the wait is unmistakably a wait, short
 * enough to spend more than once in a boot.
 */
#define DEADLINE_MS 50u

/*
 * And how long M17 stage five is willing to wait. Longer than the rest,
 * because here the deadline is what the check is measuring against
 * rather than what it is waiting for: it costs nothing when the client
 * is released, and a system that left the client to time out would be
 * unmistakable rather than arguable.
 */
#define STALE_DEADLINE_MS 500u

/*
 * How many times the reader reads the clock in the run that is meant
 * to cost more. Large enough that the time it takes is plainly beside
 * a single reading, and small enough not to lengthen the boot: a
 * quarter of a million reads took 380 ms, which is most of a paced
 * line, and sixteen thousand take about twenty.
 */
#define CLOCK_READS 0x4000u

/*
 * The status a killed program carries in these checks. A number nothing
 * else here uses, so a status of it says a kill ended the program and
 * not the program itself.
 */
#define KILL_CODE 23

/*
 * How many sends give up in a boot, and from which of section 12.1's
 * two states. Three queued sends time out with nobody receiving: one
 * with nobody ever having received, one from M20 stage five with a
 * server that has been killed and no replacement coming, and one
 * from that milestone's stage seven whose channel has no handles
 * left at all, which is the case that proves a sender coming off the
 * queue gives the slot back. One client gives up while a server
 * holds its message, and one program does the same with a buffer
 * lent. Asserted because they are counts of what the checks ask for
 * and not of how fast the host is.
 */
#define DEADLINE_QUEUED_EXPECTED    3u
#define DEADLINE_WORKING_EXPECTED   3u

static int32_t
child_join(struct child *c, uint32_t within_ms)
{
    struct sys_op_status_args sargs;
    struct sys_op_fence_args fargs;
    struct handle_table *ht = &proc_current()->p_handles;

    fargs.value = SYS_COMPLETION_EXITED;
    /*
     * A deadline, because the failure this check is here to catch is
     * an exit that advances nothing, and waiting for ever on one is
     * a machine that hangs with no cause attached. Section 14.3: a
     * deadline is for detecting a failure, not for scheduling.
     *
     * How long depends on what the child waits for. A child that only
     * computes is done in milliseconds; a child that reads the console
     * is waiting for a person, and a person is slower than any number
     * that would still catch a hang.
     */
    fargs.deadline = time_now() + time_from_ms(within_ms);
    if (run(SYS_OP_FENCE_WAIT, c->c_completion, &fargs,
      sizeof(fargs)) != SYS_OK) {
        panic("join %s: it did not finish, or its exit advanced nothing",
          c->c_proc->p_name);
    }

    memset(&sargs, 0, sizeof(sargs));
    if (run(SYS_OP_PROC_STATUS, c->c_handle, &sargs,
      sizeof(sargs)) != SYS_OK) {
        panic("join %s: no status", c->c_proc->p_name);
    }
    if (sargs.out_state != SYS_STATE_EXITED) {
        panic("join %s: released by a completion that had not happened",
          c->c_proc->p_name);
    }

    handle_close(ht, c->c_completion);
    handle_close(ht, c->c_handle);

    return sargs.out_code;
}

/*
 * A process that does nothing but stop, at a priority below the thread
 * that spawns it so that it cannot have finished before it is asked
 * about. A child that outranks its parent would answer the "still
 * running" checks below by having already run.
 */
static void
exiter(void *arg)
{
    (void)arg;
    thread_exit(7);
}

/*
 * The frame allocator of section 8's extension, and the memory map the
 * port claims.
 *
 * The last frame is written and read back, which is the only part of this
 * that is about the machine rather than about the code. A port states its
 * memory rather than discovering it, per section 27, and a statement can
 * be wrong: a machine started with less memory than the constant says
 * would otherwise be found out by a page table landing in nothing, a long
 * way from the line that was mistaken.
 */
static void
page_checks(void)
{
    uintptr_t a, b, c;
    volatile uint64_t *top;
    uint32_t before;

    page_report();

    /*
     * The memory a loaded program occupies is not free memory, and the
     * allocator thought it was until this was written: a page table
     * would have landed in a running program's text, or the next spawn
     * would have written over a page table. Nothing had allocated
     * anything yet, so nothing had noticed.
     */
    if (page_take(boot_first_load())) {
        panic("frames: a loaded program's memory is in the free map");
    }

    before = page_used();

    a = page_alloc();
    b = page_alloc();
    if (a == 0 || b == 0) {
        panic("frames: the map is empty at boot");
    }
    if ((a & PAGE_MASK) != 0 || (b & PAGE_MASK) != 0) {
        panic("frames: %p or %p is not page-aligned", (void *)a, (void *)b);
    }
    if (a == b) {
        panic("frames: the same frame twice");
    }
    cnprintf("    two frames: %p and %p\n", (void *)a, (void *)b);

    /*
     * Zeroed, because the first use of a frame is a page table and a page
     * table with anything in it is a page table containing somebody
     * else's memory. Written and freed, so the check that follows is
     * against a frame that is known not to be blank.
     */
    if (*(volatile uint64_t *)a != 0) {
        panic("frames: %p came back with something in it", (void *)a);
    }
    *(volatile uint64_t *)a = 0x5eedfacefeedfaceULL;

    page_free(a);
    c = page_alloc();
    if (c != a) {
        panic("frames: a released frame was not the next one out");
    }
    if (*(volatile uint64_t *)c != 0) {
        panic("frames: %p was handed out with the last owner's bytes in it",
          (void *)c);
    }
    expect("a frame released and taken again", SYS_OK, SYS_OK);

    page_free(c);
    page_free(b);
    if (page_used() != before) {
        panic("frames: %u leaked", page_used() - before);
    }

    /*
     * The far end of what the port claims. Section 27 lets a port state
     * its memory map, and this is the line where a wrong statement stops
     * being a comment and starts being a fault.
     */
    if (!page_take(page_last())) {
        panic("frames: the last frame is spoken for at boot");
    }
    top = (volatile uint64_t *)(uintptr_t)(page_last());
    *top = 0x0123456789abcdefULL;
    if (*top != 0x0123456789abcdefULL) {
        panic("frames: the last frame the port claims is not memory");
    }
    page_free(page_last());

    cnprintf("    the last frame the port claims, %p, is memory\n",
      (void *)top);
}
/*
 * Exit, the completion timeline, and join, per section 10.4.
 *
 * The wait that times out is the part worth having. Without it the join
 * below proves only that the wait returned, which a timeline that was
 * already at its final value would also do, and the whole point is that
 * the exit is what advances it.
 */
static void
completion_checks(void)
{
    struct sys_op_completion_args cargs;
    struct sys_op_status_args sargs;
    struct sys_op_fence_args fargs;
    struct handle_table *ht = &proc_current()->p_handles;
    struct child c;
    struct proc *p;
    int32_t code;

    sched_defer();
    p = proc_spawn("exiter", PRIO_BOOT - 1, exiter, NULL, 0);
    child_watch(&c, p);
    sched_undefer();

    memset(&sargs, 0, sizeof(sargs));
    expect("the status of a process still running",
      run(SYS_OP_PROC_STATUS, c.c_handle, &sargs, sizeof(sargs)), SYS_OK);
    if (sargs.out_state != SYS_STATE_RUNNING) {
        panic("completion: a running process reported itself exited");
    }

    fargs.value = SYS_COMPLETION_EXITED;
    fargs.deadline = time_now();
    /*
     * A poll rather than a timed wait, because a timed wait would
     * block, and blocking is exactly what lets the child run: the
     * only other runnable thread is the one being asked about. A
     * deadline already passed is the polling form, per section 14.3.
     */
    expect("its completion has not been reached",
      run(SYS_OP_FENCE_WAIT, c.c_completion, &fargs, sizeof(fargs)),
      SYS_ERR_TIMEOUT);

    /*
     * Obtaining the completion of something that is neither a thread nor
     * a process is refused rather than reaching whatever the handle names.
     */
    memset(&cargs, 0, sizeof(cargs));
    expect("the completion of a timeline",
      run(SYS_OP_OBJECT_COMPLETION, c.c_completion, &cargs, sizeof(cargs)),
      SYS_ERR_WRONG_TYPE);

    code = child_join(&c, JOIN_MS);
    cnprintf("    exiter finished with status %d\n", code);
    if (code != 7) {
        panic("completion: joined a child and read status %d", code);
    }

    expect("the completion handle after the join",
      handle_lookup(ht, c.c_completion, SYS_OBJ_NONE, 0, NULL),
      SYS_ERR_STALE);
}

/*
 * The UART interrupt, attached and handed over.
 *
 * Stage 2 of this milestone read the device here, to show that an external
 * interrupt arrives at all on boards whose only interrupt had been a
 * timer. That was scaffolding and said so; the driver of stage 5 owns the
 * device now, and what stays in the kernel is what section 19.2 says a
 * launcher does: attach the interrupt to a timeline and hand the child the
 * handles, so that the authority to attach an interrupt stays with
 * whatever assembles the system.
 */
static struct timeline *tl_uart;
static sys_handle_t h_uart_irq, h_uart_fence;

static void
console_checks(void)
{
    struct handle_table *ht = &proc_current()->p_handles;
    unsigned int irq = md_uart_irq();

    tl_uart = timeline_create("uart-rx");
    if (tl_uart == NULL) {
        panic("console: no timeline");
    }
    expect("attaching the UART receive interrupt", irq_attach(irq, tl_uart),
      SYS_OK);

    /*
     * SYS_RIGHT_TRANSFER because both of these are going to a child, and
     * section 7.4 makes moving a handle a right rather than something a
     * holder may always do.
     */
    expect("a handle to that interrupt",
      handle_insert(ht, irq_object(irq), SYS_OBJ_IRQ,
        SYS_RIGHT_IRQ | SYS_RIGHT_TRANSFER, &h_uart_irq), SYS_OK);
    expect("unmasking one that is not masked",
      run(SYS_OP_IRQ_UNMASK, h_uart_irq, NULL, 0), SYS_ERR_INVALID_ARG);
    expect("a wait handle for its timeline",
      handle_insert(ht, tl_uart, SYS_OBJ_TIMELINE,
        SYS_RIGHT_WAIT | SYS_RIGHT_TRANSFER, &h_uart_fence), SYS_OK);
}

/*
 * An interrupt taken while a program was running, which no milestone
 * before this one could take at all.
 *
 * Both ports entered user mode with interrupts masked, which was
 * deliberate and written down: the AArch64 lower-EL vector was fatal, so
 * unmasking would have panicked rather than delivered. A driver is a
 * program that waits for a device, so both sides had to change, and this
 * is the check that says the change is real.
 *
 * The timer is periodic rather than one-shot, and that is not a
 * detail. Under emulation the virtual clock runs on wall time while
 * the first execution of a path pays for translating it, so a single
 * activation one millisecond out can expire before the child has run
 * an instruction, land in the kernel, and leave nothing for the
 * program to be interrupted by. That is a check failing when nothing
 * is wrong, which is worse than the check being absent, and it took
 * a run in three to show it. A period gives the program eighty
 * chances instead of one.
 *
 * Nothing the program does can produce the interrupt, which is the
 * point: it is being interrupted rather than trapping.
 */
static void
user_interrupt_checks(void)
{
    struct timeline *tl;
    struct timer *t;
    struct child c;
    unsigned long before = md_user_irqs;
    uint32_t tries;

    tl = timeline_create("spin-timer");
    if (tl == NULL) {
        panic("spin: no timeline");
    }

    /*
     * One signal right held across the whole check, because each try
     * makes a timer and destroys it, and destroying the last thing
     * that could signal a timeline puts it in the error state for
     * good under section 9.3 rule 2. Without this the second try binds
     * its timer to a timeline that can no longer be advanced, and
     * every later activation is refused. It ran that way and nothing
     * showed, because what this counts is interrupts and not values,
     * which is exactly the sort of quiet wrong worth not leaving in.
     *
     * The value it reaches is not what this reads and cannot be: a
     * one-shot signals activation one, so the second try offers a
     * value the timeline already has and is refused for going
     * backwards. What is being counted is interrupts.
     */
    timeline_add_signaler(tl);

    /*
     * Run it until one arrives, rather than once and insisting one did.
     *
     * What is being checked is that a program can be interrupted at
     * all, which is a property of the kernel. Whether one arrives
     * inside any particular run is a property of the host, and on two
     * cores under load it stopped being reliable: this failed about
     * one run in six with the machine busy and never with it quiet,
     * which is the shape this milestone warned itself about.
     *
     * Bounded, because a kernel that cannot be interrupted must still
     * fail rather than loop.
     */
    for (tries = 0; tries < 20 && md_user_irqs == before; tries++) {
        sched_defer();
        child_watch(&c, proc_spawn_image("spin", 20, NULL, 0, NULL, 0));

        /*
         * One shot, armed here and not once for the whole check.
         *
         * A repeating timer was what this used, at a millisecond, and
         * it hung the machine. A period is a rate against the host
         * clock, and a guest that cannot service that rate never
         * returns to the program between one activation and the next:
         * the timer was seen firing 1026 times a second while the
         * program it was meant to interrupt made no progress at all,
         * for as long as anyone was willing to wait. The check then
         * printed nothing and looked exactly like a hung kernel.
         *
         * A single activation cannot do that however slow the machine
         * is. Ten milliseconds is long enough that the child is
         * certainly running by then and short enough that it is
         * certainly still running: the program is seventy-two
         * milliseconds of work on one port and seventy-eight on the
         * other, measured, and both figures stretch together when the
         * host is busy.
         *
         * Armed while dispatch is deferred, so it is armed before the
         * child has had the core and comes due while it has it.
         */
        t = timer_create("spin", tl, time_now() + time_from_ms(10), 0);
        if (t == NULL) {
            panic("spin: no timer");
        }

        sched_undefer();
        (void)child_join(&c, JOIN_MS);
        timer_destroy(t);
    }

    timeline_drop_signaler(tl);

    if (md_user_irqs == before) {
        panic("nothing interrupted a program in %u runs of one",
          (unsigned int)tries);
    }
    cnprintf("    interrupts taken while a program was running: %lu\n",
      md_user_irqs - before);
}

/*
 * Spawn as an operation, per section 10.1: a program starting a program.
 *
 * Everything this exercises happens on the far side of the trap edge. The
 * kernel starts one program and reads one status; what that program did
 * with the four refusals it is also checking is reported by the status it
 * exits with, which is the child's if all of them held and a negative code
 * of its own if any did not.
 *
 * The status is seven rather than zero on purpose. Zero is what an
 * unwritten field holds.
 */
static void
spawn_checks(void)
{
    struct child c;
    int32_t code;

    sched_defer();
    child_watch(&c, proc_spawn_image("spawner", 20, NULL, 0, NULL, 0));
    sched_undefer();

    code = child_join(&c, JOIN_MS);
    cnprintf("    spawner reported %d\n", code);
    if (code != 7) {
        panic("spawn: the spawner exited %d, expected the child's 7", code);
    }
}

/*
 * Threads a program created, per section 10.2, and the rule that a process
 * finishes when its last thread does.
 *
 * The status is what makes the second half a check rather than a
 * demonstration. The main thread exits with zero while the thread it
 * created is still blocked, so a kernel that ended the process at the
 * first exit would report zero, and one that waits for the last reports
 * what only the second thread could have written.
 */
static void
thread_checks(void)
{
    struct child c;
    int32_t code;

    sched_defer();
    child_watch(&c, proc_spawn_image("threads", 20, NULL, 0, NULL, 0));
    sched_undefer();

    code = child_join(&c, JOIN_MS);
    cnprintf("    threads reported %d\n", code);
    if (code != 9) {
        panic("threads: the process reported %d, not its last thread's 9",
          code);
    }
}

/*
 * The console and the directory the shell is handed, kept from the checks
 * that created them. Root will hold both when there is a root; until then
 * the kernel is what assembles the system.
 */
static sys_handle_t h_console;
static sys_handle_t h_fs_root;

/* The two halves of the union the shell is handed; see user_slice. */
static sys_handle_t u_first;
static sys_handle_t u_second;

/*
 * A program with no ambient authority saying something to a person.
 *
 * Every program before this one printed through the debug console of
 * section 22.4, which takes no handle and reaches the device because it
 * exists. greet is built without it, so there is no call to the backdoor
 * anywhere in its image; what it has is a connection to the console
 * service, handed over at spawn. If it says anything at all, output
 * reached a person through a handle.
 *
 * It exits with the number of bytes it read, so the kernel can tell the
 * difference between a program that talked to the console and one that
 * only appeared to.
 */
static void
greet_checks(sys_handle_t conn)
{
    struct sys_op_dup_args dup;
    sys_ns_spec_t ns[1];
    struct child c;
    int32_t code;

    /*
     * A duplicate, because a handle moves rather than copies and this
     * thread has more to say afterwards. Attenuated to what a child needs
     * and no more: it may send on the connection and may hand it on, and
     * it may not mint another.
     */
    memset(&dup, 0, sizeof(dup));
    dup.new_rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a console handle for the child",
      run(SYS_OP_HANDLE_DUP, conn, &dup, sizeof(dup)), SYS_OK);

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_CONSOLE;
    ns[0].handle = dup.out_handle;

    sched_defer();
    child_watch(&c, proc_spawn_image("greet", 20, ns, 1, NULL, 0));
    sched_undefer();

    code = child_join(&c, 60000);
    cnprintf("    greet read %d bytes and said so through a handle\n", code);
    if (code < 0) {
        panic("greet: it could not reach the console it was given");
    }
}

/*
 * The shell, which is what this milestone is for.
 *
 * It is spawned exactly as any other program, with two handles and no
 * privilege: a duplicate of the console connection and a duplicate of the
 * filesystem root. Everything it can do is what those two reach, and
 * everything a program it starts can do is what the shell chooses to hand
 * on. There is nothing else in its namespace and no ambient anything
 * behind it.
 *
 * The kernel waits for it the way it waits for any child, which is also
 * how a supervisor will: a completion timeline and a status. A shell that
 * ends by being told to is a session that ended, and a shell that ends any
 * other way is a fault this reports rather than hides.
 */
static void
shell_checks(sys_handle_t console, sys_handle_t root)
{
    struct sys_op_dup_args dup;
    sys_ns_spec_t ns[5];
    struct child c;
    int32_t code;

    memset(&dup, 0, sizeof(dup));
    dup.new_rights = SYS_RIGHT_SEND | SYS_RIGHT_DUPLICATE |
      SYS_RIGHT_TRANSFER;
    expect("a console for the shell",
      run(SYS_OP_HANDLE_DUP, console, &dup, sizeof(dup)), SYS_OK);
    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_CONSOLE;
    ns[0].handle = dup.out_handle;
    /*
     * Unnamed, deliberately. Section 20.4 calls a namespace an array
     * of named *directory* handles, and the console is not one: it
     * is a service this process reaches by role, and nothing serves
     * walks for it. Naming it would put a thing in the table that
     * answers no path below it, and a resolver would hand a program
     * a handle it cannot walk into and call it a directory.
     */

    memset(&dup, 0, sizeof(dup));
    dup.new_rights = SYS_RIGHT_SEND | SYS_RIGHT_DUPLICATE |
      SYS_RIGHT_TRANSFER;
    expect("a directory for the shell",
      run(SYS_OP_HANDLE_DUP, root, &dup, sizeof(dup)), SYS_OK);
    ns[1].id = SYS_NS_ROOT;
    ns[1].handle = dup.out_handle;
    strncpy_ns(ns[1].name, "/");

    /*
     * And the same directory again, under a second name. Section 20.4's
     * clause is that two processes see different views assembled from
     * the same underlying handles, and this is what makes that
     * demonstrable rather than a rearrangement of words: one filesystem,
     * reachable as `/` and as `/files`, and a child given one of those
     * names sees one of them.
     */
    memset(&dup, 0, sizeof(dup));
    dup.new_rights = SYS_RIGHT_SEND | SYS_RIGHT_DUPLICATE |
      SYS_RIGHT_TRANSFER;
    expect("the same directory under a second name",
      run(SYS_OP_HANDLE_DUP, root, &dup, sizeof(dup)), SYS_OK);
    ns[2].id = (sys_ns_id_t)(SYS_NS_PRIVATE + 0);
    ns[2].handle = dup.out_handle;
    strncpy_ns(ns[2].name, "/files");

    /*
     * And both halves of the union, under one name. Section 20.4 makes
     * this a library feature needing no kernel involvement, and from
     * here that is visible as there being nothing to do: two entries,
     * one name, written in the order they are to be tried.
     */
    ns[3].id = (sys_ns_id_t)(SYS_NS_PRIVATE + 1);
    ns[3].handle = u_first;
    strncpy_ns(ns[3].name, "/u");
    ns[4].id = (sys_ns_id_t)(SYS_NS_PRIVATE + 2);
    ns[4].handle = u_second;
    strncpy_ns(ns[4].name, "/u");



    sched_defer();
    child_watch(&c, proc_spawn_image("sh", 20, ns, 5, NULL, 0));
    sched_undefer();

    /*
     * As long as a person is willing to sit there. This is the one child
     * in the system whose running time is somebody else's to decide.
     */
    code = child_join(&c, 300000);
    cnprintf("    the shell exited with status %d\n", code);
    if (code != 0) {
        panic("sh: the shell ended with status %d rather than being told to",
          code);
    }
}
/*
 * The console as a service, per section 19 and section 25.
 *
 * The kernel is the launcher here, which is what root will be: it creates
 * the channel, hands the receive end to the driver along with the
 * interrupt, and keeps a connection to send on. Section 24.5 has exactly
 * this shape, and the readiness protocol it describes is the one this
 * needs, which is none: a client that sends first blocks in the rendezvous
 * until the server receives.
 *
 * Two reads, because a console has two paths and a filesystem has
 * neither. The first finds a line already waiting, which is what a
 * scripted session always produces, since a pipe delivers the whole
 * session before anything asks for any of it. The second arrives at an
 * empty ring and is held rather than refused, and the driver's other
 * thread answers it when the line arrives. Only the second needs a person
 * or a delay, and it is the one that would otherwise never be exercised.
 */
static void
console_write(sys_handle_t conn, const char *s, uint32_t len)
{
    struct sys_op_send_args tx;
    fs_request_t *req;

    memset(&tx, 0, sizeof(tx));
    req = (fs_request_t *)tx.request.inline_data;
    req->fr_op = FS_OP_WRITE;
    req->fr_length = len;
    memcpy(&tx.request.inline_data[sizeof(*req)], s, len);
    tx.request.inline_len = sizeof(*req) + len;

    if (run(SYS_OP_IPC_SEND, conn, &tx, sizeof(tx)) != SYS_OK) {
        panic("console: a write failed");
    }
}

static uint32_t
console_read(sys_handle_t conn, char *out, uint32_t max)
{
    struct sys_op_send_args tx;
    fs_request_t *req;
    fs_reply_t *rep;
    uint32_t n;

    memset(&tx, 0, sizeof(tx));
    req = (fs_request_t *)tx.request.inline_data;
    req->fr_op = FS_OP_READ;
    req->fr_length = max;
    tx.request.inline_len = sizeof(*req);

    if (run(SYS_OP_IPC_SEND, conn, &tx, sizeof(tx)) != SYS_OK) {
        panic("console: a read failed");
    }

    rep = (fs_reply_t *)tx.reply.inline_data;
    if (rep->fr_status != SYS_OK || rep->fr_length == 0) {
        panic("console: a read returned %u bytes with status %d",
          (unsigned int)rep->fr_length, rep->fr_status);
    }

    n = rep->fr_length;
    if (n >= max) {
        n = max - 1;
    }
    memcpy(out, &tx.reply.inline_data[sizeof(*rep)], n);
    out[n] = '\0';

    return n;
}

static void
console_service(void)
{
    struct proc *self = proc_current();
    struct sys_op_connect_args cargs;
    sys_ns_spec_t ns[4];
    struct channel *ch;
    sys_handle_t h_recv, conn, h_dev;
    struct grant *g_uart;
    uintptr_t regs, span;
    char line[SYS_IPC_INLINE_BYTES];
    sys_err_t err;

    ch = channel_create("console", self);
    if (ch == NULL) {
        panic("console: no channel");
    }
    err = handle_insert(&self->p_handles, ch, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &h_recv);
    expect("a receive handle for the console", err, SYS_OK);

    memset(&cargs, 0, sizeof(cargs));
    /*
     * Duplicable, because every program that talks to a person gets
     * its console this way and a handle moves rather than copies.
     * Section 7.4 has attenuation happen before a transfer, so what
     * this thread holds has to carry more than what it hands out.
     */
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_DUPLICATE |
      SYS_RIGHT_TRANSFER;
    expect("a connection to it, for this thread",
      run(SYS_OP_CHAN_CONNECT, h_recv, &cargs, sizeof(cargs)), SYS_OK);
    conn = cargs.out_handle;
    h_console = conn;

    /*
     * And a grant over the registers, per section 19.2. The authority to
     * make one is here rather than in the driver: what assembles the
     * system reads the platform description and turns it into spawn
     * calls, and this kernel is what assembles the system. The driver
     * holds no address of its own, so a copy of it started without this
     * reaches nothing.
     */
    md_console_regs(&regs, &span);
    g_uart = grant_create("console-regs", regs, span, true);
    if (g_uart == NULL) {
        panic("console: no grant for its registers");
    }
    err = handle_insert(&self->p_handles, g_uart, SYS_OBJ_GRANT,
      SYS_RIGHT_MAP | SYS_RIGHT_READ | SYS_RIGHT_WRITE |
      SYS_RIGHT_TRANSFER, &h_dev);
    expect("a grant over the console's registers", err, SYS_OK);

    /*
     * Four handles: what it serves on, the two halves of the
     * interrupt, and the device it drives. The interrupt pair are
     * private identifiers because an interrupt is not something that
     * means the same in every process, which is what section 20.4
     * reserves the range above SYS_NS_PRIVATE for.
     */
    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_SERVICE;
    ns[0].handle = h_recv;
    ns[1].id = (sys_ns_id_t)(SYS_NS_PRIVATE + 0);
    ns[1].handle = h_uart_irq;
    ns[2].id = (sys_ns_id_t)(SYS_NS_PRIVATE + 1);
    ns[2].handle = h_uart_fence;
    ns[3].id = SYS_NS_DEVICE;
    ns[3].handle = h_dev;

    /*
     * Tracing starts before the spawn and not after it. The driver
     * outranks this thread, so it runs, takes its interrupt, drains
     * whatever a pipe has already delivered and blocks, all inside
     * proc_spawn_image. A window that opened afterwards would miss every
     * transition this check is about.
     */
    trace_reset();
    trace_enable(true);

    (void)proc_spawn_image("console", 15, ns, 4, NULL, 0);

    console_write(conn, "\r\nline one: ", 12);
    (void)console_read(conn, line, sizeof(line));
    cnprintf("    first line, from the ring: %s", line);

    console_write(conn, "line two: ", 10);
    (void)console_read(conn, line, sizeof(line));
    cnprintf("    second line, held until it arrived: %s", line);

    trace_enable(false);
    trace_report("console");

    /*
     * Both of the driver's threads blocked, on different objects,
     * which is the arrangement section 19.3 prescribes and the reason
     * this milestone is measuring what threads cost. Without this the
     * stage passes on a driver that never waited for anything.
     */
    if (!trace_saw(THREAD_WAIT_RECV)) {
        panic("console: its request thread never blocked in receive");
    }
    if (!trace_saw(THREAD_WAIT_FENCE)) {
        panic("console: its receive thread never waited on the interrupt");
    }

    greet_checks(conn);
}
static void
fs_server(void *arg)
{
    struct sys_op_connect_args cargs;
    struct sys_op_reply_args tx;
    struct sys_op_recv_args rx;
    fs_request_t *req;
    fs_reply_t *rep;
    uint32_t node, n;

    (void)arg;

    for (;;) {
        memset(&rx, 0, sizeof(rx));
        if (run(SYS_OP_IPC_RECV, h_fs_recv, &rx, sizeof(rx)) != SYS_OK) {
            panic("fs: receive failed");
        }

        req = (fs_request_t *)rx.message.inline_data;
        node = (uint32_t)rx.cookie;

        memset(&tx, 0, sizeof(tx));
        tx.peer = rx.peer;
        rep = (fs_reply_t *)tx.reply.inline_data;
        rep->fr_status = SYS_ERR_NO_ENT;
        tx.reply.inline_len = sizeof(*rep);
        tx.reply.handle = SYS_ID_INVALID;

        switch (req->fr_op) {
        case FS_OP_WALK:
            for (n = 0; n < 2; n++) {
                if (fs_nodes[n].data != NULL &&
                  fs_nodes[n].name[0] == req->fr_name[0]) {
                    break;
                }
            }
            if (n < 2) {
                /*
                 * A new connection naming the node found, handed back in
                 * the message. The client could not have constructed this
                 * itself, which is the point of it being a handle.
                 */
                memset(&cargs, 0, sizeof(cargs));
                cargs.cookie = n;
                cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
                if (run(SYS_OP_CHAN_CONNECT, h_fs_recv, &cargs,
                  sizeof(cargs)) == SYS_OK) {
                    tx.reply.handle = cargs.out_handle;
                    rep->fr_status = SYS_OK;
                    rep->fr_size = fs_nodes[n].len;
                }
            }
            break;

        case FS_OP_READ:
            if (node < 2 && fs_nodes[node].data != NULL) {
                uint64_t off = req->fr_offset;
                uint32_t avail = 0;

                if (off < fs_nodes[node].len) {
                    avail = fs_nodes[node].len - (uint32_t)off;
                }
                if (avail > req->fr_length) {
                    avail = req->fr_length;
                }
                if (avail > SYS_IPC_INLINE_BYTES - sizeof(*rep)) {
                    avail = SYS_IPC_INLINE_BYTES - sizeof(*rep);
                }

                memcpy(&tx.reply.inline_data[sizeof(*rep)],
                  &fs_nodes[node].data[off], avail);
                rep->fr_length = avail;
                rep->fr_status = SYS_OK;
                tx.reply.inline_len = sizeof(*rep) + avail;
            }
            break;

        case FS_OP_CLOSE:
            rep->fr_status = SYS_OK;
            break;

        default:
            rep->fr_status = SYS_ERR_INVALID_ARG;
            break;
        }


        /*
         * Section 12.4: the same answer where the kernel can read it.
         * A refusal that lives only in the payload is a successful
         * send as far as the kernel is concerned, and a chain linked
         * behind it carries on.
         */
        tx.reply.status = rep->fr_status;

        if (run(SYS_OP_IPC_REPLY, 0, &tx, sizeof(tx)) != SYS_OK) {
            panic("fs: reply failed");
        }
    }
}

static void
fs_client(void *arg)
{
    const sys_startup_t *startup = arg;
    struct sys_op_send_args tx;
    fs_request_t *req;
    fs_reply_t *rep;
    sys_handle_t dir, file;
    char buf[SYS_IPC_INLINE_BYTES];

    if (startup->version != SYS_STARTUP_VERSION) {
        panic("client: startup record from a kernel it does not know");
    }
    dir = sys_ns_find((const sys_handle_t *)startup->ns, startup->ns_words,
      SYS_NS_ROOT);
    if (dir == SYS_HANDLE_INVALID) {
        panic("client: no root directory in the namespace it was given");
    }
    cnputs("    client: spawned holding one handle, and no root to name\n");

    /* Walk the directory to the file. */
    memset(&tx, 0, sizeof(tx));
    req = (fs_request_t *)tx.request.inline_data;
    req->fr_op = FS_OP_WALK;
    req->fr_name[0] = 'm';
    tx.request.inline_len = sizeof(*req);

    if (run(SYS_OP_IPC_SEND, dir, &tx, sizeof(tx)) != SYS_OK) {
        panic("client: walk failed");
    }
    rep = (fs_reply_t *)tx.reply.inline_data;
    if (rep->fr_status != SYS_OK) {
        panic("client: walk refused");
    }
    file = tx.reply.handle;
    cnprintf("    client: walked to a %lu byte file\n",
      (unsigned long)rep->fr_size);

    /* Read it. */
    memset(&tx, 0, sizeof(tx));
    req = (fs_request_t *)tx.request.inline_data;
    req->fr_op = FS_OP_READ;
    req->fr_offset = 0;
    req->fr_length = sizeof(buf);
    tx.request.inline_len = sizeof(*req);

    if (run(SYS_OP_IPC_SEND, file, &tx, sizeof(tx)) != SYS_OK) {
        panic("client: read failed");
    }
    rep = (fs_reply_t *)tx.reply.inline_data;
    if (rep->fr_status != SYS_OK) {
        panic("client: read refused");
    }

    memcpy(buf, &tx.reply.inline_data[sizeof(*rep)], rep->fr_length);
    buf[rep->fr_length] = '\0';
    cnprintf("    client read: %s", buf);

    thread_exit(0);
}

static void
slice_checks(void)
{
    struct proc *p = proc_current();
    struct sys_op_connect_args cargs;
    sys_ns_spec_t ns[1];
    struct thread *srv;
    struct child c;
    sys_handle_t root;
    sys_err_t err;

    ch_fs = channel_create("fs", p);
    if (ch_fs == NULL) {
        panic("slice: no channel");
    }
    err = handle_insert(&p->p_handles, ch_fs, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV, &h_fs_recv);
    expect("a receive handle for the filesystem", err, SYS_OK);

    srv = thread_create("fs-server", 15, fs_server, NULL);
    sched_ready(srv);

    /* A connection naming the root directory, to hand to the client. */
    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = FS_NODE_ROOT;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a connection to the root directory",
      run(SYS_OP_CHAN_CONNECT, h_fs_recv, &cargs, sizeof(cargs)), SYS_OK);
    root = cargs.out_handle;

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_ROOT;
    ns[0].handle = root;
    cnputs("    spawning a client process with that one handle\n");
    sched_defer();
    child_watch(&c, proc_spawn("fs-client", 20, fs_client, ns, 1));
    sched_undefer();

    (void)child_join(&c, JOIN_MS);

    expect("the handle left this process when it was handed over",
      handle_lookup(&p->p_handles, root, SYS_OBJ_NONE, 0, NULL),
      SYS_ERR_STALE);
}

/*
 * A three-level chain, to check the claim that transitivity falls out of
 * each hop sending at its own effective priority rather than needing a
 * walk. A client at 25 reaches a server at 10 which calls a server at 5;
 * if the claim holds, all three run at 25.
 */
static struct channel *ch_mid;
static struct channel *ch_low;
static sys_handle_t h_mid_recv, h_mid_send, h_low_recv, h_low_send;
static uint8_t deep_prio_seen;
static bool chain_done;

static void
deep_server(void *arg)
{
    struct sys_op_reply_args tx;
    struct sys_op_recv_args rx;

    (void)arg;

    memset(&rx, 0, sizeof(rx));
    if (run(SYS_OP_IPC_RECV, h_low_recv, &rx, sizeof(rx)) != SYS_OK) {
        panic("deep: receive failed");
    }
    deep_prio_seen = thread_current()->t_prio;

    memset(&tx, 0, sizeof(tx));
    tx.peer = rx.peer;
    if (run(SYS_OP_IPC_REPLY, 0, &tx, sizeof(tx)) != SYS_OK) {
        panic("deep: reply failed");
    }
    thread_exit(0);
}

static void
mid_server(void *arg)
{
    struct sys_op_reply_args tx;
    struct sys_op_recv_args rx;
    struct sys_op_send_args fwd;

    (void)arg;

    memset(&rx, 0, sizeof(rx));
    if (run(SYS_OP_IPC_RECV, h_mid_recv, &rx, sizeof(rx)) != SYS_OK) {
        panic("mid: receive failed");
    }
    cnprintf("    mid server (base 10) running at %u\n",
      thread_current()->t_prio);

    memset(&fwd, 0, sizeof(fwd));
    if (run(SYS_OP_IPC_SEND, h_low_send, &fwd, sizeof(fwd)) != SYS_OK) {
        panic("mid: forward failed");
    }

    memset(&tx, 0, sizeof(tx));
    tx.peer = rx.peer;
    if (run(SYS_OP_IPC_REPLY, 0, &tx, sizeof(tx)) != SYS_OK) {
        panic("mid: reply failed");
    }
    thread_exit(0);
}

static void
chain_client(void *arg)
{
    struct sys_op_send_args tx;

    (void)arg;

    memset(&tx, 0, sizeof(tx));
    if (run(SYS_OP_IPC_SEND, h_mid_send, &tx, sizeof(tx)) != SYS_OK) {
        panic("chain client: send failed");
    }
    chain_done = true;
    thread_exit(0);
}

static void
chain_checks(void)
{
    struct proc *p = proc_current();

    ch_mid = channel_create("mid", p);
    ch_low = channel_create("low", p);
    if (ch_mid == NULL || ch_low == NULL) {
        panic("chain: no channel");
    }
    /*
     * Checked rather than discarded. These are file-static and so start
     * at SYS_ID_INVALID, which means a failed insert would not be
     * undefined; it would surface much later as a stale handle in a
     * server thread, a long way from the line that actually failed.
     */
    expect("a receive handle on the mid channel",
      handle_insert(&p->p_handles, ch_mid, SYS_OBJ_CHANNEL,
        SYS_RIGHT_RECV, &h_mid_recv), SYS_OK);
    expect("a send handle on the mid channel",
      handle_insert(&p->p_handles, ch_mid, SYS_OBJ_CHANNEL,
        SYS_RIGHT_SEND, &h_mid_send), SYS_OK);
    expect("a receive handle on the low channel",
      handle_insert(&p->p_handles, ch_low, SYS_OBJ_CHANNEL,
        SYS_RIGHT_RECV, &h_low_recv), SYS_OK);
    expect("a send handle on the low channel",
      handle_insert(&p->p_handles, ch_low, SYS_OBJ_CHANNEL,
        SYS_RIGHT_SEND, &h_low_send), SYS_OK);

    sched_ready(thread_create("deep", 5, deep_server, NULL));
    sched_ready(thread_create("mid", 10, mid_server, NULL));
    sched_ready(thread_create("chain-client", 25, chain_client, NULL));

    while (!chain_done) {
        sched_yield();
    }

    cnprintf("    deep server (base 5) ran at %u\n", deep_prio_seen);
    if (deep_prio_seen != 25) {
        panic("chain: donation did not reach the far end (%u)",
          deep_prio_seen);
    }
}

/*
 * Run one client program to completion and report what it cost.
 *
 * Rendezvous and connection-pool high water are both deterministic, which
 * is why they are the measurement rather than elapsed time: section 25.7's
 * claim is about round trips and about not touching a statically sized
 * pool, and under emulation a clock would answer a question nobody asked.
 *
 * The trace is the other half. A demonstration where one program runs to
 * completion before the other starts exercises no blocking at all and
 * produces byte counts that look exactly the same, so the states each
 * thread passed through are checked rather than assumed.
 */
/*
 * What moving a buffer costs at each tier, per section 18.
 *
 * Two entries per page in each half of the move, one invalidation per
 * page in each half, and no whole-TLB flush at all. A crossing is both
 * halves, so a one page buffer crossing one boundary is four entries
 * and two invalidations, and nothing thrown away that had not moved.
 *
 * Stated per half because that is the shape the code has. The whole-TLB
 * count is kept and asserted at zero rather than deleted, because a
 * flush put back here would vanish into a system that already flushes
 * on every address space switch.
 *
 * Checked against what the ports actually wrote, not recorded as a
 * total, so it holds for any workload rather than for this one.
 */
#if SYS_TIER == 2
#define ENTRIES_PER_PAGE    2
#define INVALS_PER_PAGE     1
#define FLUSHES_PER_HALF    0
#else
#define ENTRIES_PER_PAGE    0
#define INVALS_PER_PAGE     0
#define FLUSHES_PER_HALF    0
#endif

static void
boundary_rate(const char *what, const struct vm_cost *c)
{
    if (c->vc_leaf != c->vc_pages * ENTRIES_PER_PAGE) {
        panic("%s: %lu pages moved cost %lu entries, not %lu", what,
          (unsigned long)c->vc_pages, (unsigned long)c->vc_leaf,
          (unsigned long)(c->vc_pages * ENTRIES_PER_PAGE));
    }
    if (c->vc_inval != c->vc_pages * INVALS_PER_PAGE) {
        panic("%s: %lu pages moved cost %lu invalidations, not %lu", what,
          (unsigned long)c->vc_pages, (unsigned long)c->vc_inval,
          (unsigned long)(c->vc_pages * INVALS_PER_PAGE));
    }

    /*
     * And nothing throws the whole cache away any more. This is a
     * zero the check has to keep asserting rather than a line that
     * could be deleted: a flush reintroduced here would be invisible
     * in a total that already had thousands of them from address
     * space switches.
     */
    if (c->vc_flush != c->vc_halves * FLUSHES_PER_HALF) {
        panic("%s: %lu halves cost %lu whole-TLB flushes, not %lu", what,
          (unsigned long)c->vc_halves, (unsigned long)c->vc_flush,
          (unsigned long)(c->vc_halves * FLUSHES_PER_HALF));
    }
}

#define SEND_BLOCK_NO       0
#define SEND_BLOCK_YES      1
#define SEND_BLOCK_EITHER   2
/*
 * Whole names, because the three differ in one character and keying on
 * its position would be a table that silently matches the wrong row
 * the day a fourth client is added.
 */
static bool
same_name(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }

    return *a == *b;
}

/*
 * What each of the three clients costs, asserted rather than printed.
 *
 * These are counts. They are the same on both ports, at both tiers and
 * in every run, which is what makes them a property of this kernel and
 * of these three programs rather than of the machine underneath. M10
 * settled the rule and the reason: a headline figure that can drift
 * without anything failing is a figure nobody is keeping, and the
 * numbers here are this milestone's product.
 *
 * The entries and invalidations on the same line are not in the table.
 * Those are zero at Tier 0, where there is one address space and a
 * buffer that arrives is already where it needs to be, so they are a
 * property of the tier and are reported rather than asserted.
 */
static const struct {
    const char *cc_name;
    uint64_t    cc_submits;
    uint64_t    cc_checks;
    uint64_t    cc_rounds;
    uint64_t    cc_conns;
} client_cost[] = {
    /*
     * Walk, read and close as three calls, plus the close of the
     * handle and one submission of output.
     */
    { "fs_client", 5, 15, 5, 1 },

    /*
     * The compound request of section 25.7: one message asks for all
     * three, so two round trips go and the server never draws a
     * connection from its pool.
     */
    { "fs_batch",  4, 12, 3, 0 },

    /*
     * The same three requests as fs_client, in one submission, with
     * the connection carried between them in a register.
     */
    { "fs_chain",  2, 12, 5, 1 },
};

static void
client_cost_check(const char *image, const struct proc *p, uint64_t rounds,
  uint64_t conns)
{
    unsigned int i;

    for (i = 0; i < sizeof(client_cost) / sizeof(client_cost[0]); i++) {
        if (!same_name(client_cost[i].cc_name, image)) {
            continue;
        }

        if (p->p_submits != client_cost[i].cc_submits ||
          p->p_checks != client_cost[i].cc_checks ||
          rounds != client_cost[i].cc_rounds ||
          conns != client_cost[i].cc_conns) {
            panic("%s: %lu submissions, %lu checks, %lu rendezvous, "
              "%lu connections, against %lu, %lu, %lu, %lu",
              image, (unsigned long)p->p_submits,
              (unsigned long)p->p_checks, (unsigned long)rounds,
              (unsigned long)conns,
              (unsigned long)client_cost[i].cc_submits,
              (unsigned long)client_cost[i].cc_checks,
              (unsigned long)client_cost[i].cc_rounds,
              (unsigned long)client_cost[i].cc_conns);
        }
        return;
    }

    /*
     * A client with no row asserts nothing, which is the shape of
     * check this project keeps finding: it passes because it did not
     * look. Every program run through here is one whose cost is the
     * point, so a name that is not in the table is a table that needs
     * a row rather than a client that needs excusing.
     */
    panic("%s: run as a client with no cost recorded for it", image);
}


static void
run_client(const char *image, sys_handle_t root, uint32_t expect_bytes,
  int expect_send_block)
{
    struct pool *conns = connection_pool();
    sys_ns_spec_t ns[1];
    struct vm_cost cost;
    struct proc *p;
    struct child c;
    int32_t code;
    uint64_t before = channel_rendezvous;
    uint32_t pool_before;

    vm_cost_read(&cost);
    pool_highwater_reset(conns);
    pool_before = conns->highwater;

    trace_reset();
    trace_enable(true);

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_ROOT;
    ns[0].handle = root;

    /*
     * Spawning and watching are one operation. The child outranks this
     * thread, so without the deferral it runs and exits inside
     * proc_spawn_image, before anything holds its completion timeline,
     * and halts the system for an exit nobody had claimed.
     */
    sched_defer();
    p = proc_spawn_image(image, 20, ns, 1, NULL, 0);
    child_watch(&c, p);
    sched_undefer();

    code = child_join(&c, JOIN_MS);

    trace_enable(false);

    {
        struct vm_cost after;
        uint64_t rounds = channel_rendezvous - before;
        uint64_t leaf, flush;

        vm_cost_read(&after);
        leaf = after.vc_leaf - cost.vc_leaf;
        flush = after.vc_flush - cost.vc_flush;

        cnprintf("    %-10s %lu submissions, %lu checks, %lu rendezvous, "
          "%lu connections, %lu entries, %lu invalidations\n", image,
          (unsigned long)p->p_submits,
          (unsigned long)p->p_checks,
          (unsigned long)rounds,
          (unsigned long)(conns->highwater - pool_before),
          (unsigned long)leaf,
          (unsigned long)(after.vc_inval - cost.vc_inval));

        /*
         * The rate over this run alone, against what was asked for
         * rather than against how many messages there were. The
         * earlier version multiplied by the rendezvous count, which
         * held only because every message here happens to carry one
         * page, and would have been checking the workload rather than
         * the mechanism the day one did not.
         *
         * At Tier 0 it is nothing at all, which is not the check
         * being skipped. There is one address space and a buffer that
         * arrives is already where it needs to be, so the whole of
         * this is what the other tier pays to keep programs apart.
         */
        {
            struct vm_cost run;

            run.vc_leaf = leaf;
            run.vc_flush = flush;
            run.vc_inval = after.vc_inval - cost.vc_inval;
            run.vc_table = after.vc_table - cost.vc_table;
            run.vc_pages = after.vc_pages - cost.vc_pages;
            run.vc_halves = after.vc_halves - cost.vc_halves;
            boundary_rate(image, &run);
        }

        client_cost_check(image, p, rounds,
          conns->highwater - pool_before);
    }
    trace_report(image);

    if (code != (int32_t)expect_bytes) {
        panic("%s: read %d bytes, expected %u", image, code,
          expect_bytes);
    }

    /*
     * Both sides must have blocked on each other. Without this the stage
     * passes on a run where the server happened to be waiting every time,
     * which never queues a sender and never proves the client can wait for
     * capacity as opposed to for work.
     */
    if (!trace_saw(THREAD_WAIT_RECV)) {
        panic("%s: the server never blocked in receive", image);
    }
    if (!trace_saw(THREAD_WAIT_REPLY)) {
        panic("%s: the client never waited for a reply", image);
    }

    /*
     * Whether the client ever queued behind a busy server is not the same
     * question, and the two forms answer it differently. Three messages
     * give the server two chances to be elsewhere when the next one
     * arrives, and it is both times. One message cannot queue at all: the
     * server is waiting when it arrives, because the client has not given
     * it anything else to do. Fewer sends is fewer chances to wait for
     * capacity, which is a saving section 25.7 does not mention.
     *
     * About this client and not about the system, since the block store
     * arrived. The file server sends onward for every block and queues
     * behind the store when it does, so asking whether anything at all
     * send-blocked now answers yes whatever the client did.
     */
    if (expect_send_block == SEND_BLOCK_EITHER) {
        /*
         * Not asserted, and that is the result rather than a gap. Two
         * cores make progress independently, so whether the client
         * finds its server already at the rendezvous is a question
         * about which core got there first. Asserting either answer
         * would be asserting a race.
         */
        cnprintf("    and it %s send-block\n",
          trace_saw_thread(image, THREAD_WAIT_SEND) ? "did" : "did not");
    } else if (trace_saw_thread(image, THREAD_WAIT_SEND) !=
      (expect_send_block == SEND_BLOCK_YES)) {
        panic("%s: send-blocking was %sexpected and %shappened", image,
          expect_send_block == SEND_BLOCK_YES ? "" : "un",
          expect_send_block == SEND_BLOCK_YES ? "never " : "");
    }
}

/*
 * The same slice again, with both halves as loaded programs, and then the
 * same read a second way to answer section 25.7.
 *
 * Stage 8 proved the protocol and the rendezvous with kernel threads
 * standing in for processes. What it could not reach is the boundary: the
 * trap edge, the startup record as a program reads it, and a scheduler
 * blocking and resuming threads that are in user mode when they block.
 *
 * The programs cannot print. There is no console for user code, so each
 * client reports what it read as its exit status and the kernel says so.
 */

/*
 * A program that names memory it does not own, and one image with two
 * live processes. The two halves of what an address space per process
 * buys, checked from the outside.
 */
static void
trespass_checks(sys_handle_t root)
{
    sys_ns_spec_t ns[1];
    struct child c;
    int32_t code;

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_ROOT;
    ns[0].handle = root;

    sched_defer();
    child_watch(&c, proc_spawn_image("trespass", 20, ns, 1, NULL, 0));
    sched_undefer();

    code = child_join(&c, JOIN_MS);

    /*
     * Zero means it was refused and carried on. Any other value is the
     * step it got to: the refusal it did not get, or the ordinary read
     * that should have worked and did not.
     */
    if (code != 0) {
        panic("trespass: exited %d, so the check it stopped at is the "
          "one that failed", code);
    }
    cnprintf("    %-38s %s\n", "a buffer outside the caller's memory",
      "REFUSED");
}

/*
 * Two processes from one image.
 *
 * At Tier 0 a program runs at the address it was linked for, so a second
 * copy would write over the first. The kernel refuses, and that refusal
 * is the cost of having no translation. At Tier 2 the address is virtual
 * and the memory behind it is frames nobody else has, so the same request
 * succeeds. Nothing about the request differs between the two: the same
 * source, the same archive, the same operation, and a different answer
 * because the machine underneath can do a different thing.
 *
 * Both are spawned below this thread's priority so that neither runs
 * until both exist. Otherwise the first would exit before the second was
 * asked for and there would be nothing to refuse.
 */
static void
two_copies_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    sys_handle_t h1 = SYS_HANDLE_INVALID, h2 = SYS_HANDLE_INVALID;
    struct child c1, c2;
    void *obj;
    sys_err_t err;

    err = proc_spawn_op(self, "status", PRIO_BOOT - 1, NULL, 0, NULL, 0,
      &h1);
    expect("one process from an image", err, SYS_OK);

    err = proc_spawn_op(self, "status", PRIO_BOOT - 1, NULL, 0, NULL, 0,
      &h2);
#if SYS_TIER == 2
    expect("a second, from the same image", err, SYS_OK);
#else
    expect("a second, from the same image, refused", err,
      SYS_ERR_WOULDBLOCK);
#endif

    if (handle_lookup(ht, h1, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
        panic("two copies: the first spawn returned no process");
    }
    child_watch(&c1, obj);
#if SYS_TIER == 2
    if (handle_lookup(ht, h2, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
        panic("two copies: the second spawn returned no process");
    }
    child_watch(&c2, obj);
#endif

    if (child_join(&c1, JOIN_MS) != 7) {
        panic("two copies: the first did not run to its own exit");
    }
    handle_close(ht, h1);
#if SYS_TIER == 2
    if (child_join(&c2, JOIN_MS) != 7) {
        panic("two copies: the second did not run to its own exit");
    }
    handle_close(ht, h2);
    cnprintf("    %-38s %s\n", "both ran, in memory of their own", "OK");
#else
    (void)c2;
    cnprintf("    %-38s %s\n", "one address, so one of it", "OK");
#endif
}

/*
 * A reply that fails on its way across, and a payload that arrives
 * anyway.
 *
 * Section 7.4 has a handle travel by move, and a move can fail: the
 * server may name a handle it does not hold. The reply still happened
 * and the bytes in it are still what the server wrote, so both sides
 * learn the transfer failed and the client gets what it was sent.
 *
 * This exists because the first version of the staged reply path
 * delivered the payload only when the reply reported success, and
 * nothing in the system noticed. A client asking for a file and being
 * given an error with an empty message instead of the status the server
 * wrote is the shape of the bug that would have followed.
 */
static struct channel *ch_half;
static sys_handle_t h_half_recv;

static void
half_server(void *arg)
{
    struct sys_op_recv_args rx;
    struct sys_op_reply_args tx;

    (void)arg;

    memset(&rx, 0, sizeof(rx));
    if (run(SYS_OP_IPC_RECV, h_half_recv, &rx, sizeof(rx)) != SYS_OK) {
        panic("half: the receive failed");
    }

    memset(&tx, 0, sizeof(tx));
    tx.peer = rx.peer;
    tx.reply.inline_len = 5;
    memcpy(tx.reply.inline_data, "half", 5);

    /* A handle this process does not hold, so the move cannot succeed. */
    tx.reply.handle = (sys_handle_t)0x1234;

    if (run(SYS_OP_IPC_REPLY, 0, &tx, sizeof(tx)) == SYS_OK) {
        panic("half: a handle nobody holds moved");
    }

    thread_exit(0);
}

static void
reply_error_checks(void)
{
    struct proc *p = proc_current();
    struct sys_op_send_args tx;
    sys_handle_t send_h;
    struct thread *t;
    sys_err_t err;

    ch_half = channel_create("half", p);
    if (ch_half == NULL) {
        panic("half: no channel");
    }
    err = handle_insert(&p->p_handles, ch_half, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV, &h_half_recv);
    expect("a channel for a reply that half fails", err, SYS_OK);
    err = handle_insert(&p->p_handles, ch_half, SYS_OBJ_CHANNEL,
      SYS_RIGHT_SEND, &send_h);
    expect("a send handle on it", err, SYS_OK);

    t = thread_create("half-server", PRIO_BOOT - 1, half_server, NULL);
    sched_ready(t);

    memset(&tx, 0, sizeof(tx));
    tx.request.inline_len = 1;
    err = run(SYS_OP_IPC_SEND, send_h, &tx, sizeof(tx));
    expect("a reply whose handle could not move", err, SYS_ERR_STALE);

    if (tx.reply.inline_data[0] != 'h' ||
      tx.reply.inline_data[3] != 'f') {
        panic("ipc: the failed handle took the reply payload with it");
    }
    cnprintf("    %-38s %s\n", "and the payload arrived regardless",
      (const char *)tx.reply.inline_data);

    handle_close(&p->p_handles, send_h);
}

/*
 * The kernel as a client of a program, with a buffer of its own.
 *
 * It is refused, and that is the answer rather than a gap. Kernel memory
 * is in every address space already, so there is nothing to hand over,
 * and it is in none of them reachable from user mode, so a program given
 * the address would fault on it. What the kernel would need to lend a
 * program a page is the grant of section 19.2, which is a handle rather
 * than an address, and which stage 7 builds.
 *
 * Both tiers refuse. Tier 0 has one address space and the read would
 * work, and letting it work there would put a difference between the
 * tiers into the interface rather than into the enforcement.
 *
 * The first version of this stage skipped the hand-over for kernel
 * memory instead of refusing it, on the grounds that the kernel is
 * mapped everywhere. It is, without the bit that lets user mode touch
 * it, and the file server faulted on the first byte it wrote.
 */
static void
kernel_buffer_checks(sys_handle_t root)
{
    static char into[64];
    struct sys_op_send_args tx;
    fs_request_t *req;
    sys_handle_t file;
    sys_err_t err;

    /* Walk to the file, as any client would. This much works. */
    memset(&tx, 0, sizeof(tx));
    tx.request.handle = SYS_HANDLE_INVALID;
    tx.request.inline_len = sizeof(*req);
    req = (fs_request_t *)tx.request.inline_data;
    req->fr_op = FS_OP_WALK;
    memcpy(req->fr_name, "motd", sizeof("motd"));
    err = run(SYS_OP_IPC_SEND, root, &tx, sizeof(tx));
    expect("the kernel walking to a file a program serves", err, SYS_OK);
    file = tx.reply.handle;

    /* The read does not, because the buffer is the kernel's. */
    memset(&tx, 0, sizeof(tx));
    tx.request.handle = SYS_HANDLE_INVALID;
    tx.request.inline_len = sizeof(*req);
    tx.request.buf = (uintptr_t)into;
    tx.request.buf_len = sizeof(into);
    tx.request.buf_flags = SYS_BUF_WRITE;

    req = (fs_request_t *)tx.request.inline_data;
    req->fr_op = FS_OP_READ;
    req->fr_offset = 0;
    req->fr_length = sizeof(into);

    err = run(SYS_OP_IPC_SEND, file, &tx, sizeof(tx));
    expect("and reading it into kernel memory", err, SYS_ERR_FAULT);

    if (into[0] != 0) {
        panic("kernel buffer: refused and written to anyway");
    }

    memset(&tx, 0, sizeof(tx));
    tx.request.handle = SYS_HANDLE_INVALID;
    tx.request.inline_len = sizeof(*req);
    req = (fs_request_t *)tx.request.inline_data;
    req->fr_op = FS_OP_CLOSE;
    (void)run(SYS_OP_IPC_SEND, file, &tx, sizeof(tx));
    handle_close(&proc_current()->p_handles, file);
}

/*
 * A program that touches memory that is not its own, and what happens
 * to it.
 *
 * The same image, the same instruction, the same address, on two tiers.
 * At Tier 0 the load succeeds and the program prints the word it found,
 * because there is one address space and nothing between a program and
 * the kernel's first instruction. At Tier 2 the hardware refuses, the
 * thread ends with the kind, the address and the instruction in its
 * status, and everything else goes on running.
 *
 * This is section 6.1's argument in two runs of one program, and it is
 * what stage 3 saw and could not check: a fault stopped the machine
 * then, so the difference could be observed and not survived.
 */
static void
fault_checks(void)
{
    struct sys_op_status_args sargs;
    struct handle_table *ht = &proc_current()->p_handles;
    const char *argv[2];
    char addr[19];
    sys_handle_t h;
    struct child c;
    uintptr_t kernel, rx_end, kend;
    void *obj;

    /*
     * The kernel's first instruction. Nothing about it matters except
     * that it is not the program's: it is the address the review of
     * stage 3 used, and the same one the mapping made read-only. The
     * other two ends of the image are taken because the port writes all
     * three and has nowhere to put one it is not given.
     */
    /*
     * Before anything is made to fault: the mapping is a pure
     * function and every row of it can be asked about here.
     */
    md_fault_check();

    md_kernel_image(&kernel, &rx_end, &kend);
    (void)rx_end;
    (void)kend;

    hex_into(addr, kernel);

    argv[0] = "touch_addr";
    argv[1] = addr;

    sched_defer();
    if (proc_spawn_op(proc_current(), "touch_addr", PRIO_BOOT - 1, NULL, 0,
      argv, 2, &h) != SYS_OK) {
        panic("fault: could not start the program");
    }
    if (handle_lookup(ht, h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
        panic("fault: the spawn returned no process");
    }
    child_watch(&c, obj);
    sched_undefer();

    (void)child_join(&c, JOIN_MS);

    memset(&sargs, 0, sizeof(sargs));
    if (run(SYS_OP_PROC_STATUS, h, &sargs, sizeof(sargs)) !=
      SYS_OK) {
        panic("fault: no status for it");
    }

#if SYS_TIER == 2
    if (sargs.out_fault != SYS_FAULT_ACCESS) {
        panic("fault: it read 0x%lx and lived, kind %u",
          (unsigned long)kernel, (unsigned int)sargs.out_fault);
    }
    if (sargs.out_fault_addr != kernel) {
        panic("fault: reported 0x%lx, touched 0x%lx",
          (unsigned long)sargs.out_fault_addr, (unsigned long)kernel);
    }
    /*
     * The instruction is the program's own, which is the part that makes
     * the report worth having: an address without the instruction that
     * reached it says where but not who.
     */
    if (!proc_owns(c.c_proc, sargs.out_fault_pc, 1)) {
        panic("fault: pc 0x%lx is not in the program",
          (unsigned long)sargs.out_fault_pc);
    }
    cnprintf("    %-38s %s at 0x%lx\n", "reading the kernel, it died of",
      fault_name(sargs.out_fault), (unsigned long)sargs.out_fault_addr);
    cnprintf("    %-38s 0x%lx\n", "on its own instruction at",
      (unsigned long)sargs.out_fault_pc);
#else
    if (sargs.out_fault != SYS_FAULT_NONE) {
        panic("fault: it died of %u reading 0x%lx, at this tier",
          (unsigned int)sargs.out_fault, (unsigned long)kernel);
    }

    /*
     * It read the kernel, and this is what was there. Comparing the
     * status with the same word read from here is what makes this a
     * check rather than an observation that a program exited: an exit
     * code of zero would have been produced by a program that never ran
     * the load at all.
     */
    if (sargs.out_code != (int32_t)(*(volatile const uint32_t *)kernel)) {
        panic("fault: it read 0x%x where the kernel has 0x%x",
          (unsigned int)sargs.out_code,
          (unsigned int)(*(volatile const uint32_t *)kernel));
    }
    cnprintf("    %-38s 0x%x\n", "reading the kernel, it got back",
      (unsigned int)sargs.out_code);
#endif

    handle_close(ht, h);
}

/*
 * The move contract, enforced or trusted, from one program.
 *
 * Section 6.1's rule is that a sender loses its buffer between send and
 * reply. `grabber` breaks it with a second thread, which is the only way
 * it can be broken: the thread that sent the message is blocked and has
 * no instruction to execute in between.
 *
 * The kernel is the server here rather than the file server, because the
 * window has to be opened deliberately. This thread receives the
 * message, which is when the buffer leaves the program, then signals the
 * timeline the second thread is waiting on. That thread outranks this
 * one, so it runs before the signal returns: the write lands inside the
 * window every time rather than whenever the timer happens to fall.
 *
 * What the program reports is what happened to that thread, and the two
 * tiers answer differently from one source. This is the milestone's
 * thesis, and the first check here whose expected result is not the same
 * on both tiers.
 */
static void
move_checks(void)
{
    struct proc *self = proc_current();
    struct sys_op_connect_args cargs;
    struct sys_op_recv_args rx;
    struct sys_op_reply_args tx;
    sys_ns_spec_t ns[2];
    struct timeline *gate;
    struct channel *ch;
    sys_handle_t h_recv, h_gate;
    struct child c;
    int32_t code;
    sys_err_t err;

    ch = channel_create("move", self);
    gate = timeline_create("move-gate");
    if (ch == NULL || gate == NULL) {
        panic("move: no channel or no timeline");
    }

    err = handle_insert(&self->p_handles, ch, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV, &h_recv);
    expect("a channel the kernel serves", err, SYS_OK);

    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = 0;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a connection to it, for the grabber",
      run(SYS_OP_CHAN_CONNECT, h_recv, &cargs, sizeof(cargs)), SYS_OK);

    /*
     * The gate goes to the program with the right to wait and nothing
     * else. Signalling stays here, which is what makes the window this
     * thread's to open: a program that could signal its own gate could
     * open it before the send.
     */
    err = handle_insert(&self->p_handles, gate, SYS_OBJ_TIMELINE,
      SYS_RIGHT_WAIT | SYS_RIGHT_TRANSFER, &h_gate);
    expect("a gate for its second thread to wait on", err, SYS_OK);

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_SERVICE;
    ns[0].handle = cargs.out_handle;
    ns[1].id = SYS_NS_PRIVATE;
    ns[1].handle = h_gate;

    sched_defer();
    child_watch(&c, proc_spawn_image("grabber", 20, ns, 2, NULL, 0));
    sched_undefer();

    /*
     * The program is above this thread, so by the time this line runs it
     * has started its second thread, sent, and queued: there is a
     * message waiting and nothing else left to run.
     */
    memset(&rx, 0, sizeof(rx));
    if (run(SYS_OP_IPC_RECV, h_recv, &rx, sizeof(rx)) != SYS_OK) {
        panic("move: the receive failed");
    }

    if (timeline_signal(gate, 1) != SYS_OK) {
        panic("move: the gate would not open");
    }

    /*
     * And let it run. Opening the gate makes the second thread
     * runnable and nothing more: this thread keeps the core until it
     * gives it up, and if it went straight on to reply the buffer
     * would be back in the program before the thread that was
     * supposed to touch it ever executed. That version of this check
     * passed on both tiers and proved nothing.
     *
     * A yield is enough and is exact. This thread is the lowest
     * priority in the system, so the yield runs the other thread to
     * its end, whichever end that is.
     */
    sched_yield();

    memset(&tx, 0, sizeof(tx));
    tx.peer = rx.peer;
    tx.reply.handle = SYS_HANDLE_INVALID;
    if (run(SYS_OP_IPC_REPLY, 0, &tx, sizeof(tx)) != SYS_OK) {
        panic("move: the reply failed");
    }

    code = child_join(&c, JOIN_MS);

#if SYS_TIER == 2
    if (code != 1) {
        panic("move: grabber reported %d, expected the write to fault",
          code);
    }
    cnprintf("    %-38s %s\n", "a sender touching its own buffer",
      "FAULTED");
#else
    if (code != 0) {
        panic("move: grabber reported %d, expected the write to land",
          code);
    }
    cnprintf("    %-38s %s\n", "a sender touching its own buffer",
      "TRUSTED");
#endif

    handle_close(&self->p_handles, h_recv);
}

/*
 * A device is not reachable without the grant that covers it.
 *
 * Two halves, and the driver above is the first: it holds no address of
 * its own, asks the kernel where its grant is, and the console works. If
 * a grant were merely decorative that half would pass anyway, because
 * the registers were mapped into every process for three stages.
 *
 * The second half is a program reading the console's register page
 * without holding anything, and it runs at Tier 2 only. Not because
 * Tier 0 would pass, but because at Tier 0 it would succeed, and
 * succeeding means a program reading a live serial port's registers to
 * make a point about permission. Which register is safe to read differs
 * by board, one of them is a byte wide, and reading the wrong one
 * consumes an input byte or acknowledges an interrupt. The tier with
 * nothing to enforce is the tier where the demonstration costs
 * something and shows nothing.
 *
 * The third thing is the driver started without its grant, because the
 * failure that replaces is worse than a fault: a console that comes up,
 * serves requests and writes to nothing.
 */
static void
device_checks(void)
{
    uintptr_t regs, span;

    md_console_regs(&regs, &span);

#if SYS_TIER == 2
    {
        struct sys_op_status_args sargs;
        struct handle_table *ht = &proc_current()->p_handles;
        const char *argv[2];
        char addr[19];
        uintptr_t at = regs + span - 4;
        sys_handle_t h;
        struct child c;
        void *obj;

        hex_into(addr, at);

        argv[0] = "touch_addr";
        argv[1] = addr;

        sched_defer();
        if (proc_spawn_op(proc_current(), "touch_addr", PRIO_BOOT - 1,
          NULL, 0, argv, 2, &h) != SYS_OK) {
            panic("device: could not start the program");
        }
        if (handle_lookup(ht, h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
            panic("device: the spawn returned no process");
        }
        child_watch(&c, obj);
        sched_undefer();

        (void)child_join(&c, JOIN_MS);

        memset(&sargs, 0, sizeof(sargs));
        if (run(SYS_OP_PROC_STATUS, h, &sargs, sizeof(sargs)) != SYS_OK) {
            panic("device: no status for it");
        }
        if (sargs.out_fault != SYS_FAULT_ACCESS ||
          sargs.out_fault_addr != at) {
            panic("device: a program read 0x%lx without a grant, kind %u",
              (unsigned long)at, (unsigned int)sargs.out_fault);
        }
        cnprintf("    %-38s %s at 0x%lx\n", "a register without a grant",
          fault_name(sargs.out_fault),
          (unsigned long)sargs.out_fault_addr);
        handle_close(ht, h);

        /*
         * And the driver itself, started with nothing. It stops at the
         * lookup rather than reaching for an address, which is the
         * change this stage made to it.
         *
         * Only at this tier, because starting a second copy of a live
         * image is only possible here. At Tier 0 the same request is
         * refused for a different reason and would test the refusal.
         */
        sched_defer();
        if (proc_spawn_op(proc_current(), "console", PRIO_BOOT - 1, NULL,
          0, NULL, 0, &h) != SYS_OK) {
            panic("device: could not start a second driver");
        }
        if (handle_lookup(ht, h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
            panic("device: the second spawn returned no process");
        }
        child_watch(&c, obj);
        sched_undefer();

        if (child_join(&c, JOIN_MS) != -4) {
            panic("device: a driver with no grant did not say so");
        }
        cnprintf("    %-38s %s\n", "a driver started without one",
          "stopped, and said which");
        handle_close(ht, h);
    }
#else
    cnprintf("    %-38s 0x%lx\n", "the registers a grant covers",
      (unsigned long)regs);
    cnprintf("    %-38s %s\n", "and what stops a program reaching them",
      "nothing, at this tier");
#endif
}





/*
 * The frame count, read when nothing is moving it.
 *
 * A bracket that takes its baseline the instant a check begins includes
 * whatever the check before it left in flight, and what is most often
 * in flight is a reap: a program that exited a moment ago still holds
 * its slot until the next switch collects its last thread. Its frames
 * then come back inside the window and are counted against whatever the
 * window is named after. M14 stage two is what made that visible, by
 * making a process small enough that the boot reaches the next check
 * before the last one has finished letting go.
 *
 * Both the count and the number of processes waiting to be reaped have
 * to hold still, and the second is the one that matters: a parked
 * thread moves no frames, so a count alone reads as quiet right up to
 * the moment somebody else's memory comes back. Waiting for the number
 * to reach zero would not do, because a process whose handles are
 * deliberately held never gets reaped at all.
 *
 * Settled rather than delayed: same over several reads with a yield
 * between them is a statement about the system being quiet and not
 * about how long that took. The bound is a deadline because a count
 * that never settles is a system doing something, and saying so beats
 * waiting for ever.
 */
static uint32_t
frames_quiet(void)
{
    sys_time_t deadline = time_now() + time_from_ms(JOIN_MS);
    uint32_t last = page_used();
    uint32_t pending = proc_exited_unreaped();
    uint32_t same = 0;

    while (same < 3) {
        uint32_t now, left;

        sched_yield();
        now = page_used();
        left = proc_exited_unreaped();

        if (now == last && left == pending) {
            same++;
        } else {
            same = 0;
            last = now;
            pending = left;
        }
        if (time_now() > deadline) {
            panic("frames: the count never settled, last %u with %u "
              "processes still to reap", (unsigned int)last,
              (unsigned int)pending);
        }
    }

    return last;
}

/*
 * What holds a frame that no process holds, per M13 stage 4.
 *
 * Stages two and three made a process give back everything it cost, and
 * the count after a spawn and a reap returns to where it started. That
 * is not the same as the machine giving everything back, because not
 * every frame belongs to a process. Some belong to an object that
 * outlives the process it was lent to, and the reap that returns a
 * process's own memory must not touch those: a grant's memory is the
 * grant's, wherever it happens to be mapped.
 *
 * So this measures rather than frees. Each mechanism is run with the
 * count read either side, and what stays is reported beside the name of
 * what is holding it. Freeing them and finding out is the other way to
 * learn this, and it is the way that hands a live frame to somebody
 * else.
 *
 * The numbers are asserted because they are counts. Two of them are not
 * written down here but read from the run that produced them, which is
 * the difference between checking a mechanism and checking a constant:
 * what the pager holds is the pages the pager supplied, whatever the
 * image's size turns out to be.
 *
 * What is measured is what the run left behind. Who holds it is an
 * argument, printed beside the number and not established by it, so a
 * figure that changes means something in that run stopped giving a
 * frame back, and not necessarily the thing named.
 *
 * It waits for the number rather than reading it once, because stage
 * two established that the join is not the reap: a program that has
 * exited holds its slot until its last thread is collected at the top
 * of some core's next switch, and a count read before that has a whole
 * process in it. Waiting on the figure itself is what this settled on
 * after waiting on a proxy went wrong. The proxy was the process pool's
 * count returning to what it was before the check, which is not this
 * check's business to require: under the paced session the console has
 * input to deal with and a process appears that this never started, and
 * the wait then failed on somebody else's work.
 */
static void
frame_holder(const char *what, uint32_t before, uint32_t expect,
  const char *who)
{
    sys_time_t deadline = time_now() + time_from_ms(JOIN_MS);
    int64_t held;

    /*
     * Signed, and exact. This used to clamp a count that had fallen to
     * zero and compare that, which for a row expecting nothing held is
     * a check that cannot fail: anything else giving memory back inside
     * the window drives the difference negative, the clamp reads it as
     * nothing held, and a leak of this check's own would be hidden
     * under it. Fewer frames than there were is not nothing held, it is
     * a window that had somebody else's work in it, and saying so is
     * the only way the row means anything.
     */
    for (;;) {
        held = (int64_t)page_used() - (int64_t)before;
        if (held == (int64_t)expect) {
            break;
        }
        if (time_now() > deadline) {
            panic("frames: %s left %ld held, not %u", what,
              (long)held, (unsigned int)expect);
        }
        sched_yield();
    }

    cnprintf("    %-38s %ld, held by %s\n", what, (long)held, who);
}

/*
 * A row of the same table measured against its holder rather than
 * against the machine's free memory, per M14 stage two's review.
 *
 * The two rows with a named holder can be asked about it directly:
 * grant_frames() is the frames grants describe, so a check that hands a
 * page to a grant should raise it by one and a pager that answers five
 * faults should raise it by five. That is the quantity the row is
 * about.
 *
 * The window form these used to have is confounded by anything else
 * giving memory back inside it, and something does: reaping is deferred
 * to a switch, and work inside one of these checks can release the last
 * handle to a process that exited during the check before it. Then a
 * dozen frames come back in the middle of the window and the row reads
 * as though its holder had let go. Waiting for quiet does not fix it,
 * because a parked thread moves no frames until it is collected, and
 * draining first does not either, because the release happens inside
 * the window rather than before it.
 */
static void
grant_holder(const char *what, uint32_t before, uint32_t expect,
  const char *who)
{
    /*
     * Signed for the reason the other one is: a count that fell is not
     * nothing held, and reading it as nothing is how a row stops being
     * able to fail. Nothing destroys a grant today, so this cannot go
     * negative yet, and the day something does the row will say so
     * rather than wrap to four billion.
     */
    int64_t held = (int64_t)grant_frames() - (int64_t)before;

    cnprintf("    %-38s %ld, held by %s\n", what, (long)held, who);

    if (held != (int64_t)expect) {
        panic("frames: %s left %ld held, not %u", what,
          (long)held, (unsigned int)expect);
    }
}



/*
 * And the same question asked of a page that has been lent away and
 * given back, per M14 stage three's review.
 *
 * A buffer sent to a server is unmapped from the client, mapped into
 * the server and mapped back when the reply comes. That is the one path
 * on which a program's own page is re-mapped by something other than
 * the loader, so it is the one place the permissions the loader chose
 * can be quietly replaced by different ones. They were: the take-back
 * asked the borrower what the page should be, the borrower does not own
 * that address, and the answer for an address a process does not own is
 * everything.
 *
 * Here rather than beside the other two because it needs a server to
 * lend to, and the file server does not exist when those run.
 */
static void
lent_permission_checks(sys_handle_t root)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct sys_op_status_args sargs;
    const struct boot_member *m;
    const char *argv[2];
    sys_ns_spec_t ns[1];
    sys_handle_t h;
    struct child c;
    void *obj;

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_ROOT;
    ns[0].handle = root;

    argv[0] = "leap";
    argv[1] = "lent";

    sched_defer();
    if (proc_spawn_op(self, "leap", PRIO_BOOT - 1, ns, 1, argv, 2, &h) !=
      SYS_OK) {
        panic("permissions: could not start leap with a connection");
    }
    if (handle_lookup(ht, h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
        panic("permissions: the spawn returned no process");
    }
    child_watch(&c, obj);
    sched_undefer();

    (void)child_join(&c, JOIN_MS);

    memset(&sargs, 0, sizeof(sargs));
    if (run(SYS_OP_PROC_STATUS, h, &sargs, sizeof(sargs)) != SYS_OK) {
        panic("permissions: no status for leap");
    }

#if SYS_TIER == 2
    if (sargs.out_fault != SYS_FAULT_ACCESS) {
        panic("permissions: a program ran from a buffer it had lent and "
          "lived, kind %u", (unsigned int)sargs.out_fault);
    }

    m = boot_lookup("leap");
    if (m == NULL) {
        panic("permissions: no leap in the archive");
    }
    if (sargs.out_fault_addr < (uintptr_t)m->bm_load + m->bm_rw ||
      sargs.out_fault_addr >= (uintptr_t)m->bm_load + m->bm_mem) {
        panic("permissions: ran from 0x%lx, outside its writable memory",
          (unsigned long)sargs.out_fault_addr);
    }
    cnprintf("    %-38s %s at 0x%lx\n",
      "a program running from a lent buffer", fault_name(sargs.out_fault),
      (unsigned long)sargs.out_fault_addr);
#else
    (void)m;
    if (sargs.out_fault != SYS_FAULT_INSTRUCTION) {
        panic("permissions: flat memory answered a jump into a lent "
          "buffer with kind %u", (unsigned int)sargs.out_fault);
    }
    cnprintf("    %-38s %s\n", "a program running from a lent buffer",
      "fetched, then refused as an instruction");
#endif

    expect("giving leap's handle back", handle_close(ht, h), SYS_OK);

    /*
     * And this runs last of everything in the slice, because closing
     * that handle does not reap it. Something else still holds a
     * reference at this point, released later by work the boot thread
     * does rather than by yielding, so a loop waiting here would wait
     * for ever. The first placement had this check before the frame
     * measurements and its sixteen frames came back in the middle of
     * one: the row after it read minus sixteen. Where a check's
     * leavings land is part of the check, and what holds that
     * reference is a question for a stage that is about references.
     */
}


/*
 * Where the startup record is, per M14 stage four.
 *
 * Section 20.4 has the loader place the record in the child's memory.
 * Until this milestone that meant the last page of the slot, chosen
 * because it was the one place a program's own layout did not reach
 * while the whole slot was mapped. Nothing maps the whole slot now, so
 * that page is not there, and the record moved to the last page of what
 * the program needs, which the linker script reserves above the stack.
 *
 * Two things are worth asserting about it and one is not. That a
 * program can read its namespace and its arguments is checked by every
 * program in this boot that has either, and would fail loudly. What
 * those checks would not notice is where the record sits: a record
 * placed somewhere a program can still read is a record that works
 * today and stops working the moment the loader maps one page less.
 * So this asserts the address against the archive, and that the page is
 * read-only, which is what the loader chose for it.
 */
/*
 * What must not be freed under somebody, per M20 stage four.
 *
 * Stage three frees a channel when the last thing naming it lets go.
 * The question this answers is what "nothing names it" leaves out,
 * and the answer is a thread that is already inside. Four things were
 * proposed and three of them turned out to be parties: a thread
 * queued to send, a thread blocked receiving, and a thread inside a
 * request. The fourth, a pulse nobody has collected, is not.
 *
 * Each is built, the handles are all closed under it, and the slot is
 * required to still be there. Then the party is let out and the slot
 * is required to come back. Asserting only the second half would pass
 * on a kernel that freed the channel immediately and got away with
 * it, which is the shape of every check this project has had to throw
 * away.
 */
static struct channel *hold_ch;
static volatile bool hold_sent;

static void
hold_sender(void *arg)
{
    struct sys_op_send_args tx;

    memset(&tx, 0, sizeof(tx));
    tx.request.inline_len = 4;
    memcpy(tx.request.inline_data, "hold", 4);

    if (run(SYS_OP_IPC_SEND, (sys_handle_t)(uintptr_t)arg, &tx,
      sizeof(tx)) != SYS_OK) {
        panic("holds: the sender's send failed");
    }
    hold_sent = true;
    thread_exit(0);
}

/* How long the sender that is never served waits before giving up. */
#define HOLD_GIVE_UP_MS     20u

static volatile bool hold_gave_up;

/*
 * A sender that nobody will ever serve, per M20 stage seven's review.
 * It queues, its handle is closed under it, and what gets it back is
 * its own deadline rather than a server.
 */
static void
hold_quitter(void *arg)
{
    struct sys_op_send_args tx;
    sys_op_t send;

    memset(&tx, 0, sizeof(tx));
    tx.request.handle = SYS_HANDLE_INVALID;
    tx.request.inline_len = 4;
    memcpy(tx.request.inline_data, "gone", 4);
    tx.deadline = time_now() + time_from_ms(HOLD_GIVE_UP_MS);

    memset(&send, 0, sizeof(send));
    send.opcode = SYS_OP_IPC_SEND;
    send.target = (sys_handle_t)(uintptr_t)arg;
    send.flags = SYS_OP_DEADLINE;
    send.arg_ptr = (uintptr_t)&tx;
    send.arg_len = sizeof(tx);

    if ((sys_err_t)run_batch(&send, 1) != SYS_ERR_TIMEOUT) {
        panic("holds: the sender nobody served did not give up");
    }
    hold_gave_up = true;
    thread_exit(0);
}

static volatile bool hold_received;


static void
hold_receiver(void *arg)
{
    struct sys_op_recv_args rx;

    memset(&rx, 0, sizeof(rx));
    if (run(SYS_OP_IPC_RECV, (sys_handle_t)(uintptr_t)arg, &rx,
      sizeof(rx)) != SYS_OK) {
        panic("holds: the receiver's receive failed");
    }
    hold_received = true;
    thread_exit(0);
}

/* Whether the pool still has this slot, by identifier and not pointer. */
static bool
slot_alive(sys_id_t id)
{
    return pool_lookup(channel_pool(), id) != NULL;
}

/*
 * Wait for something to become true, and give up rather than spin for
 * ever. A check that hangs is a machine stopped with no cause
 * attached, which is what the deadline on every join here is for.
 */
static void
wait_for(const volatile bool *flag, const char *what)
{
    uint64_t until = time_now() + time_hz();

    while (!*flag) {
        if (time_now() > until) {
            panic("holds: %s never happened", what);
        }
        sched_yield();
    }
}

static void
wait_until(struct thread *const volatile *head, const char *what)
{
    uint64_t until = time_now() + time_hz();

    while (*head == NULL) {
        if (time_now() > until) {
            panic("holds: %s never happened", what);
        }
        sched_yield();
    }
}

static void
channel_hold_checks(void)
{
    struct proc *self = proc_current();
    uint32_t base = channel_pool()->used;
    uint32_t discarded = channel_pulses_discarded;

    cnputs("holds: what a channel may not be freed under\n");

    /*
     * A thread queued to send, and then the same thread inside a
     * request. Two obstacles from one construction, because they are
     * the two halves of one rendezvous: the sender is on the queue
     * until a server takes it, and the server is on the serving list
     * until it replies.
     *
     * The boot thread does the receiving and replying without a
     * handle, which is the only way to do it here: the handles are
     * all closed by then, and that is the point.
     */
    {
        sys_handle_t h;
        sys_id_t id;
        struct sys_msg msg;
        sys_thread_id_t peer;
        uint64_t cookie;
        struct thread *t;

        hold_ch = channel_create("held", self);
        if (hold_ch == NULL) {
            panic("holds: no channel");
        }
        id = hold_ch->c_id;
        expect("a send handle for a thread that will queue",
          handle_insert(&self->p_handles, hold_ch, SYS_OBJ_CHANNEL,
          SYS_RIGHT_SEND, &h), SYS_OK);

        hold_sent = false;
        t = thread_create("holder", 20, hold_sender,
          (void *)(uintptr_t)h);
        sched_ready(t);

        wait_until(&hold_ch->c_senders, "the sender queueing");

        expect("closing the only handle under it",
          handle_close(&self->p_handles, h), SYS_OK);

        if (channel_holds(hold_ch) != 0) {
            panic("holds: %u still name a channel with no handles",
              (unsigned int)channel_holds(hold_ch));
        }
        if (!slot_alive(id)) {
            panic("holds: the slot went with a sender queued on it");
        }
        cnprintf("    %-38s %s\n", "with a sender queued on it",
          "kept");

        expect("taking the sender's message without a handle",
          channel_recv(hold_ch, &msg, &peer, &cookie), SYS_OK);
        if (!slot_alive(id)) {
            panic("holds: the slot went with a server inside it");
        }
        if (hold_ch->c_serving == NULL) {
            panic("holds: nothing is serving after a receive");
        }
        cnprintf("    %-38s %s\n", "with a server inside a request",
          "kept");

        expect("replying, which lets the last of them out",
          channel_reply(peer, &msg), SYS_OK);
        wait_for(&hold_sent, "the sender returning");

        if (slot_alive(id)) {
            panic("holds: the slot stayed after everybody left");
        }
        cnprintf("    %-38s %s\n", "and when the last one leaves",
          "given back");
    }

    /*
     * And the same sender, let out the other way, per stage seven's
     * review. Stage four proved the slot comes back when the sender
     * is served. A sender that gives up comes off the same queue and
     * leaves the channel just as empty, and until this check there
     * was nothing asking on that path: the slot stayed for ever.
     *
     * Nobody receives here. What gets the sender back is the
     * deadline it set itself.
     */
    {
        sys_handle_t h;
        sys_id_t id;
        struct thread *t;
        uint64_t until;

        hold_ch = channel_create("quit", self);
        if (hold_ch == NULL) {
            panic("holds: no channel for the sender that gives up");
        }
        id = hold_ch->c_id;
        expect("a send handle for a thread nobody will serve",
          handle_insert(&self->p_handles, hold_ch, SYS_OBJ_CHANNEL,
          SYS_RIGHT_SEND, &h), SYS_OK);

        hold_gave_up = false;
        t = thread_create("quitter", 20, hold_quitter,
          (void *)(uintptr_t)h);
        sched_ready(t);

        wait_until(&hold_ch->c_senders, "the sender queueing to quit");

        expect("closing the only handle under that one too",
          handle_close(&self->p_handles, h), SYS_OK);
        if (!slot_alive(id)) {
            panic("holds: the slot went with a sender still queued");
        }

        until = time_now() + time_from_ms(JOIN_MS);
        while (!hold_gave_up) {
            if (time_now() > until) {
                panic("holds: the sender never gave up");
            }
            sched_yield();
        }

        if (slot_alive(id)) {
            panic("holds: the slot stayed after the sender gave up");
        }
        cnprintf("    %-38s %s\n",
          "and when the sender gives up instead", "given back");
    }

    /*
     * A thread blocked receiving. Let out with a pulse, because a
     * message would need a handle to send and there are none.
     */

    {
        sys_handle_t h;
        sys_id_t id;
        struct thread *t;

        hold_ch = channel_create("heard", self);
        if (hold_ch == NULL) {
            panic("holds: no channel to receive on");
        }
        id = hold_ch->c_id;
        expect("a receive handle for a thread that will wait",
          handle_insert(&self->p_handles, hold_ch, SYS_OBJ_CHANNEL,
          SYS_RIGHT_RECV, &h), SYS_OK);

        hold_received = false;
        t = thread_create("hearer", 20, hold_receiver,
          (void *)(uintptr_t)h);
        sched_ready(t);

        wait_until(&hold_ch->c_receivers, "the receiver arriving");

        expect("closing the only handle under that one",
          handle_close(&self->p_handles, h), SYS_OK);
        if (!slot_alive(id)) {
            panic("holds: the slot went with a thread receiving");
        }
        cnprintf("    %-38s %s\n", "with a thread blocked receiving",
          "kept");

        channel_pulse(hold_ch, SYS_PULSE_USER_BASE, 1);
        wait_for(&hold_received, "the receiver waking");

        if (slot_alive(id)) {
            panic("holds: the slot stayed after the receiver left");
        }
        cnprintf("    %-38s %s\n", "and when it wakes and leaves",
          "given back");
    }

    /*
     * A connection, which is not a thread and is a holder all the
     * same: it names the channel and a pager is one of these.
     */
    {
        sys_handle_t h;
        sys_id_t id;
        struct connection *cn;

        hold_ch = channel_create("connected", self);
        if (hold_ch == NULL) {
            panic("holds: no channel to connect to");
        }
        id = hold_ch->c_id;
        cn = connection_create(hold_ch, 0);
        if (cn == NULL) {
            panic("holds: no connection");
        }
        expect("a handle beside a connection",
          handle_insert(&self->p_handles, hold_ch, SYS_OBJ_CHANNEL,
          SYS_RIGHT_RECV, &h), SYS_OK);
        expect("closing the handle and not the connection",
          handle_close(&self->p_handles, h), SYS_OK);

        if (channel_holds(hold_ch) != 1) {
            panic("holds: %u name a channel one connection is on",
              (unsigned int)channel_holds(hold_ch));
        }
        if (!slot_alive(id)) {
            panic("holds: the slot went with a connection on it");
        }
        cnprintf("    %-38s %s\n", "with a connection over it",
          "kept");

        connection_destroy(cn);
        if (slot_alive(id)) {
            panic("holds: the slot stayed after the connection went");
        }
        cnprintf("    %-38s %s\n", "and when the connection goes",
          "given back");
    }

    /*
     * And a pulse, which is the one that turned out not to be a
     * party. Nothing can collect it: collecting is receiving,
     * receiving needs a handle, and the handle is what just went. A
     * slot kept for a message with no possible reader is a slot kept
     * for nobody, so the pulses go and the count of them is reported.
     */
    {
        sys_handle_t h;
        sys_id_t id;

        hold_ch = channel_create("pulsed", self);
        if (hold_ch == NULL) {
            panic("holds: no channel to pulse");
        }
        id = hold_ch->c_id;
        expect("a handle on a channel about to be pulsed",
          handle_insert(&self->p_handles, hold_ch, SYS_OBJ_CHANNEL,
          SYS_RIGHT_RECV, &h), SYS_OK);
        channel_pulse(hold_ch, SYS_PULSE_USER_BASE, 2);
        if (hold_ch->c_pulse_count != 1) {
            panic("holds: %u pulses queued, not 1",
              (unsigned int)hold_ch->c_pulse_count);
        }
        expect("closing the handle with a pulse waiting",
          handle_close(&self->p_handles, h), SYS_OK);

        if (slot_alive(id)) {
            panic("holds: the slot stayed for a pulse nobody can "
              "collect");
        }
        if (channel_pulses_discarded != discarded + 1) {
            panic("holds: %u pulses discarded, not %u",
              (unsigned int)channel_pulses_discarded,
              (unsigned int)(discarded + 1));
        }
        cnprintf("    %-38s %s\n", "with a pulse nobody can collect",
          "given back, and the pulse with it");
    }

    hold_ch = NULL;

    if (channel_pool()->used != base) {
        panic("holds: %u channels in use at the end, not %u",
          (unsigned int)channel_pool()->used, (unsigned int)base);
    }
    cnprintf("    %-38s %u, where it started\n",
      "slots in use after all of it", (unsigned int)base);
}

/*
 * A server ended, and its channel with it, per M20 stage five.
 *
 * Sections 18.2 and 18.3 argue from a supervisor that ends a server
 * and starts another, and M18's retrospective listed it as untested.
 * M16 built half of it: a server is killed and a replacement takes
 * its place, with the kernel holding the channel across the gap and
 * handing a duplicate to each one. That is a restart where the
 * channel never goes.
 *
 * This is the other half. The server is given the only handle there
 * is, so the channel belongs to it in the sense section 12.5 means,
 * and when it dies the channel goes with it. The replacement makes
 * its own. Done more times than the pool has slots, which is the
 * whole point: a system that restarts a server once an hour for a
 * week must not be a system that has run out of channels.
 *
 * And the server is killed before it has ever run, per M21 stage
 * six. This check used to send the server a request and wait for
 * the answer first, because a kill written against a thread that
 * was ready and had never run set the mark and ended nothing: the
 * thread went on into user mode, blocked in a receive, and the
 * join waited out thirty seconds and then ten minutes. The round
 * trip was a workaround written into a check to get past a defect
 * in the kernel, and it is gone. The kill is written inside the
 * same deferred region as the spawn, so on every round the thread
 * has not run, which is the case M21 stage two added a reading
 * for and the one this loop could not survive before it.
 */
#define RESTART_ROUNDS      (SYS_MAX_CHANNELS + 4u)

/*
 * What the loop did, kept for the report at the end of the boot. A
 * count of servers and the one slot they all took, per M20 stage
 * seven.
 */
static uint32_t restarts_done;
static uint32_t restarts_slot;

/*
 * Entries started by a thread whose process had been killed, summed
 * over the checks that kill one, per M21 stages one and six. The
 * number the milestone is about is zero, and it is a sum rather
 * than one check's reading because more than one check now ends a
 * process before its thread has run.
 */
static uint64_t marked_entries;

/* How long the client left holding a dead server's channel waits. */
#define RESTART_WAIT_MS     20u

static void
restart_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    uint32_t base = channel_pool()->used;
    uint32_t peak = channel_pool()->highwater;
    struct sys_op_kill_args kargs;
    sys_ns_spec_t ns[1];
    uint32_t round;
    uint32_t fresh = 0;
    uint32_t ended = 0;

    cnputs("restarts: a server ended, and its channel with it\n");

    for (round = 0; round < RESTART_ROUNDS; round++) {
        struct child sc;
        struct channel *ch;
        sys_id_t id;
        sys_handle_t give, h;
        uint64_t unrun;
        void *obj;
        int32_t code;

        ch = channel_create("restarted", self);
        if (ch == NULL) {
            panic("restarts: no channel on round %u",
              (unsigned int)round);
        }
        id = ch->c_id;

        /*
         * One handle, and it is the server's as soon as the spawn
         * has it. That is the arrangement section 18.3 describes: a
         * supervisor that started a server and is not one of its
         * clients. The connection a client used to hold is gone
         * with the round trip it was for, and the case of a client
         * that outlives its server is the one below rather than
         * this one.
         */
        if (handle_insert(ht, ch, SYS_OBJ_CHANNEL,
          SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &give) != SYS_OK) {
            panic("restarts: no handle to give on round %u",
              (unsigned int)round);
        }

        memset(ns, 0, sizeof(ns));
        ns[0].id = SYS_NS_SERVICE;
        ns[0].handle = give;

        sched_defer();
        if (proc_spawn_op(self, "echoes", PRIO_BOOT, ns, 1, NULL, 0,
          &h) != SYS_OK) {
            panic("restarts: no server on round %u",
              (unsigned int)round);
        }
        if (handle_lookup(ht, h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
            panic("restarts: the spawn returned no process");
        }
        child_watch(&sc, obj);

        /*
         * The server's handle moved rather than being copied, so
         * the server has all of it. If that stopped being true the
         * channel would never go and the loop would run the pool
         * out, which is a slower way of finding out the same thing.
         *
         * Asked here, inside the deferred region, because after it
         * the server can run, end and be collected, and a check
         * that read the channel then would be reading a slot
         * something else had taken.
         */
        if (channel_holds(ch) != 1) {
            panic("restarts: %u hold a channel the server owns",
              (unsigned int)channel_holds(ch));
        }
        if (round == 0) {
            fresh = sys_id_index(id);
        } else if (sys_id_index(id) != fresh) {
            panic("restarts: round %u took slot %u, not %u",
              (unsigned int)round, (unsigned int)sys_id_index(id),
              (unsigned int)fresh);
        }

        /*
         * And killed before it has run, in the region the spawn was
         * made in, so that nothing dispatched it in between. This
         * is the sequence M20 could not write. The mark is set on a
         * process whose only thread is ready and has never been on
         * a core, and what ends that thread is the reading in the
         * trampoline it is about to arrive on.
         */
        unrun = proc_unrun_programs;
        memset(&kargs, 0, sizeof(kargs));
        kargs.code = 7;
        if (run(SYS_OP_PROC_KILL, h, &kargs, sizeof(kargs)) != SYS_OK) {
            panic("restarts: the server refused to be killed");
        }
        sched_undefer();

        code = child_join(&sc, JOIN_MS);
        if (code != 7) {
            panic("restarts: the server exited %d on round %u",
              (int)code, (unsigned int)round);
        }

        /*
         * And it ended where a thread that has never run ends,
         * asked once a round rather than once at the end. The
         * total at the end of the boot would come out right with
         * one round having taken another way out, and the way out
         * this loop is about is the one in the trampoline.
         */
        if (proc_unrun_programs != unrun + 1) {
            panic("restarts: round %u ended somewhere other than "
              "its trampoline", (unsigned int)round);
        }
        ended++;

        /*
         * And it started nothing on its way out, which is the same
         * claim stage one's check makes and the reason this loop
         * adds to that count rather than keeping one of its own.
         */
        marked_entries += sc.c_proc->p_dying_entries;
        if (handle_close(ht, h) != SYS_OK) {
            panic("restarts: the spawn handle was already gone");
        }

        /*
         * And the channel comes back at the reap rather than at the
         * exit, which is section 15.6's distinction and not a
         * looseness in this check. A join says the process has
         * ended; its handle table is closed when the last thing that
         * could ask about it lets go, and the collection happens at
         * the top of whichever core's next switch reaches it. So
         * this waits, and says how long it was willing to.
         */
        {
            uint64_t until = time_now() + time_from_ms(JOIN_MS);

            while (slot_alive(id)) {
                if (time_now() > until) {
                    panic("restarts: the channel outlived its server "
                      "on round %u", (unsigned int)round);
                }
                sched_yield();
            }
        }
    }

    /*
     * The pool where it started, after more restarts than it has
     * slots. The high-water mark is the assertion with teeth: a
     * kernel that freed nothing would have reached it long ago, and
     * one that freed everything but handed out a fresh slot each time
     * would have too.
     */
    if (channel_pool()->used != base) {
        panic("restarts: %u channels in use, not %u",
          (unsigned int)channel_pool()->used, (unsigned int)base);
    }
    if (channel_pool()->highwater != peak) {
        panic("restarts: the high-water mark went from %u to %u",
          (unsigned int)peak,
          (unsigned int)channel_pool()->highwater);
    }
    if (ended != RESTART_ROUNDS) {
        panic("restarts: %u of %u servers ended",
          (unsigned int)ended, (unsigned int)RESTART_ROUNDS);
    }
    restarts_done = ended;
    restarts_slot = fresh;
    cnprintf("    %-38s %u, all on slot %u\n",
      "killed before running, and ended", (unsigned int)ended,
      (unsigned int)fresh);
    cnprintf("    %-38s %u in use, %u at once\n",
      "and the pool where it started", (unsigned int)base,
      (unsigned int)peak);

    /*
     * And a client that was holding a connection when the server
     * went. Two questions, and the second is the one section 12.5
     * draws a distinction for.
     *
     * The channel does not go with the server here, because the
     * connection names it and a connection is a holder. That is the
     * right answer rather than a limitation: a client still holding
     * something it can send on is a reason for the object to exist.
     *
     * What the client is told when it sends afterwards is nothing.
     * It queues, and what gets it back is its own deadline.
     *
     * That is not the answer this stage went looking for and it is
     * the right one. Section 12.5's two errors say what a client is
     * told when the server it was already talking to went:
     * `SYS_ERR_STALE` if the server is still there to be asked
     * again, `SYS_ERR_OWNER_DEAD` if it is not. A client that starts
     * talking afterwards is a different question, and the kernel
     * cannot answer it: a channel whose owner is gone is exactly
     * what a channel looks like between a server being killed and
     * its replacement taking the receive right, which is the restart
     * M16 built and made invisible to clients on purpose. Refusing
     * the send would make every restart visible to every client.
     *
     * So the two cases differ and the difference is worth saying.
     * The client in flight when the owner died is released with
     * `SYS_ERR_OWNER_DEAD`, because leaving it there across a gap of
     * unknown length is worse. The client that arrives afterwards
     * waits, because it might be about to be served. Neither is the
     * kernel knowing which happened.
     */
    {
        struct child sc;
        struct channel *ch;
        struct sys_op_connect_args cargs;
        struct sys_op_send_args tx;
        sys_id_t id;
        sys_handle_t h_recv, give, h_send, h;
        void *obj;

        ch = channel_create("outlived", self);
        if (ch == NULL) {
            panic("restarts: no channel for the held case");
        }
        id = ch->c_id;

        expect("a channel a client will hold on to",
          handle_insert(ht, ch, SYS_OBJ_CHANNEL,
          SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &h_recv), SYS_OK);
        memset(&cargs, 0, sizeof(cargs));
        cargs.rights = SYS_RIGHT_SEND;
        expect("a connection for the client",
          run(SYS_OP_CHAN_CONNECT, h_recv, &cargs, sizeof(cargs)),
          SYS_OK);
        h_send = cargs.out_handle;

        if (handle_insert(ht, ch, SYS_OBJ_CHANNEL,
          SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &give) != SYS_OK) {
            panic("restarts: cannot hand the channel over");
        }
        memset(ns, 0, sizeof(ns));
        ns[0].id = SYS_NS_SERVICE;
        ns[0].handle = give;

        sched_defer();
        if (proc_spawn_op(self, "echoes", PRIO_BOOT, ns, 1, NULL, 0,
          &h) != SYS_OK) {
            panic("restarts: no server for the held case");
        }
        if (handle_lookup(ht, h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
            panic("restarts: the spawn returned no process");
        }
        child_watch(&sc, obj);
        sched_undefer();

        {
            struct thread *st = proc_threads(sc.c_proc);
            uint64_t until = time_now() + time_from_ms(JOIN_MS);

            while (st->t_wait != THREAD_WAIT_RECV) {
                if (time_now() > until) {
                    panic("restarts: the held case's server never "
                      "reached its receive point");
                }
                sched_yield();
            }
        }

        expect("and the kernel letting go of its own",
          handle_close(ht, h_recv), SYS_OK);

        memset(&kargs, 0, sizeof(kargs));
        kargs.code = 7;
        expect("ending the server under its client",
          run(SYS_OP_PROC_KILL, h, &kargs, sizeof(kargs)), SYS_OK);
        if (child_join(&sc, JOIN_MS) != 7) {
            panic("restarts: the held case's server exited wrong");
        }
        (void)handle_close(ht, h);

        /*
         * By identifier, because the thing being tested is whether
         * the channel is still there. Reading holds through the
         * pointer would be reading whatever took the slot if this
         * check were failing, which is the one case where a wrong
         * answer matters.
         */
        {
            uint64_t until = time_now() + time_from_ms(JOIN_MS);
            struct channel *live;

            for (;;) {
                live = pool_lookup(channel_pool(), id);
                if (live == NULL) {
                    panic("restarts: the channel went under a live "
                      "client");
                }
                if (channel_holds(live) == 1) {
                    break;
                }
                if (time_now() > until) {
                    panic("restarts: %u hold a channel one client is "
                      "on", (unsigned int)channel_holds(live));
                }
                sched_yield();
            }
        }
        cnprintf("    %-38s %s\n", "a client holding it across the end",
          "kept");

        {
            sys_op_t send;
            uint64_t deadline;

            memset(&tx, 0, sizeof(tx));
            tx.request.handle = SYS_HANDLE_INVALID;
            tx.request.inline_len = 1;
            deadline = time_now() + time_from_ms(RESTART_WAIT_MS);
            tx.deadline = deadline;

            memset(&send, 0, sizeof(send));
            send.opcode = SYS_OP_IPC_SEND;
            send.target = h_send;
            send.flags = SYS_OP_DEADLINE;
            send.arg_ptr = (uintptr_t)&tx;
            send.arg_len = sizeof(tx);

            expect("what that client is told when it sends",
              (sys_err_t)run_batch(&send, 1), SYS_ERR_TIMEOUT);
            if (time_now() < deadline) {
                panic("restarts: the send came back before its "
                  "deadline, so it was told something");
            }
            cnprintf("    %-38s %s\n",
              "and it waited out its own deadline",
              "nothing else told it");
        }


        expect("and the client letting go",
          handle_close(ht, h_send), SYS_OK);
        if (slot_alive(id)) {
            panic("restarts: the channel stayed after its last "
              "client went");
        }
        cnprintf("    %-38s %s\n", "and when that client lets go",
          "given back");
    }

    if (channel_pool()->used != base) {
        panic("restarts: %u channels in use at the end, not %u",
          (unsigned int)channel_pool()->used, (unsigned int)base);
    }
}

/*
 * More channels than the pool has, per M20 stage six.
 *
 * M13's shape, which is the one that actually proves reuse. Stage
 * five spends a channel at a time, fifty-two of them, and that says
 * a slot comes back. It does not say the pool can carry a load: one
 * slot going round is the easiest case there is, and a kernel that
 * leaked a slot for every two it freed would pass it.
 *
 * So this holds several at once and does it over and over. Eight
 * alive together, sixty-four times, which is more cycles than the
 * pool has slots and more than ten times as many channels as exist.
 * What is asserted is the pool back where it started after every
 * cycle, the same slots each time on later generations, the number
 * the kernel counted out against the number counted back, and the
 * high-water mark. The mark covers the one thing the others cannot:
 * a peak reached inside a cycle and gone again by the end of it.
 *
 * Every channel here is closed down two different ways, because
 * there are two orders and they take different paths through the
 * free. Closing the receive handle first leaves the connection
 * holding the slot; closing the send handle first leaves the handle
 * holding it. Either way the last one out is what gives it back.
 * Which order a channel gets turns on the cycle as well as on its
 * place in it, so that every slot sees both and not only the one its
 * position would always give it.
 */
#define SPEND_CYCLES      (SYS_MAX_CHANNELS + 16u)
#define SPEND_AT_ONCE     8u

/*
 * Order a cycle's slots so two cycles can be compared. Eight of them,
 * so the sort is the obvious one, and the generation travels with the
 * slot because what is being compared is both.
 */
static void
sort_slots(uint32_t *slot, uint64_t *gen, uint32_t n)
{
    uint32_t i, j;

    for (i = 1; i < n; i++) {
        uint32_t s = slot[i];
        uint64_t g = gen[i];

        for (j = i; j > 0 && slot[j - 1] > s; j--) {
            slot[j] = slot[j - 1];
            gen[j] = gen[j - 1];
        }
        slot[j] = s;
        gen[j] = g;
    }
}

static void
channel_endurance_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct sys_op_connect_args cargs;
    uint32_t base = channel_pool()->used;
    uint32_t first[SPEND_AT_ONCE];
    uint64_t seen[SPEND_AT_ONCE];
    uint32_t gave_back = channel_freed;
    uint32_t peak = 0;
    uint32_t spent = 0;
    uint32_t cycle, i;
    uint64_t started, took;

    cnputs("channels spent: more of them than the pool has slots\n");

    if (base + SPEND_AT_ONCE > SYS_MAX_CHANNELS) {
        panic("spent: %u slots in use, so %u more will not fit",
          (unsigned int)base, (unsigned int)SPEND_AT_ONCE);
    }

    started = time_now();

    for (cycle = 0; cycle < SPEND_CYCLES; cycle++) {
        sys_handle_t recv[SPEND_AT_ONCE], send[SPEND_AT_ONCE];
        sys_id_t id[SPEND_AT_ONCE];
        uint32_t slot[SPEND_AT_ONCE];
        uint64_t gen[SPEND_AT_ONCE];

        for (i = 0; i < SPEND_AT_ONCE; i++) {
            struct channel *ch = channel_create("spent", self);

            if (ch == NULL) {
                panic("spent: no channel on cycle %u, %u in",
                  (unsigned int)cycle, (unsigned int)i);
            }
            id[i] = ch->c_id;
            slot[i] = sys_id_index(id[i]);
            gen[i] = sys_id_gen(id[i]);

            if (handle_insert(ht, ch, SYS_OBJ_CHANNEL,
              SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER,
              &recv[i]) != SYS_OK) {
                panic("spent: no handle on cycle %u",
                  (unsigned int)cycle);
            }
            memset(&cargs, 0, sizeof(cargs));
            cargs.rights = SYS_RIGHT_SEND;
            if (run(SYS_OP_CHAN_CONNECT, recv[i], &cargs,
              sizeof(cargs)) != SYS_OK) {
                panic("spent: no connection on cycle %u",
                  (unsigned int)cycle);
            }
            send[i] = cargs.out_handle;
            spent++;
        }

        /*
         * The same slots as the first cycle and not merely the same
         * number of them, which is stage three's rule applied to a
         * load rather than to one channel. The generation has to
         * have moved on for every one of them as well: a slot that
         * came back with the generation it went in with would mean
         * an identifier somebody kept still names it.
         */
        sort_slots(slot, gen, SPEND_AT_ONCE);
        for (i = 0; i < SPEND_AT_ONCE; i++) {
            /*
             * Distinct, which is the one failure the comparison
             * below cannot see on the cycle it happens: a pool that
             * handed the same slot to two live channels would give
             * the first cycle a set the later ones matched.
             */
            if (i > 0 && slot[i] == slot[i - 1]) {
                panic("spent: slot %u came out twice on cycle %u",
                  (unsigned int)slot[i], (unsigned int)cycle);
            }
            if (cycle == 0) {
                first[i] = slot[i];
                seen[i] = gen[i];
                continue;
            }
            if (slot[i] != first[i]) {
                panic("spent: cycle %u took slot %u where the first "
                  "took %u", (unsigned int)cycle,
                  (unsigned int)slot[i], (unsigned int)first[i]);
            }
            if (gen[i] <= seen[i]) {
                panic("spent: slot %u came back on generation %lu, "
                  "having gone in on %lu", (unsigned int)slot[i],
                  (unsigned long)gen[i], (unsigned long)seen[i]);
            }
            seen[i] = gen[i];
        }

        for (i = 0; i < SPEND_AT_ONCE; i++) {
            bool handle_last = ((i + cycle) & 1u) == 0;
            sys_handle_t a = handle_last ? send[i] : recv[i];
            sys_handle_t b = handle_last ? recv[i] : send[i];

            if (handle_close(ht, a) != SYS_OK) {
                panic("spent: a handle was already gone on cycle %u",
                  (unsigned int)cycle);
            }
            if (!slot_alive(id[i])) {
                panic("spent: the slot went with one holder left on "
                  "cycle %u", (unsigned int)cycle);
            }
            if (handle_close(ht, b) != SYS_OK) {
                panic("spent: the other handle was already gone on "
                  "cycle %u", (unsigned int)cycle);
            }
            if (slot_alive(id[i])) {
                panic("spent: the slot outlived both its handles on "
                  "cycle %u", (unsigned int)cycle);
            }
        }

        if (channel_pool()->used != base) {
            panic("spent: %u in use after cycle %u, not %u",
              (unsigned int)channel_pool()->used,
              (unsigned int)cycle, (unsigned int)base);
        }
        if (cycle == 0) {
            peak = channel_pool()->highwater;
        } else if (channel_pool()->highwater != peak) {
            panic("spent: the mark went from %u to %u on cycle %u",
              (unsigned int)peak,
              (unsigned int)channel_pool()->highwater,
              (unsigned int)cycle);
        }
    }

    took = time_now() - started;

    /*
     * Made against given back, which is the assertion the count of
     * cycles cannot make: `spent` is incremented beside the create
     * and can only ever be what the loop bounds say, so asserting it
     * asserts arithmetic. What the kernel counted on the way out is
     * a different number arrived at by a different path, and the two
     * have to meet.
     */
    gave_back = channel_freed - gave_back;
    if (gave_back != spent) {
        panic("spent: %u channels made and %u slots given back",
          (unsigned int)spent, (unsigned int)gave_back);
    }

    /*
     * The two numbers the stage exists to print beside each other,
     * and what the count of them cost. How long it took is the
     * host's and is reported rather than asserted, per M9's rule.
     */
    cnprintf("    %-38s %u, of %u slots\n", "channels made and ended",
      (unsigned int)spent, (unsigned int)SYS_MAX_CHANNELS);
    cnprintf("    %-38s %u cycles, %u alive at a time\n",
      "spent in", (unsigned int)SPEND_CYCLES,
      (unsigned int)SPEND_AT_ONCE);
    cnprintf("    %-38s %u, from the first cycle on\n",
      "the high-water mark", (unsigned int)peak);
    cnprintf("    %-38s %u, where it started\n",
      "slots in use after all of it",
      (unsigned int)channel_pool()->used);
    cnprintf("    %-38s %u, every one of them\n",
      "slots the kernel counted back", (unsigned int)gave_back);
    cnprintf("    %-38s about %lu us, %lu us a channel\n",
      "what the whole of it took",
      (unsigned long)(took * 1000000ULL / time_hz()),
      (unsigned long)(took * 1000000ULL / time_hz() / spent));
}

/* Walks made by the time channel_checks reports, including its own. */
#define CHANNEL_SCANS_EXPECTED    4

/*
 * The count against a walk, per M20 stage two.
 *
 * Run at points through the boot rather than only at the end, because
 * a count that is wrong and then right again is a count that cannot be
 * trusted, and the end of a boot is the one moment when everything has
 * finished letting go. The walk itself does the comparing and stops
 * the machine on a disagreement; this is only where it is asked.
 *
 * What it costs is the reason there are five of these and not fifty.
 * Each holds the kernel lock with interrupts off for about seven
 * hundred microseconds, which is less than the longest region in this
 * boot and not by much, so they are placed at the boundaries between
 * groups of checks where nothing is part-way through.
 */
static void
channel_count_check(void)
{
    struct channel_scan cs;

    channel_scan(&cs);
}

/*
 * And what the walks found, said once.
 *
 * The count of walks is asserted because a check that silently stopped
 * running would leave this section printing a reassuring line about
 * nothing. What the walks agreed about is not printed per walk: they
 * either all agreed or the machine stopped inside one.
 */
static void
channel_checks(void)
{
    struct channel_scan cs;

    cnputs("channels: what holds one, counted and walked\n");

    /*
     * The slot itself, per M20 stage three. A pool where used falls
     * below high-water says slots come back; it does not say which
     * one came back, and a pool that handed out a fresh slot every
     * time while freeing old ones would say the same thing until it
     * ran out.
     *
     * So the identity is checked and not just the count. One channel
     * made, its slot noted, its only handle closed, and another made:
     * the same index, a generation that has moved on, and the pool
     * exactly where it started. The generation is what makes the
     * first channel's identifier name nothing rather than name the
     * second one, which is section 7.1's rule and the reason a slot
     * can be reused at all.
     */
    {
        struct proc *self = proc_current();
        struct channel *a, *b;
        sys_handle_t h;
        uint32_t before = channel_pool()->used;
        uint32_t index;
        sys_id_t gen, id;

        a = channel_create("slot", self);
        if (a == NULL) {
            panic("channels: no slot to check reuse with");
        }
        id = a->c_id;
        index = sys_id_index(id);
        gen = sys_id_gen(id);

        expect("a handle on a channel made for this",
          handle_insert(&self->p_handles, a, SYS_OBJ_CHANNEL,
          SYS_RIGHT_RECV, &h), SYS_OK);
        expect("closing it", handle_close(&self->p_handles, h), SYS_OK);

        if (channel_pool()->used != before) {
            panic("channels: %u in use after the one made here went, "
              "not %u", (unsigned int)channel_pool()->used,
              (unsigned int)before);
        }
        if (pool_lookup(channel_pool(), id) != NULL) {
            panic("channels: the identifier of a freed channel still "
              "finds one");
        }

        b = channel_create("slot again", self);
        if (b == NULL) {
            panic("channels: the slot did not come back");
        }
        if (sys_id_index(b->c_id) != index) {
            panic("channels: slot %u went back and slot %u came out",
              (unsigned int)index,
              (unsigned int)sys_id_index(b->c_id));
        }
        if (sys_id_gen(b->c_id) == gen) {
            panic("channels: the slot came back with the generation "
              "it went in with");
        }
        cnprintf("    %-38s slot %u, generation %lu then %lu\n",
          "the slot a channel gave back", (unsigned int)index,
          (unsigned long)gen, (unsigned long)sys_id_gen(b->c_id));

        /*
         * And it comes back empty, which is `pool_alloc` clearing
         * every object it hands out rather than anything this
         * milestone does. The check is here because that is the load
         * bearing part: the day a pool stops clearing a recycled
         * slot, the field that would be felt first is the flag saying
         * a pulse was lost, and a fresh channel would tell its server
         * it had missed something that happened before it existed.
         */
        if (b->c_pulse_count != 0 || b->c_pulse_users != 0 ||
          b->c_pulse_head != 0 || b->c_pulse_lost) {
            panic("channels: a reused slot came back with %u pulses, "
              "%u of them a user's, head %u, lost %s",
              (unsigned int)b->c_pulse_count,
              (unsigned int)b->c_pulse_users,
              (unsigned int)b->c_pulse_head,
              b->c_pulse_lost ? "yes" : "no");
        }
        if (b->c_serving != NULL || b->c_senders != NULL ||
          b->c_receivers != NULL || b->c_conns != NULL ||
          b->c_bounded || b->c_reaping) {
            panic("channels: a reused slot came back with somebody "
              "else's lists");
        }
        cnprintf("    %-38s %s\n", "the state it came back with",
          "empty");

        expect("a handle on the one that took its place",
          handle_insert(&self->p_handles, b, SYS_OBJ_CHANNEL,
          SYS_RIGHT_RECV, &h), SYS_OK);
        expect("closing that one too",
          handle_close(&self->p_handles, h), SYS_OK);
        if (channel_pool()->used != before) {
            panic("channels: %u in use at the end of the check, not "
              "%u", (unsigned int)channel_pool()->used,
              (unsigned int)before);
        }
    }

    channel_scan(&cs);

    cnprintf("    %-38s %u times, all agreeing\n",
      "the kernel's count against a walk", (unsigned int)channel_scans);
    cnprintf("    %-38s %u of %u, %u at once\n",
      "slots in use at this point", (unsigned int)cs.cs_used,
      (unsigned int)cs.cs_of, (unsigned int)cs.cs_at_once);
    cnprintf("    %-38s %u\n", "slots given back by here",
      (unsigned int)channel_freed);

    if (channel_scans < CHANNEL_SCANS_EXPECTED) {
        panic("channels: %u walks, not %u",
          (unsigned int)channel_scans,
          (unsigned int)CHANNEL_SCANS_EXPECTED);
    }

    /*
     * What stage one asserted here was that nothing was reachable and
     * nothing was ever given back, which was the subject of the
     * milestone. Stage three makes both false, so the assertion is
     * the other way round now: slots come back, and the pool is
     * holding fewer than it has handed out at once.
     */
    if (channel_freed == 0) {
        panic("channels: not one slot has gone back, with all the "
          "checks run");
    }
    if (cs.cs_used >= cs.cs_at_once) {
        panic("channels: %u in use and %u at once, so nothing is "
          "being reused", (unsigned int)cs.cs_used,
          (unsigned int)cs.cs_at_once);
    }
}

static void
startup_record_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct sys_op_status_args sargs;
    const struct boot_member *m;
    const char *argv[2];
    uintptr_t rec;
    char addr[19];
    sys_handle_t h;
    struct child c;
    void *obj;

    cnputs("startup: where the record a program is handed lives\n");

    m = boot_lookup("poke_addr");
    if (m == NULL) {
        panic("startup: no poke_addr in the archive");
    }

    /*
     * Where it has to be, computed the way the loader computes it and
     * checked against what the archive says the program has. A record
     * outside that is a record in memory the loader no longer maps.
     */
    rec = (uintptr_t)m->bm_load + m->bm_mem - PAGE_SIZE;

    if (rec < (uintptr_t)m->bm_load + m->bm_rw) {
        panic("startup: the record would be in read-only memory");
    }

    cnprintf("    %-38s 0x%lx, %lu pages into %lu\n",
      "the record sits at", (unsigned long)rec,
      (unsigned long)((m->bm_mem - PAGE_SIZE) / PAGE_SIZE),
      (unsigned long)(m->bm_mem / PAGE_SIZE));

    hex_into(addr, rec);
    argv[0] = "poke_addr";
    argv[1] = addr;

    sched_defer();
    if (proc_spawn_op(self, "poke_addr", PRIO_BOOT - 1, NULL, 0,
      argv, 2, &h) != SYS_OK) {
        panic("startup: could not start poke_addr");
    }
    if (handle_lookup(ht, h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
        panic("startup: the spawn returned no process");
    }

#if SYS_TIER == 2
    /*
     * Where the loader actually put it, against where the archive says
     * it has to be. Poking the address alone does not settle this:
     * proc_prot makes the last page of a program's memory read-only
     * wherever the record went, so a record placed somewhere else would
     * leave this page read-only, the poke would fault, and the check
     * would pass with the record misplaced. Comparing the addresses is
     * what makes the fault below mean what it says.
     *
     * Tier 2 only, because at Tier 0 the record is not in the
     * program's memory at all: there is one address space and the
     * record lives in the process object, which a program can read
     * because nothing stops it. Section 15.2 is the same division
     * everywhere else in this milestone.
     */
    if (((struct proc *)obj)->p_startup_va != rec) {
        panic("startup: the loader put the record at 0x%lx, not 0x%lx",
          (unsigned long)((struct proc *)obj)->p_startup_va,
          (unsigned long)rec);
    }
#endif

    child_watch(&c, obj);
    sched_undefer();

    (void)child_join(&c, JOIN_MS);

    memset(&sargs, 0, sizeof(sargs));
    if (run(SYS_OP_PROC_STATUS, h, &sargs, sizeof(sargs)) != SYS_OK) {
        panic("startup: no status for poke_addr");
    }

#if SYS_TIER == 2
    if (sargs.out_fault != SYS_FAULT_ACCESS) {
        panic("startup: a program wrote the record it was handed and "
          "lived, kind %u", (unsigned int)sargs.out_fault);
    }
    if (sargs.out_fault_addr != rec) {
        panic("startup: reported 0x%lx, wrote 0x%lx",
          (unsigned long)sargs.out_fault_addr, (unsigned long)rec);
    }
    cnprintf("    %-38s %s at 0x%lx\n", "a program writing its own record",
      fault_name(sargs.out_fault), (unsigned long)sargs.out_fault_addr);
#else
    if (sargs.out_fault != SYS_FAULT_NONE) {
        panic("startup: flat memory refused a write, kind %u",
          (unsigned int)sargs.out_fault);
    }
    cnprintf("    %-38s %s\n", "a program writing its own record",
      "permitted, because nothing here says no");
#endif

    expect("giving poke_addr's handle back", handle_close(ht, h), SYS_OK);
}


/*
 * The channel a deadline check sends on, and the one thread that ever
 * receives on it.
 */
static struct channel *dl_ch;

/*
 * A timeline nothing ever signals, so a wait on it is a sleep until the
 * deadline. Used by the server that answers too late.
 */
static struct timeline dl_tl;

/* And one for M17's checks to wait on, signalled by nothing. */
static struct timeline kill_tl;

/*
 * Signalled by the slow server once it has replied, so the check can
 * wait for it rather than spin: the boot thread outranks the server, so
 * a yield here would never let it run.
 */
static struct timeline dl_done;
static sys_err_t dl_served = SYS_ERR_INTERRUPTED;

static void
dl_server(void *arg)
{
    struct sys_msg msg, rep;
    sys_thread_id_t peer;
    uint64_t cookie;

    (void)arg;

    memset(&msg, 0, sizeof(msg));
    dl_served = channel_recv(dl_ch, &msg, &peer, &cookie);
    if (dl_served != SYS_OK) {
        thread_exit(1);
    }

    memset(&rep, 0, sizeof(rep));
    rep.handle = SYS_HANDLE_INVALID;
    (void)channel_reply(peer, &rep);

    thread_exit(0);
}

static volatile uint32_t dl_slow_took;
static sys_err_t dl_slow_reply = SYS_ERR_INTERRUPTED;

/*
 * A server that takes a message and answers too late.
 *
 * It waits past the client's deadline before replying, which is the
 * whole of section 12.1's second case: the client abandons work in
 * progress, and the server discovers it when the reply is refused.
 */
static void
dl_slow_server(void *arg)
{
    struct sys_msg msg, rep;
    sys_thread_id_t peer;
    uint64_t cookie;
    uint64_t until;

    (void)arg;

    memset(&msg, 0, sizeof(msg));
    if (channel_recv(dl_ch, &msg, &peer, &cookie) != SYS_OK) {
        thread_exit(1);
    }
    dl_slow_took = 1;

    /*
     * Past the deadline, by waiting on a timeline nothing will ever
     * signal. The client is blocked on its reply and this thread is the
     * only one that could answer, so a sleep here is the client's
     * deadline arriving.
     */
    until = time_now() + time_from_ms(DEADLINE_MS * 3);
    (void)timeline_wait(&dl_tl, 1, until);

    memset(&rep, 0, sizeof(rep));
    rep.handle = SYS_HANDLE_INVALID;
    dl_slow_reply = channel_reply(peer, &rep);
    (void)timeline_signal(&dl_done, 1);

    thread_exit(0);
}

/*
 * A server that takes a message and never answers.
 *
 * It takes the client's message, and with it the client's buffer, and
 * then waits past the client's deadline without answering. That is a
 * server that has stopped serving, which is what a client's deadline is
 * for, and it holds the buffer for as long as the client is willing to
 * wait.
 */
static void
dl_hold_server(void *arg)
{
    struct sys_msg msg;
    sys_thread_id_t peer;
    uint64_t cookie;

    (void)arg;

    memset(&msg, 0, sizeof(msg));
    if (channel_recv(dl_ch, &msg, &peer, &cookie) != SYS_OK) {
        thread_exit(1);
    }
    dl_slow_took = 1;

    /*
     * And then it holds, until past the client's deadline, and ends.
     * An earlier version blocked in a second receive for the rest of
     * the boot, which left a thread above the boot thread parked on
     * this channel and moved what a later check counts on the way from
     * an interrupt to a program. A check that outlives itself is a
     * check that breaks the next one.
     */
    (void)timeline_wait(&dl_tl, 1, time_now() + time_from_ms(DEADLINE_MS * 3));

    thread_exit(0);
}

static sys_err_t dl_race_reply = SYS_ERR_INTERRUPTED;

/* The client's deadline, so the server can hold the core past it. */
static volatile uint64_t dl_race_until;

/*
 * A server that answers in time and then keeps the core.
 *
 * It replies at once, which makes the client runnable, and then waits
 * past the client's deadline without giving the core up to it. The
 * client outranks nothing here: it is below this thread, so it does not
 * run until this one blocks, and the timer fires in between.
 *
 * That is the race of M15 stage five, arranged rather than waited for.
 * The reply happens first and the deadline arrives second, on a thread
 * that has its answer and has not yet been back to collect it.
 */
static void
dl_race_server(void *arg)
{
    struct sys_msg msg, rep;
    sys_thread_id_t peer;
    uint64_t cookie;

    (void)arg;

    memset(&msg, 0, sizeof(msg));
    if (channel_recv(dl_ch, &msg, &peer, &cookie) != SYS_OK) {
        thread_exit(1);
    }

    memset(&rep, 0, sizeof(rep));
    rep.handle = SYS_HANDLE_INVALID;
    dl_race_reply = channel_reply(peer, &rep);

    /*
     * Past the client's deadline, holding the core. Spun rather than
     * slept: blocking hands the core to the client at once, and then
     * it runs and collects its answer before the timer ever fires,
     * which is the ordinary case and not this one. Interrupts stay on
     * through a spin, so the timer arrives while the client is runnable
     * and has not run.
     */
    while (time_now() < dl_race_until) {
        md_cpu_relax();
    }

    (void)timeline_signal(&dl_done, 1);

    thread_exit(0);
}

/*
 * A deadline on a send, per M15 stage one.
 *
 * Section 14.3 has every blocking call take an absolute deadline and
 * gives SYS_DEADLINE_NOW as the one that polls. Section 12.1 says what
 * a send does with it: a client that finds no server and will not wait
 * was never queued, so the server never learns it existed.
 *
 * The flag is the half worth checking twice, and the two halves are the
 * same send with the same zeroed field. A deadline read unconditionally
 * would turn every program that clears its arguments into one that
 * polls, because SYS_DEADLINE_NOW is zero, so the flag has to be what
 * decides: with it, the send returns at once; without it, the send
 * waits for a server that is not there yet, which is what every client
 * written before deadlines existed does.
 */
static void
deadline_checks(void)
{
    struct proc *self = proc_current();
    struct sys_op_send_args args;
    struct sys_op_connect_args cargs;
    struct thread *srv;
    sys_handle_t h_recv, to;
    sys_err_t err;

    cnputs("deadlines: a send that will not wait\n");

    dl_ch = channel_create("deadline", self);
    if (dl_ch == NULL) {
        panic("deadline: no channel");
    }
    err = handle_insert(&self->p_handles, dl_ch, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &h_recv);
    expect("a channel nobody is receiving on yet", err, SYS_OK);

    memset(&cargs, 0, sizeof(cargs));
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a connection to it",
      run(SYS_OP_CHAN_CONNECT, h_recv, &cargs, sizeof(cargs)), SYS_OK);
    to = cargs.out_handle;

    /*
     * A deadline already past, with nobody at a receive point. Nothing
     * can complete this and the caller said it would not wait.
     */
    memset(&args, 0, sizeof(args));
    args.request.handle = SYS_HANDLE_INVALID;
    args.deadline = SYS_DEADLINE_NOW;

    {
        sys_op_t send;

        /*
         * Through the submission path, so that the entry is checked
         * before it runs the way a program's entry is. Sent through
         * the kernel's own helper instead, this would still return
         * TIMEOUT with the static check refusing every flagged send,
         * because the helper skips those checks.
         */
        memset(&send, 0, sizeof(send));
        send.opcode = SYS_OP_IPC_SEND;
        send.target = to;
        send.flags = SYS_OP_DEADLINE;
        send.arg_ptr = (uintptr_t)&args;
        send.arg_len = sizeof(args);

        expect("a send that will not wait, with nobody to answer",
          (sys_err_t)run_batch(&send, 1), SYS_ERR_TIMEOUT);
    }

    /*
     * And a deadline that has to actually arrive, per M15 stage two.
     * The poll above never blocked, so nothing so far has armed a timer
     * or taken a thread off a queue: this is the first send in this
     * system that waits and then stops waiting.
     *
     * How long it waited is read as well as what it returned. A
     * deadline already past, or a timer that fired at once, gives the
     * same error without the waiting having happened, and section
     * 12.1's claim is about a client that waited and gave up.
     */
    {
        sys_op_t send;
        uint64_t began, waited;
        uint64_t want = time_from_ms(DEADLINE_MS);

        memset(&args, 0, sizeof(args));
        args.request.handle = SYS_HANDLE_INVALID;
        args.deadline = time_now() + want;

        memset(&send, 0, sizeof(send));
        send.opcode = SYS_OP_IPC_SEND;
        send.target = to;
        send.flags = SYS_OP_DEADLINE;
        send.arg_ptr = (uintptr_t)&args;
        send.arg_len = sizeof(args);

        began = time_now();
        expect("a send that waits and gives up",
          (sys_err_t)run_batch(&send, 1), SYS_ERR_TIMEOUT);
        waited = time_now() - began;

        if (waited < want) {
            panic("deadline: it gave up after %lu ticks, before its %lu",
              (unsigned long)waited, (unsigned long)want);
        }

        /*
         * The floor is asserted and the figure is printed. That it
         * waited at least as long as it said is the kernel's; how long
         * after its deadline it ran again is the host's, which is M9's
         * rule.
         */
        cnprintf("    %-38s %lu ms asked for, %lu ticks waited\n",
          "it waited before giving up", (unsigned long)DEADLINE_MS,
          (unsigned long)waited);

        /*
         * And nothing was left behind. Section 12.1 calls this the
         * clean case: the client was taken off the queue, nothing was
         * delivered, and nothing was allocated on its behalf, so a
         * server arriving afterwards would find an empty channel rather
         * than a message from a thread that has gone.
         *
         * Read off the channel rather than by having a server try to
         * receive, because a receive carries no deadline: a server that
         * found nothing would be a thread that never came back.
         */
        if (dl_ch->c_senders != NULL) {
            panic("deadline: a client that gave up is still queued");
        }
    }

    cnprintf("    %-38s %s\n", "what it left on the channel", "nothing");

    /*
     * And a deadline on something that cannot wait, which is a caller
     * saying what the operation does not do. Section 14.3 has every
     * blocking call take one; a fence wait carries its own and a
     * receive deliberately has none, so today the list is one operation
     * long. Refused rather than ignored, for the reason section 17.4's
     * result register is.
     */
    {
        sys_op_t nop;

        /*
         * Through the submission path rather than through the kernel's
         * own helper, because that is where an entry is checked before
         * it runs: the helper is the kernel calling an operation and
         * skips the static checks a program's entry gets. Asking the
         * wrong path made this read OK the first time it ran.
         */
        memset(&nop, 0, sizeof(nop));
        nop.opcode = SYS_OP_NOP;
        nop.flags = SYS_OP_DEADLINE;

        expect("a deadline on an operation that cannot wait",
          (sys_err_t)run_batch(&nop, 1), SYS_ERR_NOT_SUPPORTED);
    }

    /*
     * The same send, the same zeroed deadline, and no flag. This one
     * has to wait, so a server is started below it: if the field were
     * read anyway, this would return SYS_ERR_TIMEOUT before the server
     * ever ran.
     */
    srv = thread_create("dl-server", PRIO_BOOT - 1, dl_server, NULL);
    sched_ready(srv);

    memset(&args, 0, sizeof(args));
    args.request.handle = SYS_HANDLE_INVALID;
    args.deadline = SYS_DEADLINE_NOW;

    expect("the same send with the field left alone",
      run(SYS_OP_IPC_SEND, to, &args, sizeof(args)), SYS_OK);
    expect("and the server it waited for", dl_served, SYS_OK);

    /*
     * And a send with a deadline that is answered before it arrives,
     * which is the case the other three do not reach: every send above
     * either polls or gives up, so none of them leaves a timer armed
     * for a thread that goes on to be served.
     *
     * It is what makes the cancel after the block matter. A served
     * sender that kept its registered deadline would have it fire later
     * against a thread that is running, or blocked on something else,
     * and section 14.3's own rule is that whichever of the two happens
     * first leaves the other stale.
     */
    {
        sys_op_t send;

        dl_served = SYS_ERR_INTERRUPTED;
        srv = thread_create("dl-answer", PRIO_BOOT - 1, dl_server, NULL);
        sched_ready(srv);

        memset(&args, 0, sizeof(args));
        args.request.handle = SYS_HANDLE_INVALID;
        args.deadline = time_now() + time_from_ms(DEADLINE_MS);

        memset(&send, 0, sizeof(send));
        send.opcode = SYS_OP_IPC_SEND;
        send.target = to;
        send.flags = SYS_OP_DEADLINE;
        send.arg_ptr = (uintptr_t)&args;
        send.arg_len = sizeof(args);

        expect("a send with a deadline it did not need",
          (sys_err_t)run_batch(&send, 1), SYS_OK);
        expect("and the server that answered it", dl_served, SYS_OK);
    }

    /*
     * And the other case, per M15 stage three: a client whose message a
     * server is already holding. Section 12.1 has it abandon work in
     * progress rather than withdraw a request nobody saw, so the server
     * is not told and finds out when it replies.
     */
    {
        sys_op_t send;
        struct pool *conns = connection_pool();
        uint32_t before = conns->used;

        timeline_init(&dl_tl, "dl-late");
        timeline_init(&dl_done, "dl-done");

        dl_slow_took = 0;
        dl_slow_reply = SYS_ERR_INTERRUPTED;
        /*
         * Above this thread, so that it runs the moment it is readied
         * and is already blocked in receive when the send below
         * arrives. That is what makes this the REPLY-blocked case: at a
         * lower priority it would not have run yet, the send would
         * queue, and the timer would be armed on the way through
         * THREAD_WAIT_SEND instead. The first version of this check did
         * exactly that and tested stage two twice.
         */
        srv = thread_create("dl-slow", PRIO_BOOT + 1, dl_slow_server, NULL);
        sched_ready(srv);

        memset(&args, 0, sizeof(args));
        args.request.handle = SYS_HANDLE_INVALID;
        args.deadline = time_now() + time_from_ms(DEADLINE_MS);

        memset(&send, 0, sizeof(send));
        send.opcode = SYS_OP_IPC_SEND;
        send.target = to;
        send.flags = SYS_OP_DEADLINE;
        send.arg_ptr = (uintptr_t)&args;
        send.arg_len = sizeof(args);

        expect("a send whose server answers too late",
          (sys_err_t)run_batch(&send, 1), SYS_ERR_TIMEOUT);

        /*
         * That the server had it is the difference between this case
         * and the last one. Without it this would be stage two again
         * with a slower server.
         */
        if (dl_slow_took == 0) {
            panic("deadline: the server never took the message");
        }

        /*
         * And this thread is no longer pointing at the server it was
         * donating to. The priority itself is not moved here, because
         * the server outranks this thread and donation only ever
         * raises, but the relationship is set on every rendezvous and
         * has to be undone by whatever ends one.
         */
        if (thread_current()->t_donee != NULL) {
            panic("deadline: it gave up still donating to its server");
        }

        /*
         * And the reply, which arrives after this thread gave up.
         * Waited for on a timeline rather than spun on: this thread
         * outranks the server, so a yield would never let it run.
         */
        expect("waiting for the server to answer",
          timeline_wait(&dl_done, 1,
            time_now() + time_from_ms(JOIN_MS)), SYS_OK);
        expect("and the reply it made afterwards", dl_slow_reply,
          SYS_ERR_STALE);

        if (conns->used != before) {
            panic("deadline: %u connection records left over",
              (unsigned int)(conns->used - before));
        }
        /*
         * And the channel has nobody inside a request on it. A server
         * whose reply was refused is finished with that request as
         * surely as one whose reply landed, and the list chain_raise
         * walks has to say so.
         */
        if (dl_ch->c_serving != NULL) {
            panic("deadline: the channel still has a server in a request");
        }

        cnprintf("    %-38s %s\n", "what the server was left holding",
          "nothing it cannot give back");
    }

    /*
     * And a client that lent memory, per M15 stage four. Section 6.1
     * hands the buffer to the server for the length of the call, which
     * at Tier 2 is a real mapping: the pages leave the client's address
     * space. A client that gives up has to get them back with the
     * permissions they had, and the program is what says it did, by
     * writing them afterwards and reading them back.
     *
     * A user program rather than a thread here, because a kernel thread
     * shares the kernel's address space and there is nothing to take
     * away from it: what this stage is about only exists between two
     * address spaces.
     */
    {
        struct sys_op_connect_args pargs;
        sys_ns_spec_t ns[1];
        uint64_t began, took;
        sys_handle_t h;
        struct child c;
        void *obj;
        int32_t code;

        memset(&pargs, 0, sizeof(pargs));
        pargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
        expect("a connection for the program that lends",
          run(SYS_OP_CHAN_CONNECT, h_recv, &pargs, sizeof(pargs)), SYS_OK);

        dl_slow_took = 0;
        srv = thread_create("dl-hold", PRIO_BOOT + 1, dl_hold_server, NULL);
        sched_ready(srv);

        /*
         * Nothing is handed over any more, per M16 stage four. The
         * program computes its own instant from the counter and the
         * frequency in its system page, and the check's part is to
         * know how long that instant is meant to be and to say that
         * the program did not come back before it.
         */
        memset(ns, 0, sizeof(ns));
        ns[0].id = SYS_NS_SERVICE;
        ns[0].handle = pargs.out_handle;

        began = time_now();

        sched_defer();
        if (proc_spawn_op(proc_current(), "patient", PRIO_BOOT - 1, ns, 1,
          NULL, 0, &h) != SYS_OK) {
            panic("deadline: could not start the program that lends");
        }
        if (handle_lookup(&self->p_handles, h, SYS_OBJ_PROCESS, 0,
          &obj) != SYS_OK) {
            panic("deadline: the spawn returned no process");
        }
        child_watch(&c, obj);
        sched_undefer();

        code = child_join(&c, JOIN_MS);
        took = time_now() - began;

        if (dl_slow_took == 0) {
            panic("deadline: the holder never took the message");
        }
        if (code != 0) {
            const char *why;

            switch (code) {
            case 1: why = "it has no page, so no frequency"; break;
            case 2: why = "it was given no service to send to"; break;
            case 3: why = "its send did not time out"; break;
            case 4: why = "its buffer came back changed"; break;
            case 5: why = "it gave up before its own deadline"; break;
            default: why = "it did not finish"; break;
            }
            panic("deadline: the program that lent its buffer: %s, "
              "status %d", why, code);
        }

        /*
         * And it did not come back early, which is the whole of what
         * says the deadline it made was the one it meant. A program
         * that read no clock and passed SYS_DEADLINE_NOW gets the same
         * SYS_ERR_TIMEOUT and writes its buffer just the same, and the
         * only thing that tells the two apart is that one of them
         * waited. Measured around the spawn as well as the run, so the
         * floor is the deadline and not a tight bound on it: what is
         * asserted is that it did not return sooner, per M9 and per
         * M15's own send.
         */
        if (took < time_from_ms(DEADLINE_MS)) {
            panic("deadline: the program was back in %lu ticks, inside "
              "the %lu it asked to wait", (unsigned long)took,
              (unsigned long)time_from_ms(DEADLINE_MS));
        }

        cnprintf("    %-38s %s\n", "a client that lent and gave up",
          "wrote its buffer afterwards");
        cnprintf("    %-38s %lu ms of its own, %lu ticks gone\n",
          "a deadline the program made itself",
          (unsigned long)DEADLINE_MS, (unsigned long)took);

        expect("giving its handle back",
          handle_close(&self->p_handles, h), SYS_OK);

        /*
         * And once the holder has ended, it is not still inside a
         * request on this channel. It exits without ever replying,
         * which is the one thing that would otherwise take a thread off
         * that list, and the list is what a priority chain walks into.
         *
         * Waited for on its completion rather than assumed: until it
         * ends it is still serving, and asserting before that asserts
         * the opposite of what is true.
         */
        expect("waiting for the holder to end",
          timeline_wait(&srv->t_completion, SYS_COMPLETION_EXITED,
            time_now() + time_from_ms(JOIN_MS)), SYS_OK);

        if (dl_ch->c_serving != NULL) {
            panic("deadline: %s ended and %s is still being served",
              "the holder", dl_ch->c_serving->t_name);
        }
    }

    /*
     * And the race, per M15 stage five: the reply and the deadline
     * arriving together. Arranged rather than waited for. The server
     * outranks this thread, so it replies, this thread becomes runnable
     * without running, and the server then holds the core past the
     * deadline. The timer fires on a thread that already has its
     * answer.
     *
     * Without a guard the timer overwrites that answer with
     * SYS_ERR_TIMEOUT and enqueues a thread that is already on a run
     * queue, which enqueue panics on. What is asserted is that exactly
     * one of the two happened: the reply.
     */
    {
        sys_op_t send;

        timeline_init(&dl_done, "dl-race-done");
        dl_race_reply = SYS_ERR_INTERRUPTED;

        srv = thread_create("dl-race", PRIO_BOOT + 1, dl_race_server, NULL);
        sched_ready(srv);

        memset(&args, 0, sizeof(args));
        args.request.handle = SYS_HANDLE_INVALID;
        args.deadline = time_now() + time_from_ms(DEADLINE_MS);
        dl_race_until = args.deadline + time_from_ms(DEADLINE_MS);

        memset(&send, 0, sizeof(send));
        send.opcode = SYS_OP_IPC_SEND;
        send.target = to;
        send.flags = SYS_OP_DEADLINE;
        send.arg_ptr = (uintptr_t)&args;
        send.arg_len = sizeof(args);

        expect("a send answered just before its deadline",
          (sys_err_t)run_batch(&send, 1), SYS_OK);
        expect("and the reply that answered it", dl_race_reply, SYS_OK);

        /*
         * And the deadline really did arrive while this thread held its
         * answer. Without that the check is a send that was answered,
         * which stage two already makes.
         */
        if (time_now() < args.deadline) {
            panic("deadline: it returned before its own deadline");
        }

        expect("waiting for the server to finish",
          timeline_wait(&dl_done, 1,
            time_now() + time_from_ms(JOIN_MS)), SYS_OK);
    }

    cnprintf("    %-38s %s\n", "what decides whether it waits",
      "the flag, not the field");

    expect("giving the connection back",
      handle_close(&self->p_handles, to), SYS_OK);
    expect("giving the channel back",
      handle_close(&self->p_handles, h_recv), SYS_OK);
}
/*
 * Run tick, wait for it to be reaped, and answer with what it read.
 *
 * A function because the check runs it twice: once to say the page is
 * there and says the right thing, and again after a process that had
 * the page mapped has been reaped, to say the reap left it alone.
 */
static int32_t
syspage_tick(void)
{
    struct child c;
    sys_time_t deadline;
    sys_id_t id;
    int32_t code;

    /*
     * Spawned the way the frame checks spawn, and not through
     * SYS_OP_PROC_SPAWN, because this waits for the reap and that
     * spawn cannot give it one. The operation refuses a priority above
     * the caller's, so its child runs below the boot thread; a child
     * advances its completion before it sets itself dead, and a boot
     * thread that spins waiting for the reap outranks it and never
     * lets it get there. It was watched not being reaped for thirty
     * seconds. Spawning above the waiter is what makes the wait
     * terminate.
     */
    sched_defer();
    child_watch(&c, proc_spawn_image("tick", 20, NULL, 0, NULL, 0));
    id = c.c_proc->p_id;
    sched_undefer();

    code = child_join(&c, JOIN_MS);

    /*
     * And wait for the reap, per M13: the join says the process has
     * exited and closing the handles says nothing can ask about it,
     * but its last thread is parked rather than collected until the
     * top of the next switch. The slot going back is the reap exactly,
     * so the identifier going stale is the event to wait for.
     */
    deadline = time_now() + time_from_ms(JOIN_MS);
    while (pool_lookup(proc_pool(), id) != NULL) {
        if (time_now() > deadline) {
            panic("syspage: tick exited and was never reaped");
        }
        sched_yield();
    }

    return code;
}

/*
 * The system page of section 6.2, per M16 stage two.
 *
 * Two questions, and the second is the one that makes the first mean
 * anything. A program reads a field and gets what the kernel put there,
 * which says the mapping exists and points at the right frame. A
 * program writes to it and is refused, which says the mapping is
 * read-only rather than merely present: a page a program could write
 * would be a program that can move every other program's clock.
 *
 * The address is not written down here or there. The kernel puts it in
 * the startup record, libc keeps it, and the program asks for it, so a
 * program that never looks at its record cannot reach the page at all.
 * What this check knows is where the loader puts it, which is the end
 * of the slot, and that is what it points poke_addr at.
 */
static void
syspage_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    const struct boot_member *m;
    struct sys_op_status_args sargs;
    int32_t want, got;
    char addr[19];
    const char *argv[2];
    uintptr_t where;
    sys_handle_t h;
    struct child c;
    void *obj;

    cnputs("the system page: what a program can read of the clock\n");

    /*
     * The frequency and not a token, because a token would pass against
     * a page of zeroes with a version written into it. This is the one
     * number the kernel and the program can both name, and comparing it
     * is how the check knows the program read the frame the kernel
     * filled rather than some other page that happened to be mapped.
     */
    want = (int32_t)(time_hz() & 0x7fffffffu);
    got = syspage_tick();
    if (got != want) {
        panic("syspage: tick said %ld, the kernel has %ld",
          (long)got, (long)want);
    }
    cnprintf("    %-38s %lu ticks a second\n", "what the program read",
      (unsigned long)time_hz());

    /*
     * And the write. poke_addr is pointed at where its own page is,
     * which is the end of its slot: one frame past everything the image
     * asked for, because that is where proc_load puts it.
     */
    m = boot_lookup("poke_addr");
    if (m == NULL) {
        panic("syspage: no poke_addr in the archive");
    }

#if SYS_TIER == 2
    where = (uintptr_t)m->bm_load + m->bm_mem;
#else
    where = syspage_frame();
#endif
    hex_into(addr, where);

    argv[0] = "poke_addr";
    argv[1] = addr;

    sched_defer();
    if (proc_spawn_op(self, "poke_addr", PRIO_BOOT - 1, NULL, 0,
      argv, 2, &h) != SYS_OK) {
        panic("syspage: could not start poke_addr");
    }
    if (handle_lookup(ht, h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
        panic("syspage: the spawn returned no process");
    }
    child_watch(&c, obj);
    sched_undefer();

    (void)child_join(&c, JOIN_MS);

    memset(&sargs, 0, sizeof(sargs));
    if (run(SYS_OP_PROC_STATUS, h, &sargs, sizeof(sargs)) != SYS_OK) {
        panic("syspage: no status for poke_addr");
    }

#if SYS_TIER == 2
    if (sargs.out_fault != SYS_FAULT_ACCESS) {
        panic("syspage: a program wrote the clock and lived, kind %u",
          (unsigned int)sargs.out_fault);
    }

    /*
     * The address as well as the kind. What it rules out is a fault
     * somewhere else wearing the same kind, which a program stopped
     * for some other reason would give. It does not rule out a loader
     * that mapped nothing here, because an unmapped page refuses a
     * write at the same address for the same kind: what rules that out
     * is the read above, and the two are one check in two parts.
     */
    if (sargs.out_fault_addr != where) {
        panic("syspage: reported 0x%lx, wrote 0x%lx",
          (unsigned long)sargs.out_fault_addr, (unsigned long)where);
    }
    cnprintf("    %-38s %s at 0x%lx\n", "a program writing the clock",
      fault_name(sargs.out_fault),
      (unsigned long)sargs.out_fault_addr);
#else
    /*
     * Flat memory permits it, and says so rather than being left out.
     * There is one frame and every process sees the same one, so at
     * Tier 0 a program really can move the clock everything else
     * reads. Section 13.3 already names the tier as the tier without
     * protection, and this is one more thing it does not protect.
     */
    if (sargs.out_fault != SYS_FAULT_NONE) {
        panic("syspage: flat memory refused a write, kind %u",
          (unsigned int)sargs.out_fault);
    }

    /*
     * And the write landed on the page everything reads, which is the
     * part worth asserting. "Permitted" on its own would also be true
     * of a store that went into a copy nobody else sees, and the cost
     * being named here is that there are no copies: one frame, every
     * process, and any of them can move it.
     */
    {
        const sys_syspage_t *sp = (const sys_syspage_t *)syspage_frame();

        if (sp->version == SYS_SYSPAGE_VERSION) {
            panic("syspage: the write did not reach the shared page");
        }
        cnprintf("    %-38s %s\n", "a program writing the clock",
          "permitted, and the whole machine's page changed");

        /*
         * Put it back. The check broke a page the rest of this boot
         * reads, and leaving it broken would make every later stage
         * measure the damage rather than the system.
         */
        syspage_build();
        if (sp->version != SYS_SYSPAGE_VERSION || sp->hz != time_hz()) {
            panic("syspage: could not put the page back");
        }
    }
#endif

    expect("giving poke_addr's handle back", handle_close(ht, h), SYS_OK);

    /*
     * And once more, now that a process that had the page mapped has
     * been reaped. tick waits for its own reap before it returns, so
     * this runs after one has happened.
     * One frame serves every process, so a reap that gave it back would
     * hand the machine's clock to whatever asked for a frame next, and
     * nothing above would notice: the read happened before the reap,
     * and the write only says that something is mapped where the page
     * should be.
     */
    got = syspage_tick();
    if (got != want) {
        panic("syspage: after a reap tick said %ld, not %ld",
          (long)got, (long)want);
    }

    /*
     * And the allocator is asked outright, because reading the page
     * again is not enough and the break test is what showed it: a frame
     * that has been freed still holds what it held until something else
     * takes it, so the read passed with the page on the free list and
     * the failure surfaced stages later as a double free. page_take
     * answers false for a frame that is held, which is the answer
     * wanted here.
     */
    if (page_take(syspage_frame())) {
        panic("syspage: a reap gave the machine's page back");
    }
    cnprintf("    %-38s %s\n", "and after a reap of one of them",
      "the same page, and still held");
}
/*
 * What a status from the reader means. Shared by the two checks that
 * run it, because a bare number says nothing about which of a
 * program's several ways of failing happened.
 */
static const char *
clock_why(int32_t code)
{
    switch (code) {
    case -1: return "it read the clock going backwards";
    case -2: return "it read a clock behind the kernel's";
    case -3: return "it was handed no time to compare against";
    case -4: return "the gap was too wide for a status";
    case -5: return "it has no page, so no frequency";
    default: return "it did not survive reading the clock";
    }
}

/*
 * The clock a program can read, per M16 stage three.
 *
 * The kernel reads the counter, runs a program that reads it twice, and
 * reads it again. What the program returns is its second reading
 * measured from the kernel's first, so the two kernel readings bracket
 * it: nothing here trusts the program's arithmetic, because both ends
 * of the bracket are numbers this took itself.
 *
 * The bracket is what a weaker check would miss. A clock stuck at a
 * constant, a clock counting in nanoseconds rather than ticks, and a
 * clock reading some other counter all return a number; only a number
 * inside the span the kernel measured is the clock the kernel has.
 */
static void
clock_read_checks(void)
{
    struct child c;
    uint64_t before, after, span;
    char arg[19];
    const char *argv[2];
    int32_t code;

    cnputs("the clock: a program reading it without a system call\n");

    /*
     * One reading, handed to the program and kept for the comparison.
     * It was two at first, taken a few instructions apart, which made
     * the program measure from one instant and the kernel bound it
     * with another: not a bracket, and tighter than either end meant,
     * so an interrupt landing between the two reads would have failed
     * a clock that was right.
     */
    before = time_now();
    hex_into(arg, (uintptr_t)before);
    argv[0] = "clock";
    argv[1] = arg;

    sched_defer();
    child_watch(&c, proc_spawn_image("clock", 20, NULL, 0, argv, 2));

    /*
     * On the other core where there is one, because the permission to
     * read the counter is per core and the boot sets it in two places.
     * Running only on the core that starts the machine leaves the
     * second write untested, and a port that had it in one place would
     * pass everything here. The thread has not run yet: the defer is
     * what makes the move legal.
     */
    if (cpu_count() > 1) {
        expect("the reader moved to the second core",
          sched_migrate(proc_threads(c.c_proc), 1), SYS_OK);
    }

    sched_undefer();

    code = child_join(&c, JOIN_MS);
    after = time_now();

    if (code < 0) {
        /*
         * Named, because the interesting failures are the program's
         * own and a bare number would not say which. Anything else is
         * the kernel's status for a process that did not finish: with
         * the port's permission taken away the read is an illegal
         * instruction, and the program never reaches a return.
         */
        panic("the clock: %s, status %d", clock_why(code), (int)code);
    }

    span = after - before;

    /*
     * Strictly after the kernel's reading, which is the lower end of
     * the bracket and is the kernel's to assert. A spawn, a load and a
     * program's startup happen between the two, so a clock that has
     * not moved at all in that time has stopped. Without this a clock
     * frozen at exactly the value the program was handed would answer
     * zero and pass.
     */
    if (code == 0) {
        panic("the clock: it had not moved by the time the program "
          "read it");
    }
    if ((uint64_t)code > span) {
        panic("the clock: the program read %lu ticks past the kernel's "
          "start, which was %lu ticks before its end",
          (unsigned long)code, (unsigned long)span);
    }

    /*
     * Both numbers are durations and neither is asserted against a
     * figure, per M9. What is asserted is that one fell inside the
     * other, which is an ordering and is the same on every host.
     */
    cnprintf("    %-38s %lu of the kernel's %lu ticks\n",
      "where the program's reading fell", (unsigned long)code,
      (unsigned long)span);
    cnprintf("    %-38s about %lu us\n",
      "kernel to kernel, around the whole run",
      (unsigned long)(span * 1000000ULL / time_hz()));
}
/*
 * What reading the clock costs, per M16 stage five.
 *
 * Section 6.2's whole claim for the page and the counter is that a
 * program tells the time without asking the kernel. A count of
 * crossings is what says so, and M11 already keeps one per process:
 * p_submits is every time this process entered the kernel with work.
 *
 * So the same program is run twice, reading the clock once and reading
 * it CLOCK_READS times, and the two counts are compared. A
 * count that grew with the reads would mean the reading was a system
 * call wearing an inline face. The count is also asserted outright,
 * because equality alone would hold for two runs that both crossed a
 * thousand times.
 */
static uint64_t
clock_submits(uint64_t reads, uint64_t *took, int32_t *said)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct child c;
    const char *argv[3];
    char now[19], many[19];
    uint64_t began, submits;
    sys_handle_t keep;
    int32_t code;

    /*
     * One reading, handed over and kept, for the reason stage three's
     * check has one: the program compares its own first reading with
     * what it was given, and a second reading taken here would make
     * that comparison a different question from the one it looks like.
     */
    began = time_now();
    hex_into(now, (uintptr_t)began);
    hex_into(many, (uintptr_t)reads);
    argv[0] = "clock";
    argv[1] = now;
    argv[2] = many;

    sched_defer();
    child_watch(&c, proc_spawn_image("clock", 20, NULL, 0, argv, 3));

    /*
     * A handle of this check's own, held across the join. child_join
     * closes the two it made, and the count has to be read from a
     * process that is still there: without this the reap is free to
     * happen between the join and the read.
     */
    if (handle_insert(ht, c.c_proc, SYS_OBJ_PROCESS, SYS_RIGHT_STAT,
      &keep) != SYS_OK) {
        panic("the clock: no handle slot to hold the reader with");
    }
    sched_undefer();

    code = child_join(&c, JOIN_MS);
    *took = time_now() - began;

    if (code <= 0) {
        panic("the clock: the reader that was to read %lu times: %s, "
          "status %d", (unsigned long)reads, clock_why(code), (int)code);
    }

    *said = code;

    /*
     * And it took no recoverable fault either. p_submits counts system
     * calls, so on its own it would say nothing about a kernel that
     * trapped the instruction and emulated it. This kernel has no such
     * path: a user fault either goes to a pager, which this program
     * has none of and which this number would show, or it ends the
     * program, which is what taking the port's permission away does.
     */
    if (c.c_proc->p_faults != 0) {
        panic("the clock: the reader faulted %lu times reading it",
          (unsigned long)c.c_proc->p_faults);
    }

    submits = c.c_proc->p_submits;
    expect("giving the reader's handle back", handle_close(ht, keep),
      SYS_OK);

    return submits;
}

static void
clock_cost_checks(void)
{
    uint64_t once, many, t_once, t_many;
    int32_t n_once, n_many;

    cnputs("the clock: what reading it costs\n");

    once = clock_submits(0, &t_once, &n_once);
    many = clock_submits(CLOCK_READS, &t_many, &n_many);

    if (once != many) {
        panic("the clock: %lu crossings for one reading and %lu for "
          "%lu of them", (unsigned long)once, (unsigned long)many,
          (unsigned long)CLOCK_READS);
    }

    /*
     * One, and named rather than left as whatever came back. A program
     * that reads the clock and exits enters the kernel to exit and for
     * nothing else, which is M11's kind of number: the same on both
     * ports, at both tiers and in every run.
     */
    if (once != 1) {
        panic("the clock: a program that reads the clock and exits "
          "crossed %lu times", (unsigned long)once);
    }

    cnprintf("    %-38s %lu, and %lu for %lu of them\n",
      "crossings for one reading", (unsigned long)once,
      (unsigned long)many, (unsigned long)CLOCK_READS);

    /*
     * And the reads happened, which the crossings cannot say: a
     * program that ignored the argument would report the same one
     * crossing. The program answers with how many times it read, which
     * is a count and is the same on every host. Timing it and asking
     * whether the longer run took longer was tried first and is not a
     * check: with the loop taken out, the two runs differed by
     * whatever the spawns happened to cost and it passed.
     */
    if (n_once != 2 || n_many != (int32_t)(CLOCK_READS + 2)) {
        panic("the clock: %ld readings where 2 were asked for, and %ld "
          "where %lu were", (long)n_once, (long)n_many,
          (unsigned long)(CLOCK_READS + 2));
    }

    cnprintf("    %-38s %ld and %ld\n", "readings the two runs took",
      (long)n_once, (long)n_many);

    /*
     * The durations are printed and not asserted, per M9: how much
     * longer that many reads take is the host's business.
     */
    cnprintf("    %-38s %lu ticks against %lu\n",
      "and the run that read them took", (unsigned long)t_many,
      (unsigned long)t_once);
}
/*
 * What a kill reaches, per M17 stages one and two.
 *
 * `spin` computes for eighty milliseconds and makes one submission in
 * its life, to exit. Between those it is a program doing arithmetic,
 * which is the case section 10.5's promptness is written for.
 *
 * Stage one asserted the defect here: the program was killed and
 * finished its loop anyway, because the mark a kill leaves was read in
 * one place, the entry boundary inside a submission, and this program
 * reaches one only on its way out. It ran on for 59 to 68 milliseconds
 * after being killed.
 *
 * The mark is read on every way back to user mode now, and the same
 * arrangement asserts the opposite. What catches this program is not a
 * new trap: it was interrupted before the kill, which is how the check
 * got the core back to do the killing, so it is sitting in the kernel
 * inside that interrupt. The kill is what it finds on the way out of
 * it.
 *
 * Which is why the count below is zero and is asserted at zero. The
 * entry it left through was made before it was marked, so a check that
 * waited for a marked thread to enter the kernel would be waiting for
 * something that does not have to happen.
 */
static void
kill_reach_checks(void)
{
    struct handle_table *ht = &proc_current()->p_handles;
    struct child c;
    const struct thread *t;
    uint64_t before, killed, ran_on, entries;
    sys_handle_t keep;
    int32_t code;

    cnputs("a kill: what it reaches when nothing is blocked\n");

    timeline_init(&kill_tl, "kill-settle");

    /*
     * Below the boot thread, so that this check keeps the core until it
     * gives it up. Above it, the program would run to its end before
     * the kill was written, and the check would be of nothing.
     */
    sched_defer();
    child_watch(&c, proc_spawn_image("spin", PRIO_BOOT - 1, NULL, 0,
      NULL, 0));

    /*
     * A handle of this check's own, so the count on the process can be
     * read after the join, which closes the two the join made.
     */
    if (handle_insert(ht, c.c_proc, SYS_OBJ_PROCESS, SYS_RIGHT_STAT,
      &keep) != SYS_OK) {
        panic("a kill: no handle slot to hold the program with");
    }
    sched_undefer();

    /*
     * Long enough that the program is certainly running in user mode
     * and not still in its loader. A wait on a timeline nothing signals
     * is how the rest of this boot sleeps.
     */
    (void)timeline_wait(&kill_tl, 1, time_now() + time_from_ms(10));

    /*
     * Not blocked and not gone, which is what this stage is about: a
     * thread waiting on something would be released by the kill and
     * prove nothing. It does not say the program has run, because a
     * thread that has never been dispatched is READY too, and that
     * was tried: removing the wait above leaves the state exactly as
     * it is. What says the program ran is that it went on running
     * afterwards.
     */
    t = proc_threads(c.c_proc);
    if (t == NULL || (t->t_state != THREAD_READY &&
      t->t_state != THREAD_RUNNING)) {
        panic("a kill: the program was not computing, state %u",
          t == NULL ? 0u : (unsigned int)t->t_state);
    }

    entries = c.c_proc->p_dying_entries;
    before = time_now();

    if (proc_kill(c.c_proc, KILL_CODE) != SYS_OK) {
        panic("a kill: the process refused to be killed");
    }
    killed = time_now();

    code = child_join(&c, JOIN_MS);
    ran_on = time_now() - killed;

    /*
     * The code it was killed with, which says the kill decided how this
     * program ended. Its own zero would say it ran its loop out, which
     * is what it did until the mark was read on the way back to user
     * mode.
     */
    if (code != KILL_CODE) {
        panic("a kill: it came back with %d and not the %d it was "
          "killed with", (int)code, KILL_CODE);
    }

    cnprintf("    %-38s %s\n", "a program that computes and calls",
      "left on its way back to user mode");
    /*
     * Zero, and reported rather than asserted. The program never
     * enters the kernel after it is marked: it is already inside an
     * interrupt, taken before the kill, and it leaves through the
     * return from that. What could make it one is an unrelated
     * interrupt landing in the microseconds between, which would be a
     * correct system failing a check, so the number is printed and the
     * status is what is held to.
     */

    cnprintf("    %-38s %lu\n", "entries it made while marked",
      (unsigned long)(c.c_proc->p_dying_entries - entries));
    expect("giving the program's handle back", handle_close(ht, keep),
      SYS_OK);
    cnprintf("    %-38s %lu ticks, about %lu us\n",
      "how long it ran on afterwards", (unsigned long)ran_on,
      (unsigned long)(ran_on * 1000000ULL / time_hz()));

    /*
     * And it poked nothing, because nothing of it was running: the
     * killer had the core. That is the whole of why one core needs no
     * poke, and it is asserted here rather than argued.
     */
    if (c.c_proc->p_kill_pokes != 0) {
        panic("a kill: it poked %lu cores for a thread that was not "
          "running", (unsigned long)c.c_proc->p_kill_pokes);
    }
    cnprintf("    %-38s %lu ticks\n", "and the kill itself took",
      (unsigned long)(killed - before));
}
/*
 * What a kill does not reach, per M21 stage one.
 *
 * Section 10.5 says a kill unblocks every thread of its target
 * exactly as if each had been cancelled. `proc_kill` does that and
 * leaves the rest to the mark, on the assumption its own comment
 * states: a thread that is ready "has already stopped somewhere in
 * this kernel, and it finds the mark on the way back out". True of a
 * thread that was preempted and true of one that was released, both
 * of which return to user mode through a trap path that reads the
 * mark. False of a thread that has never run, because there is no
 * trap to return from and the trampoline into user mode does not
 * look.
 *
 * So this spawns a server, kills it before the scheduler can dispatch
 * it, and reports what happened, which is nothing. What is asserted
 * is the wrong answer: if a kill at this moment worked, this
 * milestone has no subject and should be abandoned rather than built.
 * Stage two is what turns the assertion over.
 *
 * The process was not left wedged either. A second kill reached it,
 * because by then it was blocked in a receive and a blocked thread
 * is what cancellation was built for, which is the shape of M20
 * stage five's workaround. That recovery is gone with the defect:
 * the first kill ends it now, and a second would be refused by a
 * process that has already exited.
 */
/*
 * One reading of where a process's first thread is, taken under the
 * kernel lock.
 *
 * Not because the fields tear, but because the thread can stop
 * existing between reading the list head and reading the state. A
 * thread that exits is taken off its process's list and its slot goes
 * back to the pool, and the whole point of the check below is that it
 * runs while a thread may be doing exactly that. Reading it any other
 * way is reading a pointer in the one case the check is about, which
 * is the mistake M20 made three times.
 *
 * A copy comes out and no pointer does, so nothing here is held once
 * the lock is put down.
 */
static bool
first_thread_state(const struct proc *p, uint32_t *state, uint32_t *wait,
  uint64_t *points, bool *exited)
{
    const struct thread *t;
    intr_state_t s;
    intr_state_t ks = 0;
    bool took = false;
    bool have;

    s = md_intr_disable();
    if (!klock_held()) {
        ks = klock_enter();
        took = true;
    }

    *exited = p->p_exited;
    t = proc_threads((struct proc *)p);
    have = t != NULL;
    if (have) {
        *state = (uint32_t)t->t_state;
        *wait = (uint32_t)t->t_wait;
        *points = t->t_points;
    }

    if (took) {
        klock_exit(ks);
    }
    md_intr_restore(s);

    return have;
}

static void
kill_unrun_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct sys_op_kill_args kargs;
    struct channel *ch;
    struct child c;
    sys_ns_spec_t ns[1];
    sys_handle_t give, h;
    uint64_t killed, waited;
    uint32_t state = 0;
    uint32_t wait = 0;
    uint64_t points = 0;
    bool exited = false;
    void *obj;
    int32_t code;

    cnputs("a kill: what it reaches in a thread that never ran\n");

    ch = channel_create("unrun", self);
    if (ch == NULL) {
        panic("unrun: no channel for the server");
    }
    if (handle_insert(ht, ch, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &give) != SYS_OK) {
        panic("unrun: no handle to give the server");
    }
    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_SERVICE;
    ns[0].handle = give;

    /*
     * Spawned and killed inside one deferral, which is what makes the
     * moment exact rather than hoped for. Without it the server may
     * reach its receive before the kill is written, and the check
     * would be of a blocked thread, which is the case that works.
     */
    sched_defer();
    if (proc_spawn_op(self, "echoes", PRIO_BOOT, ns, 1, NULL, 0,
      &h) != SYS_OK) {
        panic("unrun: no server");
    }
    if (handle_lookup(ht, h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
        panic("unrun: the spawn returned no process");
    }
    child_watch(&c, obj);

    if (!first_thread_state(c.c_proc, &state, &wait, &points,
      &exited) || state != THREAD_READY) {
        panic("unrun: the server is not ready and waiting, state %u",
          (unsigned int)state);
    }

    /*
     * And it has never been anywhere. Ready is not the same claim:
     * a thread that ran and was preempted is ready too, and that one
     * returns to user mode through a trap path which reads the mark,
     * so it is not the case this stage is about. A preemption point
     * is passed on the way into every entry, so a thread that has
     * passed none has not been in the kernel on its own account.
     */
    if (points != 0) {
        panic("unrun: the server has passed %lu preemption points, "
          "so it has run", (unsigned long)points);
    }

    memset(&kargs, 0, sizeof(kargs));
    kargs.code = KILL_CODE;
    if (run(SYS_OP_PROC_KILL, h, &kargs, sizeof(kargs)) != SYS_OK) {
        panic("unrun: the server refused to be killed");
    }
    if (!c.c_proc->p_dying) {
        panic("unrun: the kill left no mark");
    }

    /*
     * Read again, after the kill and before the core is let go.
     *
     * The reading above says what was true when the check looked; the
     * kill is a separate operation and the thread could have been
     * dispatched on the other core in between. That window is what
     * this milestone's *Watch for* is about: a check that kills too
     * late measures the case that already works and looks exactly
     * like one that passes. It is asserted rather than hoped for, at
     * the only moment where the answer means anything.
     */
    if (!first_thread_state(c.c_proc, &state, &wait, &points,
      &exited) || exited || state != THREAD_READY || points != 0) {
        panic("unrun: the kill landed on state %u with %lu points",
          (unsigned int)state, (unsigned long)points);
    }

    killed = time_now();
    sched_undefer();

    /*
     * And then it runs anyway. Waited for rather than sampled, because
     * a thread that has not been dispatched yet is not evidence of
     * anything: what is being shown is that it gets all the way to a
     * receive and stays there.
     */
    /*
     * And now it ends without ever having run, which is what stage
     * two built and what stage one asserted the opposite of. The join
     * is the statement: a supervisor that asked for this process to
     * stop is told it has, inside a deadline this check names rather
     * than inside however long the program felt like waiting.
     */
    code = child_join(&c, JOIN_MS);
    waited = time_now();
    if (code != KILL_CODE) {
        panic("unrun: it came back with %d, not %d", (int)code,
          (int)KILL_CODE);
    }

    /*
     * And nothing was poked, because nothing of it was running.
     * `proc_kill` pokes a thread that is running and leaves a ready
     * one to find the mark on its way out, which is the assumption
     * this whole stage is about. Asserted here so that a change which
     * started poking ready threads would have to say so: it would not
     * fix this, since a ready thread is on no core to be poked, and
     * it would move a number in the compared report.
     */
    if (c.c_proc->p_kill_pokes != 0) {
        panic("unrun: it poked %lu cores for a thread that had never "
          "run", (unsigned long)c.c_proc->p_kill_pokes);
    }
    /*
     * It made no entry, which is the second half of stage one's
     * finding gone. The count is of entries a marked thread started,
     * and a thread stopped before user mode starts none.
     */
    marked_entries += c.c_proc->p_dying_entries;
    if (c.c_proc->p_dying_entries != 0) {
        panic("unrun: it started %lu entries on its way out",
          (unsigned long)c.c_proc->p_dying_entries);
    }
    if (proc_unrun_programs == 0) {
        panic("unrun: it ended somewhere other than the trampoline");
    }
    if (handle_close(ht, h) != SYS_OK) {
        panic("unrun: the spawn handle was already gone");
    }

    cnprintf("    %-38s %s\n", "a server killed before it ran",
      "ended, and never reached user mode");
    cnprintf("    %-38s %lu\n", "entries it started while marked",
      (unsigned long)0);
    cnprintf("    %-38s about %lu us\n",
      "from the kill to the join returning",
      (unsigned long)((waited - killed) * 1000000ULL / time_hz()));
}

/*
 * A kill read before an entry rather than after one, per M21 stage
 * three.
 *
 * M17 read the mark at the end of an entry and gave the reason for
 * reading it at all: a killed thread must not run fifteen more
 * operations. What that placement cannot do is stop an entry which
 * blocks, because nothing follows it, and it cannot be ordered
 * against a kill: a thread that passes the mark's place and then
 * blocks has gone past the only question that would have stopped it,
 * and the cancel may already have been and gone.
 *
 * `twostep` is what shows the reading in place. It sends two
 * requests in one submission to a channel nothing answers, so it
 * blocks inside the first. The kill cancels that wait, the first
 * entry comes back interrupted, and the second entry is the one that
 * must never begin.
 *
 * That boundary is one the old placement also covered, from the
 * other side, and the count is what tells them apart. What the new
 * placement adds and this cannot reach is the boundary before the
 * first entry of a submission, which needs a thread that arrived
 * already marked. Nothing in this boot produces one: it takes a mark
 * written while the thread is in user mode on another core and a
 * trap of its own within the few microseconds before the kill's
 * interrupt lands, which was hit once in sixteen attempts while this
 * was being built and is not something to hold a boot to.
 */
/*
 * Receive a request, and not a pulse.
 *
 * A server that has outlived a client is told so on the channel it
 * was serving, and the telling is delivered by the same call as a
 * request. This check kills two clients and then waits for a third
 * to ask, so what came back first was `SYS_PULSE_DEAD_OWNER` and
 * the check read the makings of a thread out of it. Pulses are what
 * they are and M18 measures them; here they are in the way, and are
 * dropped. There is a sender queued by the time this is called, so
 * the receive that follows a pulse has something waiting for it.
 */
static void
recv_request(struct channel *ch, struct sys_msg *out,
  sys_thread_id_t *peer, const char *what)
{
    uint64_t cookie;

    for (;;) {
        if (channel_recv(ch, out, peer, &cookie) != SYS_OK) {
            panic("%s: the request would not come", what);
        }
        if ((out->flags & SYS_MSG_IS_PULSE) == 0) {
            return;
        }
    }
}

/*
 * Wait until something is queued to send on this channel.
 *
 * Before a receive rather than instead of one, because a receive
 * with nothing there blocks, and a check that blocks has no
 * deadline: the loop around it never comes back to look at the
 * clock, and a program that never sends stops the machine with no
 * cause attached. Asking first turns the wait into one this can
 * give up on, and leaves the receive with nothing to wait for.
 *
 * Read under the kernel lock, because the queue is a list the other
 * core puts threads on.
 */
static void
wait_queued(struct channel *ch, const char *what)
{
    uint64_t until = time_now() + time_from_ms(JOIN_MS);

    for (;;) {
        bool queued;
        intr_state_t is;
        intr_state_t ks = 0;
        bool took = false;

        is = md_intr_disable();
        if (!klock_held()) {
            ks = klock_enter();
            took = true;
        }
        queued = ch->c_senders != NULL;
        if (took) {
            klock_exit(ks);
        }
        md_intr_restore(is);

        if (queued) {
            return;
        }
        if (time_now() > until) {
            panic("%s: nothing was queued on %s", what, ch->c_name);
        }
        /*
         * Slept on rather than yielded to. A program runs below
         * this thread, so yielding here re-dispatches this one and
         * the program never gets the core at all.
         */
        (void)timeline_wait(&kill_tl, 1,
          time_now() + time_from_ms(2));
    }
}

static void
kill_boundary_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct sys_op_kill_args kargs;
    struct channel *ch;
    struct child c;
    sys_ns_spec_t ns[1];
    sys_handle_t give, keep, h;
    uint64_t before = proc_stopped_before_later;
    void *obj;
    int32_t code;

    cnputs("a kill: what it reaches between two entries\n");

    ch = channel_create("twostep", self);
    if (ch == NULL) {
        panic("boundary: no channel");
    }

    /*
     * A handle of this check's own, kept until the end.
     *
     * The other one is handed to the program, and since M20 that is
     * the only thing holding the channel: a program that exits early
     * takes the slot back with it, and the queue read below would be
     * read out of a slot something else has taken. One handle here
     * is what keeps the object under the pointer.
     */
    if (handle_insert(ht, ch, SYS_OBJ_CHANNEL, SYS_RIGHT_RECV,
      &keep) != SYS_OK) {
        panic("boundary: no handle to keep");
    }
    if (handle_insert(ht, ch, SYS_OBJ_CHANNEL,
      SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER, &give) != SYS_OK) {
        panic("boundary: no handle to give");
    }
    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_SERVICE;
    ns[0].handle = give;

    sched_defer();
    if (proc_spawn_op(self, "twostep", PRIO_BOOT - 1, ns, 1, NULL, 0,
      &h) != SYS_OK) {
        panic("boundary: no program");
    }
    if (handle_lookup(ht, h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
        panic("boundary: the spawn returned no process");
    }
    child_watch(&c, obj);
    sched_undefer();

    /*
     * Waited for rather than timed. What has to be true when the
     * kill is written is that the program is inside its first entry
     * and blocked there, and the queue on the channel is where that
     * shows: nothing receives here, so a sender on it is a thread
     * waiting inside a send.
     */
    wait_queued(ch, "boundary");

    memset(&kargs, 0, sizeof(kargs));
    kargs.code = KILL_CODE;
    if (run(SYS_OP_PROC_KILL, h, &kargs, sizeof(kargs)) != SYS_OK) {
        panic("boundary: it refused to be killed");
    }

    code = child_join(&c, JOIN_MS);
    if (code != KILL_CODE) {
        panic("boundary: it came back with %d, not %d", (int)code,
          (int)KILL_CODE);
    }
    if (handle_close(ht, h) != SYS_OK) {
        panic("boundary: the spawn handle was already gone");
    }
    if (handle_close(ht, keep) != SYS_OK) {
        panic("boundary: the channel handle was already gone");
    }

    if (proc_stopped_before_later == before) {
        panic("boundary: nothing was stopped before the second "
          "entry");
    }

    cnprintf("    %-38s %s\n", "a kill that released a waiting entry",
      "stopped the next one from starting");
    cnprintf("    %-38s %lu\n", "stopped before an entry by here",
      (unsigned long)proc_stopped_before_later);
}

/*
 * A kill that finds every state at once, per M21 stage four.
 *
 * Section 10.5 says a kill unblocks every thread of its target
 * exactly as if each had been cancelled. Four different mechanisms
 * make that true and until now each was checked on a process with
 * one thread in one state. A supervisor ending a driver cohort is
 * not in that position: it ends a process whose threads are wherever
 * they happened to be, and nothing had asked whether the four work
 * together.
 *
 * `fourways` puts one thread in each. The states and what reaches
 * them:
 *
 *   blocked receiving      thread_cancel, off the channel's list
 *   inside a request       thread_cancel, and the server is told
 *   running                the poke, and the mark read on the way
 *                          back from the interrupt it causes
 *   ready and never run    the mark read in the trampoline, per
 *                          stage two
 *
 * The last of those is why this stage is here rather than in M17.
 * Before stage two a process arranged like this could not be killed
 * at all: three threads would end and the fourth would go on to user
 * mode and run for ever, and the join would never return.
 */
#define FOURWAYS_THREADS    4u

/*
 * What the four threads are doing, counted in one walk.
 *
 * Counted twice: once before the kill, to say the arrangement the
 * stage is about was actually reached, and once if the kill fails
 * to end them, to say which state was left behind. The second is
 * the reason this is a function. A join that times out can only
 * report that the process did not finish, and the thing worth
 * knowing is which of the four mechanisms did not run.
 */
struct fourways_states {
    uint32_t n;
    uint32_t running;
    uint32_t unrun;
    uint32_t receiving;
    uint32_t inside;
    uint32_t computing;
    uint32_t dead;
};

static void
count_states(struct proc *p, struct fourways_states *out)
{
    intr_state_t is;
    intr_state_t ks = 0;
    struct thread *t;
    bool took = false;

    memset(out, 0, sizeof(*out));

    /*
     * Under the kernel lock, because on two cores the list and the
     * states on it belong to the other core.
     */
    is = md_intr_disable();
    if (!klock_held()) {
        ks = klock_enter();
        took = true;
    }
    for (t = proc_threads(p); t != NULL; t = t->t_sibling) {
        out->n++;
        if (t->t_state == THREAD_DEAD) {
            out->dead++;
        } else if (!t->t_user_entered) {
            /*
             * Never reached user mode, which is the state in
             * question and is not the same as never having entered
             * the kernel: the thread that computes has run and has
             * passed no preemption point either.
             */
            out->unrun++;
        } else if (t->t_state == THREAD_BLOCKED &&
          t->t_wait == THREAD_WAIT_RECV) {
            out->receiving++;
        } else if (t->t_state == THREAD_BLOCKED &&
          t->t_wait == THREAD_WAIT_REPLY) {
            out->inside++;
        } else {
            out->computing++;
            if (t->t_state == THREAD_RUNNING) {
                out->running++;
            }
        }
    }
    if (took) {
        klock_exit(ks);
    }
    md_intr_restore(is);
}

/*
 * A reference on each of the four, per M21 stage four's review.
 *
 * Taken while all four are still there, because what each one exits
 * with can only be read from the thread, and a thread is collected
 * as soon as it ends and nothing holds it. Without this the walk
 * after the kill finds an empty list, and the report's "all with
 * the kill's code" is a sentence rather than a measurement.
 */
static void
hold_states(struct proc *p, struct thread **held)
{
    intr_state_t is;
    intr_state_t ks = 0;
    struct thread *t;
    bool took = false;
    uint32_t i = 0;

    is = md_intr_disable();
    if (!klock_held()) {
        ks = klock_enter();
        took = true;
    }
    for (t = proc_threads(p); t != NULL; t = t->t_sibling) {
        if (i < FOURWAYS_THREADS) {
            thread_ref(t);
            held[i] = t;
        }
        i++;
    }
    if (took) {
        klock_exit(ks);
    }
    md_intr_restore(is);

    if (i != FOURWAYS_THREADS) {
        panic("states: %u threads to hold, not %u", (unsigned int)i,
          (unsigned int)FOURWAYS_THREADS);
    }
}

/* Whether one held thread has ended, and with what. */
static bool
thread_ended(struct thread *t, int32_t *code)
{
    intr_state_t is;
    intr_state_t ks = 0;
    bool took = false;
    bool dead;

    is = md_intr_disable();
    if (!klock_held()) {
        ks = klock_enter();
        took = true;
    }
    dead = t->t_state == THREAD_DEAD;
    *code = t->t_exit_code;
    if (took) {
        klock_exit(ks);
    }
    md_intr_restore(is);

    return dead;
}

#if SYS_TIER == 2
static volatile bool nudge_ran;

/*
 * Something for the other core to dispatch, per M21 stage four.
 *
 * A thread that ends leaves itself parked for the next dispatch on
 * the core it ran on to collect, and a core with nothing to run
 * takes no dispatch. So a program moved to the second core and
 * killed there leaves its last thread parked, that thread holds a
 * reference to its process, and the process is never reaped: the
 * check below waited thirty seconds for two channels that were
 * never going to come back. Measured, and the thread was there in
 * the pool, exited, with nothing holding it.
 *
 * This gives that core one thing to dispatch, which collects what
 * was parked on the way in. It is a property of how collection is
 * scheduled rather than of the kill, and it is written here because
 * this is the first check in the system to end a process on a core
 * that then has nothing to do.
 */
static void
nudge(void *arg)
{
    (void)arg;
    nudge_ran = true;
    thread_exit(0);
}

static void
nudge_other_core(void)
{
    struct thread *t;
    uint64_t until;

    nudge_ran = false;
    t = thread_create("nudge", PRIO_BOOT, nudge, NULL);
    sched_defer();
    if (sched_migrate(t, 1) != SYS_OK) {
        panic("states: the nudge would not move");
    }
    sched_ready(t);
    sched_undefer();

    until = time_now() + time_from_ms(JOIN_MS);
    while (!nudge_ran) {
        if (time_now() > until) {
            panic("states: the other core never dispatched");
        }
        (void)timeline_wait(&kill_tl, 1, time_now() + time_from_ms(2));
    }
}
#endif /* SYS_TIER == 2 */

static void
kill_states_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct sys_op_kill_args kargs;
    struct channel *serve, *store;
    struct child c;
    struct sys_msg msg;
    struct fourways_states st;
    struct thread *held[FOURWAYS_THREADS];
    struct sys_msg makings, answer;
    sys_thread_id_t peer_mk;
    uintptr_t entry, stack;
    sys_handle_t fourth;
    sys_thread_id_t peer;
    sys_ns_spec_t ns[2];
    sys_handle_t keep_s, keep_t, give_s, give_t, h;
    sys_id_t id_s, id_t;
    uint64_t until, killed;
    uint32_t was_running, ended = 0, i;
    void *obj;
    int32_t code, tcode;

    cnputs("a kill: four threads, one in each state it must reach\n");

    serve = channel_create("fourways recv", self);
    store = channel_create("fourways send", self);
    if (serve == NULL || store == NULL) {
        panic("states: no channels");
    }
    id_s = serve->c_id;
    id_t = store->c_id;

    /*
     * One handle of this check's own on each, so the objects stay
     * under the pointers read below whatever the program does with
     * the ones it was given.
     */
    if (handle_insert(ht, serve, SYS_OBJ_CHANNEL, SYS_RIGHT_RECV,
      &keep_s) != SYS_OK ||
      handle_insert(ht, store, SYS_OBJ_CHANNEL, SYS_RIGHT_RECV,
      &keep_t) != SYS_OK) {
        panic("states: no handles to keep");
    }
    if (handle_insert(ht, serve, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &give_s) != SYS_OK ||
      handle_insert(ht, store, SYS_OBJ_CHANNEL,
      SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER, &give_t) != SYS_OK) {
        panic("states: no handles to give");
    }

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_SERVICE;
    ns[0].handle = give_s;
    ns[1].id = SYS_NS_STORE;
    ns[1].handle = give_t;

    sched_defer();
    if (proc_spawn_op(self, "fourways", PRIO_BOOT - 1, ns, 2, NULL, 0,
      &h) != SYS_OK) {
        panic("states: no program");
    }
    if (handle_lookup(ht, h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
        panic("states: the spawn returned no process");
    }
    child_watch(&c, obj);

#if SYS_TIER == 2
    /*
     * The other core, before it has run, so that the thread which
     * computes is running rather than merely runnable when the kill
     * is written. Threads it makes go on the queue of the core it
     * runs on, so moving the first moves all four.
     */
    if (sched_migrate(proc_threads(c.c_proc), 1) != SYS_OK) {
        panic("states: the program would not move");
    }
#endif
    sched_undefer();

    /*
     * The makings of the fourth thread, which the program sends
     * before it makes anything else, and which are answered at once:
     * the answer is what lets the program go on.
     */
    wait_queued(store, "states: the makings");
    recv_request(store, &makings, &peer_mk, "states: the makings");
    if (makings.inline_len != 2 * sizeof(uint64_t)) {
        panic("states: the makings were %u bytes",
          (unsigned int)makings.inline_len);
    }
    {
        uint64_t got[2];

        memcpy(got, makings.inline_data, sizeof(got));
        entry = (uintptr_t)got[0];
        stack = (uintptr_t)got[1];
    }
    if (entry == 0 || stack == 0) {
        panic("states: the makings were empty");
    }
    memset(&answer, 0, sizeof(answer));
    answer.handle = SYS_HANDLE_INVALID;
    if (channel_reply(peer_mk, &answer) != SYS_OK) {
        panic("states: the makings were not answered");
    }

    /*
     * The asker's request, taken and not answered, which is what
     * leaves one thread inside a rendezvous. Taking it is also what
     * says the program has got that far.
     */
    wait_queued(store, "states");
    recv_request(store, &msg, &peer, "states: the asker");

    /*
     * And then the four are counted. Waited for rather than
     * sampled: the program reaches this arrangement in its own time
     * and a check that looked once would be checking the
     * scheduler's timing.
     */
    until = time_now() + time_from_ms(JOIN_MS);
    for (;;) {
        count_states(c.c_proc, &st);

        /*
         * On two cores the thread that computes has to be running
         * and not merely runnable, because the state being tested
         * is the one only a poke reaches. On one core the killer
         * has the core by definition, so it is runnable and the
         * poke is not what reaches it.
         */
        if (st.n == FOURWAYS_THREADS - 1 && st.unrun == 0 &&
          st.receiving == 1 && st.inside == 1 && st.computing == 1 &&
          st.running == (cpu_count() > 1 ? 1u : 0u)) {
            break;
        }
        if (time_now() > until) {
            panic("states: %u threads, %u computing of which %u "
              "running, %u never run, %u receiving, %u inside",
              (unsigned int)st.n, (unsigned int)st.computing,
              (unsigned int)st.running, (unsigned int)st.unrun,
              (unsigned int)st.receiving, (unsigned int)st.inside);
        }
        (void)timeline_wait(&kill_tl, 1, time_now() + time_from_ms(2));
    }

    /*
     * And the fourth, made here with dispatch held off and the kill
     * written before it is let go again. That is the whole of what
     * makes this thread one that has never run: a thread of the
     * program cannot arrange it, because the program is at the
     * lowest priority and anything it makes is at that priority
     * too, so the first time this thread wakes to look it puts the
     * one that computes behind the one that must not run.
     */
    sched_defer();

    if (proc_thread_create(c.c_proc, entry, 0, stack, PRIO_BOOT - 1,
      &fourth) != SYS_OK) {
        panic("states: the fourth thread was refused");
    }
    if (handle_close(&c.c_proc->p_handles, fourth) != SYS_OK) {
        panic("states: the fourth thread's handle was already gone");
    }

    count_states(c.c_proc, &st);
    if (st.n != FOURWAYS_THREADS || st.unrun != 1 ||
      st.receiving != 1 || st.inside != 1 || st.computing != 1) {
        panic("states: %u threads with the fourth made, %u computing, "
          "%u never run, %u receiving, %u inside", (unsigned int)st.n,
          (unsigned int)st.computing, (unsigned int)st.unrun,
          (unsigned int)st.receiving, (unsigned int)st.inside);
    }

    cnprintf("    %-38s %u computing, %u never run, %u receiving, "
      "%u inside\n", "the four it was about to reach",
      (unsigned int)st.computing, (unsigned int)st.unrun,
      (unsigned int)st.receiving, (unsigned int)st.inside);

    hold_states(c.c_proc, held);

    was_running = st.running;
    killed = time_now();
    memset(&kargs, 0, sizeof(kargs));
    kargs.code = KILL_CODE;
    if (run(SYS_OP_PROC_KILL, h, &kargs, sizeof(kargs)) != SYS_OK) {
        panic("states: it refused to be killed");
    }

    sched_undefer();

    /*
     * Each of the four, waited for and read here rather than left
     * to the join, because what this stage has to say when it fails
     * is which of the four states was left behind, and what it has
     * to say when it passes is what each thread exited with. The
     * join can say only that the process did not finish, and only
     * about the process.
     */
    until = time_now() + time_from_ms(JOIN_MS);
    for (i = 0; i < FOURWAYS_THREADS; i++) {
        while (!thread_ended(held[i], &tcode)) {
            if (time_now() > until) {
                count_states(c.c_proc, &st);
                panic("states: the kill left %u computing, %u never "
                  "run, %u receiving, %u inside, and ended %u",
                  (unsigned int)st.computing, (unsigned int)st.unrun,
                  (unsigned int)st.receiving, (unsigned int)st.inside,
                  (unsigned int)st.dead);
            }
            (void)timeline_wait(&kill_tl, 1,
              time_now() + time_from_ms(2));
        }
        if (tcode != KILL_CODE) {
            panic("states: a thread exited with %d, not %d",
              (int)tcode, (int)KILL_CODE);
        }
        ended++;
    }

    /*
     * And let go, which is also what collects them: a thread whose
     * last reference goes while it is dead is reaped there and then.
     */
    for (i = 0; i < FOURWAYS_THREADS; i++) {
        thread_unref(held[i]);
    }

    code = child_join(&c, JOIN_MS);
    if (code != KILL_CODE) {
        panic("states: it came back with %d, not %d", (int)code,
          (int)KILL_CODE);
    }
    if (c.c_proc->p_nthreads != 0) {
        panic("states: %u threads outlived the process",
          (unsigned int)c.c_proc->p_nthreads);
    }

    /*
     * And this thread comes off the channel it was serving, which
     * the asker's request put it on. The client is gone, so the
     * answer is refused rather than delivered, and refusing it is
     * what takes this thread off the list. Left there it would be a
     * boot thread serving one channel for the rest of the run,
     * which is the cross-channel case of section 19.3's unenforced
     * rule and the one thing this boot has never done.
     */
    if (channel_reply(peer, &msg) != SYS_ERR_STALE) {
        panic("states: the answer to a dead client was not refused");
    }

    /*
     * One poke per thread that was running, which is the rule M17
     * stated and the number that says the fourth mechanism was the
     * one used. On one core nothing of the target is running when
     * the killer is, so the count is zero and the state it stands
     * for is not tested there.
     */
    if (c.c_proc->p_kill_pokes != (cpu_count() > 1 ? 1u : 0u)) {
        panic("states: %lu pokes for %u running threads",
          (unsigned long)c.c_proc->p_kill_pokes,
          (unsigned int)was_running);
    }

    cnprintf("    %-38s %u of %u, all with the kill's code\n",
      "threads the kill ended", (unsigned int)ended,
      (unsigned int)FOURWAYS_THREADS);
    cnprintf("    %-38s %lu\n", "pokes it had to send",
      (unsigned long)c.c_proc->p_kill_pokes);
    cnprintf("    %-38s about %lu us\n", "from the kill to the join",
      (unsigned long)((time_now() - killed) * 1000000ULL /
      time_hz()));

    if (handle_close(ht, h) != SYS_OK ||
      handle_close(ht, keep_s) != SYS_OK ||
      handle_close(ht, keep_t) != SYS_OK) {
        panic("states: a handle was already gone");
    }

#if SYS_TIER == 2
    nudge_other_core();
#endif

    /*
     * And the reap, waited for rather than assumed. A join says the
     * threads have ended; the handle table they shared is closed
     * when the last thing that can ask about the process lets go,
     * and the collection happens at the top of some core's next
     * switch. The two channels are what says it happened, because
     * the only handles left on them are in that table.
     */
    until = time_now() + time_from_ms(JOIN_MS);
    while (slot_alive(id_s) || slot_alive(id_t)) {
        if (time_now() > until) {
            panic("states: the channels outlived the process");
        }
        (void)timeline_wait(&kill_tl, 1, time_now() + time_from_ms(2));
    }
}

/*
 * A thread asked for in a process that is already dying, per M21
 * stage five.
 *
 * Everything before this stage is about threads that existed when
 * the kill was written. This is about one that does not. Nothing
 * stopped a thread being made in a marked process: the kill's walk
 * had been and gone, so the new thread was not cancelled by it, and
 * what ended the thread was stage two's reading in the trampoline
 * it arrived on. That works, and it is not enough. It leaves the
 * set of threads a kill has to reach open after the kill, so a
 * supervisor waiting for a process to be over is waiting on
 * something the process can add to.
 *
 * So the create is refused, and this measures the same call twice,
 * once on each side of the mark. `lastborn` sends the makings of a
 * thread, an entry point in its own image and a stack for it, and
 * waits inside that request. The supervisor builds the thread from
 * what it was sent and the thread runs, which is the working case.
 * Then the kill, and the same call again, which is refused.
 *
 * Asking twice is the point. The arguments are the same both times
 * and they came from the program, so the only thing that differs
 * between the answers is the mark.
 */
/*
 * One `lastborn`, started and asked for the makings of a thread.
 *
 * Twice, because the stage asks the same question of a process that
 * was killed and of one that ended on its own, and the second is
 * not a variation on the first: nothing marked it, and what makes
 * it over is that its last thread has gone.
 */
/* What `lastborn` returns when its request was answered. */
#define LASTBORN_RETURN 3

struct newborn {
    struct child        c;
    struct sys_msg      msg;
    sys_thread_id_t     peer;
    sys_handle_t        h;
    uintptr_t           entry;
    uintptr_t           stack;
};

static void
start_lastborn(struct channel *serve, struct newborn *n)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    sys_ns_spec_t ns[1];
    sys_handle_t give;
    uint64_t got[2];
    void *obj;

    if (handle_insert(ht, serve, SYS_OBJ_CHANNEL,
      SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER, &give) != SYS_OK) {
        panic("newborn: no handle to give");
    }

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_SERVICE;
    ns[0].handle = give;

    sched_defer();
    if (proc_spawn_op(self, "lastborn", PRIO_BOOT - 1, ns, 1, NULL, 0,
      &n->h) != SYS_OK) {
        panic("newborn: no program");
    }
    if (handle_lookup(ht, n->h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
        panic("newborn: the spawn returned no process");
    }
    child_watch(&n->c, obj);
    sched_undefer();

    wait_queued(serve, "newborn");
    recv_request(serve, &n->msg, &n->peer, "newborn: the makings");
    if (n->msg.inline_len != sizeof(got)) {
        panic("newborn: the makings were %u bytes",
          (unsigned int)n->msg.inline_len);
    }

    memcpy(got, n->msg.inline_data, sizeof(got));
    n->entry = (uintptr_t)got[0];
    n->stack = (uintptr_t)got[1];
    if (n->entry == 0 || n->stack == 0) {
        panic("newborn: the makings were empty");
    }
}

static void
kill_newborn_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct sys_op_kill_args kargs;
    struct channel *serve;
    struct newborn killed, ended;
    struct sys_msg second, answer;
    sys_thread_id_t peer_kid;
    sys_handle_t keep, kid;
    sys_id_t id;
    uint64_t until, refused;
    sys_err_t err;
    int32_t code;

    cnputs("a kill: the thread a process that is over asks for\n");

    serve = channel_create("lastborn recv", self);
    if (serve == NULL) {
        panic("newborn: no channel");
    }
    id = serve->c_id;
    if (handle_insert(ht, serve, SYS_OBJ_CHANNEL, SYS_RIGHT_RECV,
      &keep) != SYS_OK) {
        panic("newborn: no handle to keep");
    }

    refused = proc_creates_refused;

    start_lastborn(serve, &killed);

    /*
     * The working case first, because the refusals below mean
     * nothing unless the same call works on the same process a
     * moment earlier.
     */
    err = proc_thread_create(killed.c.c_proc, killed.entry, 0,
      killed.stack, PRIO_BOOT - 1, &kid);
    if (err != SYS_OK) {
        panic("newborn: a live process refused a thread, %d",
          (int)err);
    }

    wait_queued(serve, "newborn: the thread that was made");
    recv_request(serve, &second, &peer_kid,
      "newborn: the thread that was made");
    if (second.inline_len != 1) {
        panic("newborn: the second message was %u bytes",
          (unsigned int)second.inline_len);
    }

    /*
     * And the handle to it goes back, into the table it was put in,
     * which is the program's own. A handle to a thread holds the
     * thread, a thread holds its process, and a process that names
     * itself is never collected.
     */
    if (handle_close(&killed.c.c_proc->p_handles, kid) != SYS_OK) {
        panic("newborn: the thread handle was already gone");
    }

    cnprintf("    %-38s %s\n", "a thread built from what it sent",
      "ran and asked on the same channel");

    memset(&kargs, 0, sizeof(kargs));
    kargs.code = KILL_CODE;
    if (run(SYS_OP_PROC_KILL, killed.h, &kargs, sizeof(kargs)) !=
      SYS_OK) {
        panic("newborn: it refused to be killed");
    }

    code = child_join(&killed.c, JOIN_MS);
    if (code != KILL_CODE) {
        panic("newborn: it came back with %d, not %d", (int)code,
          (int)KILL_CODE);
    }
    if (killed.c.c_proc->p_nthreads != 0) {
        panic("newborn: %u threads outlived the process",
          (unsigned int)killed.c.c_proc->p_nthreads);
    }

    /*
     * The same call, on the same process, with the same arguments.
     * After the join rather than straight after the kill, so that
     * what it is refused by is the mark and not a thread that has
     * yet to notice one: this process has ended, and the only thing
     * left of it is the memory.
     */
    err = proc_thread_create(killed.c.c_proc, killed.entry, 0,
      killed.stack, PRIO_BOOT - 1, &kid);
    if (err != SYS_ERR_OWNER_DEAD) {
        panic("newborn: a killed process answered %d, not %d",
          (int)err, (int)SYS_ERR_OWNER_DEAD);
    }
    if (killed.c.c_proc->p_nthreads != 0) {
        panic("newborn: the refusal made %u threads anyway",
          (unsigned int)killed.c.c_proc->p_nthreads);
    }

    cnprintf("    %-38s %s\n", "and the same call after the kill",
      "refused, with nothing made");

    /*
     * Both requests are answered now, and both answers are refused,
     * which is what takes this thread off the channel it has been
     * serving twice over.
     */
    if (channel_reply(killed.peer, &killed.msg) != SYS_ERR_STALE ||
      channel_reply(peer_kid, &second) != SYS_ERR_STALE) {
        panic("newborn: an answer to a dead client was delivered");
    }

    if (handle_close(ht, killed.h) != SYS_OK) {
        panic("newborn: the spawn handle was already gone");
    }

    /*
     * And one that nothing killed, per the review of this stage. A
     * process is over when its last thread has gone, whether that
     * was a kill's doing or its own, and both leave a completion
     * that has been advanced. The mark is only one of the two ways
     * to get there, and the reading that was written first saw only
     * that one.
     *
     * This one is answered rather than killed: the reply lets its
     * thread return from the send and fall off the end of main.
     */
    start_lastborn(serve, &ended);

    memset(&answer, 0, sizeof(answer));
    answer.handle = SYS_HANDLE_INVALID;
    if (channel_reply(ended.peer, &answer) != SYS_OK) {
        panic("newborn: the answer was not delivered");
    }

    code = child_join(&ended.c, JOIN_MS);
    if (code != LASTBORN_RETURN) {
        panic("newborn: it ended with %d, not %d", (int)code,
          (int)LASTBORN_RETURN);
    }
    if (!ended.c.c_proc->p_exited || ended.c.c_proc->p_dying) {
        panic("newborn: it ended without having exited");
    }

    err = proc_thread_create(ended.c.c_proc, ended.entry, 0,
      ended.stack, PRIO_BOOT - 1, &kid);
    if (err != SYS_ERR_OWNER_DEAD) {
        panic("newborn: a process that ended answered %d, not %d",
          (int)err, (int)SYS_ERR_OWNER_DEAD);
    }
    if (ended.c.c_proc->p_nthreads != 0) {
        panic("newborn: the refusal made %u threads anyway",
          (unsigned int)ended.c.c_proc->p_nthreads);
    }

    if (proc_creates_refused != refused + 2) {
        panic("newborn: %lu refusals, not two",
          (unsigned long)(proc_creates_refused - refused));
    }

    cnprintf("    %-38s %s\n", "and one that nothing killed",
      "refused the same, having ended itself");

    if (handle_close(ht, ended.h) != SYS_OK ||
      handle_close(ht, keep) != SYS_OK) {
        panic("newborn: a handle was already gone");
    }

    until = time_now() + time_from_ms(JOIN_MS);
    while (slot_alive(id)) {
        if (time_now() > until) {
            panic("newborn: the channel outlived the processes");
        }
        (void)timeline_wait(&kill_tl, 1, time_now() + time_from_ms(2));
    }
}

/*
 * A kill that has to cause the entry, per M17 stage three.


 *
 * Stage two put the mark where a trap returns to user mode, and found
 * that on one core it is enough for a reason that is not the hook: a
 * killer has to be running for a kill to happen, so the thread being
 * killed is not running, and it is sitting in the kernel where it was
 * interrupted or where it blocked.
 *
 * On two cores that reasoning stops. The thread is running in user mode
 * on the other core, taking no traps, and this kernel arms nothing that
 * would make it take one: M16 stage one measured stretches of over a
 * hundred milliseconds with no kernel in them at all. Something has to
 * make it enter, and that is the poke.
 */
static void
kill_other_core_checks(void)
{
    struct handle_table *ht = &proc_current()->p_handles;
    struct child c;
    struct thread *t;
    uint64_t killed, took;
    sys_handle_t keep;
    int32_t code;

    if (cpu_count() < 2) {
        /*
         * One core, and nothing to poke. Said rather than left out: the
         * tier where this cannot happen is the tier where the kill
         * always lands on a thread that is already in the kernel, and
         * that is section 13.3's single core rather than an accident.
         */
        cnputs("a kill: the other core, where there is one\n");
        cnprintf("    %-38s %s\n", "a kill on one core pokes",
          "nothing, because nothing is running");
        return;
    }

    cnputs("a kill: reaching a program on the other core\n");

    sched_defer();
    child_watch(&c, proc_spawn_image("spin", PRIO_BOOT - 1, NULL, 0,
      NULL, 0));
    if (handle_insert(ht, c.c_proc, SYS_OBJ_PROCESS, SYS_RIGHT_STAT,
      &keep) != SYS_OK) {
        panic("a kill: no handle slot to hold the program with");
    }

    /*
     * The other core, before it has run. The defer is what makes the
     * move legal, which is M16 stage three's arrangement for the same
     * reason: a thread that is running cannot be moved.
     */
    t = proc_threads(c.c_proc);
    expect("the program moved to the second core",
      sched_migrate(t, 1), SYS_OK);
    sched_undefer();

    /*
     * Long enough that it is running there rather than about to be.
     * This core is not the one dispatching it, so waiting is the only
     * way to know, and the state below is what says it worked.
     */
    (void)timeline_wait(&kill_tl, 1, time_now() + time_from_ms(10));

    if (t->t_state != THREAD_RUNNING) {
        panic("a kill: the program is not running on the other core, "
          "state %u", (unsigned int)t->t_state);
    }

    killed = time_now();
    if (proc_kill(c.c_proc, KILL_CODE) != SYS_OK) {
        panic("a kill: the process refused to be killed");
    }

    code = child_join(&c, JOIN_MS);
    took = time_now() - killed;

    if (code != KILL_CODE) {
        panic("a kill: it came back with %d and not the %d it was "
          "killed with", (int)code, KILL_CODE);
    }

    /*
     * One poke, and it is a count: one per thread of the target that
     * was running when the kill was written, which here is the one
     * thread this program has. A kill of something that is not running
     * pokes nothing, which the check on this core asserts.
     */
    if (c.c_proc->p_kill_pokes != 1) {
        panic("a kill: it sent %lu pokes for one running thread",
          (unsigned long)c.c_proc->p_kill_pokes);
    }

    cnprintf("    %-38s %s\n", "a program running on the other core",
      "was reached");
    cnprintf("    %-38s %lu\n", "pokes the kill sent",
      (unsigned long)c.c_proc->p_kill_pokes);
    cnprintf("    %-38s %lu ticks, about %lu us\n",
      "how long it ran on afterwards", (unsigned long)took,
      (unsigned long)(took * 1000000ULL / time_hz()));

    expect("giving the program's handle back", handle_close(ht, keep),
      SYS_OK);
}
#if SYS_TIER == 2

static sys_handle_t h_hold_recv;
static volatile uint32_t hold_faults;
static volatile uint32_t hold_notices;
static volatile bool hold_stop;

/*
 * A pager that takes a fault and does not answer it, per M17 stage four.
 *
 * The faulting thread is inside a rendezvous it did not choose: section
 * 15.3 sends the fault as a message from the thread, and this one keeps
 * it. That is the state the stage is about, and the only way to get a
 * program to sit in it long enough to be killed.
 */
static void
hold_pager(void *arg)
{
    struct sys_op_recv_args rx;

    (void)arg;

    while (!hold_stop) {
        memset(&rx, 0, sizeof(rx));
        if (run(SYS_OP_IPC_RECV, h_hold_recv, &rx, sizeof(rx)) != SYS_OK) {
            break;
        }
        /*
         * A pulse is not a request. Section 10.5 tells a server that
         * its client has gone, and the kernel is the client here, so
         * the notice arrives on this channel like any other: counting
         * it as a second fault was the first thing this check did,
         * and it is the death notice arriving rather than the program
         * asking twice.
         */
        if ((rx.message.flags & SYS_MSG_IS_PULSE) != 0) {
            const sys_pulse_t *p =
              (const sys_pulse_t *)rx.message.inline_data;

            if (p->code == SYS_PULSE_DEAD_OWNER) {
                hold_notices++;
            }
            continue;
        }

        hold_faults++;

        /*
         * And nothing goes back. The client is not left for ever: it is
         * killed, and the reply that never comes is what makes this
         * check about the kill rather than about the pager.
         */
    }

    thread_exit(0);
}

/*
 * A program killed while it waits for a page, per M17 stage four.
 *
 * Its thread is blocked in a send it did not write, and the kill
 * releases it the way it releases any other client: the send returns an
 * error. What the fault path did with that error was report a fault, so
 * a program that was killed came back saying it had faulted, and a
 * supervisor reading the status would have been told the wrong cause.
 */
static void
kill_faulting_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct sys_op_connect_args cargs;
    const struct boot_member *m;
    sys_ns_spec_t ns[1];
    struct child c;
    struct channel *ch;
    struct thread *t;
    const char *argv[3];
    char abase[19], alen[19];
    uint64_t waited;
    sys_handle_t keep;
    int32_t code;

    cnputs("a kill: a program waiting for a page it will not get\n");

    m = boot_lookup("paged");
    if (m == NULL) {
        panic("a kill: no paged image");
    }

    ch = channel_create("kill-pager", self);
    if (ch == NULL) {
        panic("a kill: no channel for the pager");
    }
    expect("a channel for the pager that does not answer",
      handle_insert(ht, ch, SYS_OBJ_CHANNEL, SYS_RIGHT_RECV,
      &h_hold_recv), SYS_OK);

    memset(&cargs, 0, sizeof(cargs));
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a connection to it, for the program",
      run(SYS_OP_CHAN_CONNECT, h_hold_recv, &cargs, sizeof(cargs)),
      SYS_OK);

    hold_faults = 0;
    hold_notices = 0;
    hold_stop = false;
    t = thread_create("hold-pager", PRIO_BOOT + 1, hold_pager, NULL);
    sched_ready(t);

    hex_into(abase, (uintptr_t)m->bm_load);
    hex_into(alen, m->bm_size);
    argv[0] = "paged";
    argv[1] = abase;
    argv[2] = alen;

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_PAGER;
    ns[0].handle = cargs.out_handle;

    /*
     * Above the boot thread, because this check waits for it to fault
     * and a program below the waiter never runs: the wait is a spin at
     * a higher priority, which is a lesson stage one paid for once
     * already.
     */
    sched_defer();
    child_watch(&c, proc_spawn_image("paged", 20, ns, 1, argv, 3));
    if (handle_insert(ht, c.c_proc, SYS_OBJ_PROCESS, SYS_RIGHT_STAT,
      &keep) != SYS_OK) {
        panic("a kill: no handle slot to hold the program with");
    }
    sched_undefer();

    /*
     * Until it has faulted and the pager has the message. Waiting on a
     * count rather than on a length of time: what the check needs is
     * the state, and how long the machine takes to reach it is the
     * machine's business.
     */
    waited = time_now() + time_from_ms(JOIN_MS);
    while (hold_faults == 0) {
        if (time_now() > waited) {
            panic("a kill: it never asked for a page");
        }
        sched_yield();
    }

    if (proc_kill(c.c_proc, KILL_CODE) != SYS_OK) {
        panic("a kill: the process refused to be killed");
    }

    code = child_join(&c, JOIN_MS);

    /*
     * The code it was killed with, and not SYS_EXIT_FAULT. Both are
     * negative and a check that only asked whether it ended badly would
     * pass either way; which one it is says whether the system knows
     * why the program stopped.
     */
    if (code != KILL_CODE) {
        panic("a kill: a program killed while faulting came back with "
          "%d", (int)code);
    }

    /*
     * And it asked once. A second request would mean the fault was
     * retried after the kill, which is a dying thread going back to a
     * pager that is not going to answer it.
     */
    if (hold_faults != 1) {
        panic("a kill: the pager saw %u requests",
          (unsigned int)hold_faults);
    }

    /*
     * And it was told. Section 10.5 has a server hear that its client
     * has gone rather than discover it at reply time, and a pager is a
     * server whose client is the kernel: the notice arrives here for a
     * fault nobody will ever answer.
     *
     * Waited for rather than read once. The join returns when the
     * program has gone, and the notice is delivered to a thread that
     * has to run to take it: reading the count straight after the join
     * is reading it before the pager has necessarily looked.
     */
    waited = time_now() + time_from_ms(JOIN_MS);
    while (hold_notices == 0) {
        if (time_now() > waited) {
            panic("a kill: the pager was never told its client had gone");
        }
        sched_yield();
    }

    if (hold_notices != 1) {
        panic("a kill: the pager was told %u times that its client had "
          "gone", (unsigned int)hold_notices);
    }

    cnprintf("    %-38s %s\n", "a program killed while it waited",
      "left as killed, not as faulted");
    cnprintf("    %-38s %u asked for, %u death notice\n",
      "pages", (unsigned int)hold_faults, (unsigned int)hold_notices);

    /*
     * And the pager stops being one. Cancelled rather than left: its
     * receive is a wait nothing will end, so without this the thread
     * sits blocked for the rest of the boot and the flag telling it to
     * stop is a flag it never looks at again.
     */
    hold_stop = true;
    expect("ending the pager that does not answer", thread_cancel(t),
      SYS_OK);

    expect("giving the program's handle back", handle_close(ht, keep),
      SYS_OK);
    expect("giving the pager's channel back",
      handle_close(ht, h_hold_recv), SYS_OK);
}

#endif /* SYS_TIER == 2 */

/*
 * The channel M17 stage five sends on, and the server thread that takes
 * a message from it and ends without answering.
 */
static struct channel *stale_ch;
static volatile uint32_t stale_took;

/*
 * A server thread that takes a message and dies holding it.
 *
 * Not a cancellation and not a fault: it takes the request and returns,
 * which is the ordinary way a thread of a live server ends. The process
 * it belongs to is this one, the kernel, and it goes on running, so
 * nothing section 12.5 does for the death of an owner applies here.
 */
static void
stale_server(void *arg)
{
    struct sys_msg msg;
    sys_thread_id_t peer;
    uint64_t cookie;

    (void)arg;

    memset(&msg, 0, sizeof(msg));
    if (channel_recv(stale_ch, &msg, &peer, &cookie) != SYS_OK) {
        thread_exit(1);
    }
    stale_took = 1;

    thread_exit(0);
}

static volatile sys_err_t stale_client_err;
static struct timeline stale_done;

/*
 * A server thread that takes a message and then waits for something
 * that never comes, so that it can be cancelled while holding it.
 *
 * It ends when the wait ends, whatever ended it. That is what a thread
 * of a server does when it is cancelled: section 10.5 releases it with
 * an error and the mark, if there is one, stops it at the next return
 * to user mode. This one is a kernel thread and stops here.
 */
static void
stale_waiting_server(void *arg)
{
    struct sys_msg msg;
    sys_thread_id_t peer;
    uint64_t cookie;

    (void)arg;

    memset(&msg, 0, sizeof(msg));
    if (channel_recv(stale_ch, &msg, &peer, &cookie) != SYS_OK) {
        thread_exit(1);
    }
    stale_took = 1;

    (void)timeline_wait(&kill_tl, 1, SYS_DEADLINE_NEVER);

    thread_exit(0);
}

/*
 * A client for the case above, because the thread doing the cancelling
 * has to still be running and a client is not.
 */
static void
stale_client(void *arg)
{
    struct sys_msg req, rep;

    (void)arg;

    memset(&req, 0, sizeof(req));
    memset(&rep, 0, sizeof(rep));
    req.handle = SYS_HANDLE_INVALID;

    stale_client_err = channel_send(stale_ch, 0, &req, &rep,
      time_now() + time_from_ms(STALE_DEADLINE_MS));

    (void)timeline_signal(&stale_done, 1);
    thread_exit(0);
}

/*
 * A chain of three, for the case a released client is itself a server.
 *
 * A sends to B, B sends to C, and C ends without answering. B is
 * released, and B still owes A an answer: what it is doing on C's
 * behalf ended, and what it is doing on A's behalf did not. Section
 * 12.3 walks the list of threads inside a request in order to raise the
 * middle of a chain, so B has to still be on it.
 */
static struct channel *chain_x;
static struct channel *chain_y;
static volatile uint32_t chain_took;
static volatile bool chain_still_serving;
static volatile sys_err_t chain_mid_err;
static volatile sys_err_t chain_head_err;
static struct timeline chain_over;

static void
chain_end_server(void *arg)
{
    struct sys_msg msg;
    sys_thread_id_t peer;
    uint64_t cookie;

    (void)arg;

    memset(&msg, 0, sizeof(msg));
    if (channel_recv(chain_y, &msg, &peer, &cookie) != SYS_OK) {
        thread_exit(1);
    }
    chain_took = 1;

    thread_exit(0);
}

static void
chain_middle(void *arg)
{
    struct sys_msg msg, req, rep;
    sys_thread_id_t peer;
    uint64_t cookie;

    (void)arg;

    memset(&msg, 0, sizeof(msg));
    if (channel_recv(chain_x, &msg, &peer, &cookie) != SYS_OK) {
        thread_exit(1);
    }

    memset(&req, 0, sizeof(req));
    memset(&rep, 0, sizeof(rep));
    req.handle = SYS_HANDLE_INVALID;
    chain_mid_err = channel_send(chain_y, 0, &req, &rep,
      time_now() + time_from_ms(STALE_DEADLINE_MS));

    /*
     * Read here, by the thread it is about, because this is the only
     * moment it can be read: the boot thread is below all three and
     * does not run again until this one has replied and gone.
     */
    chain_still_serving = chain_x->c_serving != NULL &&
      chain_x->c_serving->t_donee == thread_current();

    memset(&rep, 0, sizeof(rep));
    rep.handle = SYS_HANDLE_INVALID;
    (void)channel_reply(peer, &rep);

    thread_exit(0);
}

static void
chain_head(void *arg)
{
    struct sys_msg req, rep;

    (void)arg;

    memset(&req, 0, sizeof(req));
    memset(&rep, 0, sizeof(rep));
    req.handle = SYS_HANDLE_INVALID;
    chain_head_err = channel_send(chain_x, 0, &req, &rep,
      time_now() + time_from_ms(STALE_DEADLINE_MS));

    (void)timeline_signal(&chain_over, 1);
    thread_exit(0);
}

/*
 * The middle of a chain outlives the end of it, per this stage's
 * review. Releasing a client used to take it off whatever list it was
 * serving on, which for the middle of a chain is a call it still owes
 * an answer to and nothing to do with the one that ended.
 */
static void
chain_stale_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    sys_handle_t hx, hy;

    chain_x = channel_create("chain-x", self);
    chain_y = channel_create("chain-y", self);
    if (chain_x == NULL || chain_y == NULL) {
        panic("a stale reply: no channel for the chain");
    }

    /*
     * Held by a handle each, so that giving the handles back is what
     * releases them. A channel created and never handed to anything is
     * a channel nothing can free, and thirty-two of those is the whole
     * pool: the first version of this leaked both and the cohort check
     * further down the boot was the one that ran out.
     */
    expect("a handle for the head of the chain",
      handle_insert(ht, chain_x, SYS_OBJ_CHANNEL, SYS_RIGHT_RECV, &hx),
      SYS_OK);
    expect("a handle for the end of it",
      handle_insert(ht, chain_y, SYS_OBJ_CHANNEL, SYS_RIGHT_RECV, &hy),
      SYS_OK);

    chain_took = 0;
    chain_still_serving = false;
    chain_mid_err = SYS_ERR_INTERRUPTED;
    chain_head_err = SYS_ERR_INTERRUPTED;
    timeline_init(&chain_over, "chain-done");

    /*
     * Highest first, so each is already at its receive point when the
     * one below it sends, and all three are above the boot thread so
     * that it does not run again until the chain has finished.
     */
    sched_ready(thread_create("chain-end", PRIO_BOOT + 3, chain_end_server,
      NULL));
    sched_ready(thread_create("chain-mid", PRIO_BOOT + 2, chain_middle,
      NULL));
    sched_ready(thread_create("chain-head", PRIO_BOOT + 1, chain_head,
      NULL));

    expect("waiting for the chain to finish",
      timeline_wait(&chain_over, 1, time_now() + time_from_ms(JOIN_MS)),
      SYS_OK);

    if (chain_took == 0) {
        panic("a stale reply: the end of the chain never took the message");
    }
    expect("what the middle of the chain was told", chain_mid_err,
      SYS_ERR_STALE);
    if (!chain_still_serving) {
        panic("a stale reply: the middle of the chain stopped serving the "
          "call it still owed");
    }
    expect("and the answer the head of the chain got", chain_head_err,
      SYS_OK);

    expect("giving the chain's handles back", handle_close(ht, hx),
      SYS_OK);
    expect("and the other one", handle_close(ht, hy), SYS_OK);
    chain_x = NULL;
    chain_y = NULL;

    cnprintf("    %-38s %s\n", "the middle of a chain, released",
      "still owes its own client");
}

/*
 * A server with a record per client, and what it is left holding, per
 * M18 stage one.
 *
 * Sections 12.5 and 13.1 both argue from a server that allocates
 * something for a client and would leak it if it never heard the client
 * had gone. Nothing here has ever had anything to leak, so the argument
 * has never been tested. `ledger` has: a slot out of a fixed table per
 * client, taken on request and given back only when asked.
 *
 * The number this stage produces is the leak. Every client that takes a
 * record and dies leaves it taken, and the check asserts that count so
 * that the stages which fix it have something to be measured against.
 */
static sys_handle_t h_ledger_recv;
static sys_handle_t h_ledger_ask;

/* One timeline nobody signals, for every client that has to wait. */
static struct timeline *ledger_never;

/*
 * Ask the ledger something as an ordinary client. The boot thread is a
 * client here like any other, and COUNT is the one request that takes
 * no record, so asking does not change what is being asked about.
 */
static void
ledger_ask(uint32_t op, ledger_reply_t *out)
{
    struct sys_op_send_args tx;
    ledger_request_t *req;

    memset(&tx, 0, sizeof(tx));
    tx.request.handle = SYS_HANDLE_INVALID;
    req = (ledger_request_t *)tx.request.inline_data;
    req->lr_op = op;
    tx.request.inline_len = sizeof(*req);

    if (run(SYS_OP_IPC_SEND, h_ledger_ask, &tx, sizeof(tx)) != SYS_OK) {
        panic("ledger: the server did not answer op %u",
          (unsigned int)op);
    }

    memcpy(out, tx.reply.inline_data, sizeof(*out));
    if (out->lp_status != SYS_OK) {
        panic("ledger: op %u came back %d", (unsigned int)op,
          (int)out->lp_status);
    }
}

/*
 * A cookie per connection, which is the server's own per-connection
 * name and the kernel never looks at it. Distinct here so that a server
 * keying its records on the connection rather than the client would
 * behave differently, which is what makes that policy falsifiable: with
 * every connection named zero the two policies are the same policy.
 */
#define LEDGER_COOKIE_CHECK     1u
#define LEDGER_COOKIE_HELD      2u
#define LEDGER_COOKIE_PAIR      3u
#define LEDGER_COOKIE_DIES      4u

/*
 * And one per client of the leak check, which needs them for the same
 * reason: a record let go because its connection went is a record
 * found by a name, and every connection named zero is one name.
 */
#define LEDGER_COOKIE_LEAK      8u

/* And three for the departures nobody collects, which take no client. */
#define HINT_COOKIE             16u

/* Whether the server holds a record for this thread. Defined below. */
static bool ledger_holds(sys_thread_id_t who);

/*
 * Start one client, let it get where it is going, and end it the way
 * its argument says it ends.
 *
 * `hold` and `idle` never return on their own, so they are killed;
 * `leave` and `polite` return and are joined. Which of the two this is
 * follows from the argument, and the check says which it expected.
 */
static void
ledger_client(const char *how, bool ends_itself, uint32_t want_taken,
  uint32_t want_notices, uint64_t cookie)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct sys_op_connect_args cargs;
    sys_ns_spec_t ns[2];
    ledger_reply_t rep;
    struct child c;
    const char *argv[2];
    sys_handle_t keep, hn;
    uint64_t waited;
    int32_t code;

    memset(&cargs, 0, sizeof(cargs));
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    cargs.cookie = cookie;
    if (run(SYS_OP_CHAN_CONNECT, h_ledger_recv, &cargs,
      sizeof(cargs)) != SYS_OK) {
        panic("ledger: no connection for %s", how);
    }

    if (handle_insert(ht, ledger_never, SYS_OBJ_TIMELINE,
      SYS_RIGHT_WAIT | SYS_RIGHT_TRANSFER, &hn) != SYS_OK) {
        panic("ledger: no handle for the timeline");
    }

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_PRIVATE + 0;
    ns[0].handle = hn;
    ns[1].id = SYS_NS_PRIVATE + 1;
    ns[1].handle = cargs.out_handle;

    argv[0] = "patron";
    argv[1] = how;

    /*
     * Above the boot thread, because this check waits for the client to
     * reach the server and a program below the waiter never gets there.
     */
    sched_defer();
    child_watch(&c, proc_spawn_image("patron", 20, ns, 2, argv, 2));
    if (handle_insert(ht, c.c_proc, SYS_OBJ_PROCESS, SYS_RIGHT_STAT,
      &keep) != SYS_OK) {
        panic("ledger: no handle slot for %s", how);
    }
    sched_undefer();

    /*
     * Until the server has it, asked of the server. Every client is
     * waited for the same way now: holding a request is a request code
     * rather than an arrangement about which message arrived first, so
     * there is always a serving thread to ask.
     *
     * On records ever taken and not on records held, because held goes
     * down again. The client that gives its record back can do both
     * before this thread looks, and a wait for the higher number would
     * never see it.
     */
    waited = time_now() + time_from_ms(JOIN_MS);
    for (;;) {
        ledger_ask(LEDGER_OP_COUNT, &rep);
        if (rep.lp_taken >= want_taken) {
            break;
        }
        if (time_now() > waited) {
            panic("ledger: %s never reached the server, %u records "
              "ever taken", how, (unsigned int)rep.lp_taken);
        }
        sched_yield();
    }

    if (ends_itself) {
        code = child_join(&c, JOIN_MS);
        if (code != 0) {
            panic("ledger: %s came back %d", how, (int)code);
        }
    } else {
        if (proc_kill(c.c_proc, KILL_CODE) != SYS_OK) {
            panic("ledger: %s refused to be killed", how);
        }
        code = child_join(&c, JOIN_MS);
        if (code != KILL_CODE) {
            panic("ledger: %s came back %d and not the code it was "
              "killed with", how, (int)code);
        }
    }

    /*
     * Only the process handle. The other two travelled: section 7.4 has
     * a handle in a namespace move, so this table no longer has them
     * and closing one is closing something that is not there.
     */
    expect("giving the client back", handle_close(ht, keep), SYS_OK);

    /*
     * And what the server was told about this death, per M18 stage two.
     *
     * One round trip is enough, and the reason is section 13.1: a pulse
     * is collected before a message, so a notice enqueued by the kill
     * above has already been taken by the time the server answers the
     * question below. A settling period would be a check that waits
     * instead of one that knows.
     */
    ledger_ask(LEDGER_OP_COUNT, &rep);
    if (rep.lp_notices != want_notices) {
        panic("ledger: the server was told %u times after %s, not %u",
          (unsigned int)rep.lp_notices, how, (unsigned int)want_notices);
    }
}

static void
ledger_leak_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct sys_op_connect_args cargs;
    const struct boot_member *m;
    ledger_reply_t rep;
    sys_ns_spec_t ns[2];
    struct channel *ch;
    struct child sc;
    sys_thread_id_t mine;
    sys_handle_t give, keep;
    uint32_t conns_before = connection_pool()->used;
    sys_id_t sid;
    uint64_t waited;

    cnputs("a server: what it is left holding when its clients go\n");

    m = boot_lookup("ledger");
    if (m == NULL) {
        panic("ledger: no image");
    }

    ledger_never = timeline_create("ledger-never");
    if (ledger_never == NULL) {
        panic("ledger: no timeline");
    }

    ch = channel_create("ledger", self);
    if (ch == NULL) {
        panic("ledger: no channel");
    }
    expect("a channel for the server",
      handle_insert(ht, ch, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &h_ledger_recv), SYS_OK);

    /*
     * A second handle for the same channel, to hand to the server. It
     * is a fresh insert rather than a duplicate because this table's
     * own handle was not made duplicable, and the last insert is what
     * says whose channel it is: section 12.5 has a channel belong to
     * whoever may receive on it, and that is about to be the server.
     */
    if (handle_insert(ht, ch, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &give) != SYS_OK) {
        panic("ledger: cannot hand the channel on");
    }
    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_SERVICE;
    ns[0].handle = give;
    ns[1].id = SYS_NS_PRIVATE + 0;
    if (handle_insert(ht, ledger_never, SYS_OBJ_TIMELINE,
      SYS_RIGHT_WAIT | SYS_RIGHT_TRANSFER, &ns[1].handle) != SYS_OK) {
        panic("ledger: no handle for the server's timeline");
    }

    /*
     * Above the clients, so that it is at its receive point before any
     * of them sends. Below would still work, because a queued sender is
     * taken when the server gets there, but a check that has to reason
     * about which happened first is a check with a second thing in it.
     */
    sched_defer();
    child_watch(&sc, proc_spawn_image("ledger", 21, ns, 2, NULL, 0));
    if (handle_insert(ht, sc.c_proc, SYS_OBJ_PROCESS, SYS_RIGHT_STAT,
      &keep) != SYS_OK) {
        panic("ledger: no handle slot for the server");
    }
    sched_undefer();

    memset(&cargs, 0, sizeof(cargs));
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    cargs.cookie = LEDGER_COOKIE_CHECK;
    expect("a connection to it, for the check",
      run(SYS_OP_CHAN_CONNECT, h_ledger_recv, &cargs, sizeof(cargs)),
      SYS_OK);
    h_ledger_ask = cargs.out_handle;

    /*
     * And a record for the check itself, which is a client like any
     * other and is the one that never goes away. Without it every
     * record in this table belongs to a client that has left, and a
     * server that let go of all of them whenever it collected anything
     * would end at the same numbers as one that let go of the right
     * ones.
     */
    mine = thread_id(thread_current());
    ledger_ask(LEDGER_OP_TAKE, &rep);
    if (!ledger_holds(mine)) {
        panic("ledger: the server took no record for the check");
    }

    /*
     * Three ways of dying, in the order that makes each one's state
     * unambiguous. The first is the one the server never answers, and
     * it has to be first for that to be a fact rather than a race: this
     * server has one receiver until that request arrives.
     */
    ledger_client("hold", false, 2, 1, LEDGER_COOKIE_LEAK + 1);

    /*
     * And the server had nothing before it, and nothing after it
     * either: this client died inside a request, so a notice arrived
     * and stage three released its record. One record was taken and
     * none is held.
     */
    ledger_ask(LEDGER_OP_COUNT, &rep);
    if (rep.lp_held != 1 || rep.lp_taken != 2) {
        panic("ledger: %u records held and %u ever taken after one client",
          (unsigned int)rep.lp_held, (unsigned int)rep.lp_taken);
    }

    ledger_client("idle", false, 3, 1, LEDGER_COOKIE_LEAK + 2);
    ledger_client("leave", true, 4, 1, LEDGER_COOKIE_LEAK + 3);

    /*
     * And one that gives its record back, which is the guard on
     * everything above. Without it the count could be right for a
     * server whose table only ever fills, and the whole measurement
     * would be of a number that cannot go down.
     */
    ledger_client("polite", true, 5, 1, LEDGER_COOKIE_LEAK + 4);

    ledger_ask(LEDGER_OP_COUNT, &rep);

    /*
     * Four clients took a record and none of them is still held. One
     * gave its record back and one was announced dead inside a
     * request, and the other two were never announced at all: those
     * two are the leak stages one to three measured, and what closed
     * it is the server collecting connections whose clients have gone.
     *
     * Asserted and not printed, because a stage that only printed this
     * would pass for a server whose table had emptied for any reason.
     */
    if (rep.lp_taken != 5) {
        panic("ledger: %u records were ever taken, not 5",
          (unsigned int)rep.lp_taken);
    }
    if (rep.lp_held != 1) {
        panic("ledger: %u records held after four clients, not 1",
          (unsigned int)rep.lp_held);
    }

    /*
     * And the one still held is the check's own. This is what says the
     * server let go of the right records: collecting a connection
     * releases what was taken on it, and a server that emptied its
     * table on every collection would reach the count above and lose
     * a client that is still here.
     */
    if (!ledger_holds(mine)) {
        panic("ledger: the server let go of the check's own record");
    }

    /*
     * Every client's connection, including the two whose records were
     * already gone: what is collected is a connection whose peers have
     * all left, and whether the server still had a record on it is a
     * separate question it answers for itself.
     */
    if (rep.lp_reaped != 4) {
        panic("ledger: %u connections collected, not 4",
          (unsigned int)rep.lp_reaped);
    }

    /*
     * And exactly the two nobody announced. A server that collected
     * correctly and released too much would end at the same zero
     * above, so this is what says which records collecting accounted
     * for.
     */
    if (rep.lp_drained != 2) {
        panic("ledger: collecting let go of %u records, not 2",
          (unsigned int)rep.lp_drained);
    }

    /*
     * The hints arrived and the server did nothing with them. That is
     * the claim the level state exists to make: what it acted on is
     * the connection it collected, and a hint lost would have cost it
     * nothing.
     */
    if (rep.lp_hints != 4) {
        panic("ledger: %u hints that a peer had gone, not 4",
          (unsigned int)rep.lp_hints);
    }

    cnprintf("    %-38s %u of %u ever taken\n",
      "records held, and the check holds one",
      (unsigned int)rep.lp_held, (unsigned int)rep.lp_taken);
    cnprintf("    %-38s %u\n", "records a notice released",
      (unsigned int)rep.lp_released);
    cnprintf("    %-38s %u of %u connections\n",
      "records collecting let go", (unsigned int)rep.lp_drained,
      (unsigned int)rep.lp_reaped);
    cnprintf("    %-38s %u, none acted on\n",
      "hints that a client had gone", (unsigned int)rep.lp_hints);
    cnprintf("    %-38s %u of 3 deaths\n",
      "death notices the server saw", (unsigned int)rep.lp_notices);
    cnprintf("    %-38s %s\n", "the one it was told about",
      "the client killed inside a request");

    /*
     * And what the hints cost when nobody collects them, which is the
     * review's finding turned into a check.
     *
     * On a channel of its own with nothing receiving on it, because
     * that is the state that matters: a server busy elsewhere while
     * its clients leave. The departures are arranged by holding the
     * references here rather than by programs, because a program
     * cannot be made to leave while the server is not looking.
     *
     * Six user pulses first, which any client may send, then three
     * clients leaving. The queue holds eight and reserves two, so a
     * channel that enqueued one hint per departure would panic on the
     * third with "the reserved pulse slots are full" and take the
     * machine with it. One hint is all there ever is, so the count is
     * seven and the three connections are still there to collect.
     */
    {
        struct channel *hints = channel_create("hints", self);
        struct connection *cn[3];
        uint32_t queued = 0;
        uint64_t cookie;
        uint32_t i;

        if (hints == NULL) {
            panic("hints: no channel");
        }
        if (channel_reap(hints, true, &cookie) != SYS_ERR_NO_ENT) {
            panic("hints: a channel with no connections collected one");
        }

        for (i = 0; i < 6; i++) {
            channel_pulse(hints, SYS_PULSE_USER_BASE, i);
        }

        for (i = 0; i < 3; i++) {
            cn[i] = connection_create(hints, HINT_COOKIE + i);
            if (cn[i] == NULL) {
                panic("hints: no connection");
            }

            /*
             * Held by the server, which is a live process that is not
             * this channel's owner, so these are a client's references
             * and dropping the last of them is a client leaving.
             */
            connection_retain(cn[i], sc.c_proc);
        }
        for (i = 0; i < 3; i++) {
            connection_release(cn[i], sc.c_proc);
        }

        for (i = 0; i < hints->c_pulse_count; i++) {
            uint32_t k = (hints->c_pulse_head + i) % SYS_PULSE_SLOTS;

            if (hints->c_pulses[k].code == SYS_PULSE_PEER_GONE) {
                queued++;
            }
        }
        if (queued != 1) {
            panic("hints: %u hints queued for three departures, not 1",
              (unsigned int)queued);
        }
        if (hints->c_pulse_count != 7) {
            panic("hints: %u pulses queued, not 7",
              (unsigned int)hints->c_pulse_count);
        }

        /*
         * And all three are still collectable, which is what says the
         * hint being one is not the state being one.
         */
        for (i = 0; i < 3; i++) {
            if (channel_reap(hints, false, &cookie) != SYS_OK) {
                panic("hints: only %u of three departures collected",
                  (unsigned int)i);
            }
            if (cookie < HINT_COOKIE || cookie >= HINT_COOKIE + 3) {
                panic("hints: collected a cookie this check never used");
            }
        }
        if (channel_reap(hints, false, &cookie) != SYS_ERR_NO_ENT) {
            panic("hints: a fourth departure was collected");
        }

        cnprintf("    %-38s %u for %u departures\n",
          "hints queued while nobody collected", (unsigned int)queued, 3u);
    }

    /*
     * The check's own connection goes back first, while the server is
     * still alive and still collecting. Nothing asks it anything after
     * this, so it never collects that one: it is kept, and what frees
     * it is the server dying. Closing it after the kill would take the
     * ordinary path and leave that half untested.
     */
    expect("giving the check's connection back",
      handle_close(ht, h_ledger_ask), SYS_OK);

    sid = sc.c_proc->p_id;
    expect("ending the server", proc_kill(sc.c_proc, KILL_CODE), SYS_OK);
    if (child_join(&sc, JOIN_MS) != KILL_CODE) {
        panic("ledger: the server did not end when it was killed");
    }

    expect("giving the server's handle back", handle_close(ht, keep),
      SYS_OK);
    expect("giving the server's channel back",
      handle_close(ht, h_ledger_recv), SYS_OK);

    /*
     * And the server's slot goes back, per M13: the join says it has
     * exited and closing the handle says nothing can ask about it, but
     * the slot returning is the reap and is a separate event. A check
     * that spawns six processes and asserts none of them is left is
     * cheap here and is the thing that would catch a server this
     * milestone forgot to end.
     */
    waited = time_now() + time_from_ms(JOIN_MS);
    while (pool_lookup(proc_pool(), sid) != NULL) {
        if (time_now() > waited) {
            panic("ledger: the server exited and was never reaped");
        }
        sched_yield();
    }

    /*
     * And the connection pool is where it started, per M18 stage four.
     * A connection kept back for a server to collect is a slot out of a
     * fixed pool, and one the server never collected is the same leak
     * this stage closed, moved into the kernel. The four the clients
     * used were collected; the check's own went with its handle, and
     * anything still kept for this server went when the server did.
     */
    if (connection_pool()->used != conns_before) {
        panic("ledger: %u connections in use, started with %u",
          (unsigned int)connection_pool()->used,
          (unsigned int)conns_before);
    }
}

/*
 * Two clients that share one connection, for M18 stage three. Kernel
 * threads rather than programs, because what a record is keyed on is
 * the question and a thread is what the notice names: the server cannot
 * tell these from any other client, which is the point of using them.
 *
 * They take a record and then wait for something nobody signals, so
 * they are alive and holding when the client that dies is killed.
 */
static sys_handle_t h_ident_send;
static sys_handle_t h_ident_hold;

static volatile uint32_t ident_took;
static struct thread *ident_a;
static struct thread *ident_b;
static struct thread *ident_c;

static void
ident_client(void *arg)
{
    struct sys_op_send_args tx;
    ledger_request_t *req;
    unsigned long which = (unsigned long)arg;
    bool hold = which != 0;

    memset(&tx, 0, sizeof(tx));
    tx.request.handle = SYS_HANDLE_INVALID;
    req = (ledger_request_t *)tx.request.inline_data;
    req->lr_op = hold ? LEDGER_OP_HOLD : LEDGER_OP_TAKE;
    tx.request.inline_len = sizeof(*req);

    /*
     * A hold never comes back, which is the point of it: this thread
     * is the client left inside a rendezvous, and it is here to be
     * alive and holding a record rather than to be killed.
     */
    if (run(SYS_OP_IPC_SEND, which == 1 ? h_ident_hold : h_ident_send,
      &tx, sizeof(tx)) != SYS_OK) {
        thread_exit(1);
    }
    ident_took++;

    (void)timeline_wait(&kill_tl, 1, SYS_DEADLINE_NEVER);

    thread_exit(0);
}

/* Whether the server holds a record for this thread. */
static bool
ledger_holds(sys_thread_id_t who)
{
    struct sys_op_send_args tx;
    ledger_request_t *req;
    ledger_reply_t rep;

    memset(&tx, 0, sizeof(tx));
    tx.request.handle = SYS_HANDLE_INVALID;
    req = (ledger_request_t *)tx.request.inline_data;
    req->lr_op = LEDGER_OP_HELD_FOR;
    req->lr_tag = SYS_ID_TAG32(who);
    tx.request.inline_len = sizeof(*req);

    if (run(SYS_OP_IPC_SEND, h_ledger_ask, &tx, sizeof(tx)) != SYS_OK) {
        panic("ledger: the server did not answer a held-for");
    }

    memcpy(&rep, tx.reply.inline_data, sizeof(rep));
    if (rep.lp_status != SYS_OK && rep.lp_status != SYS_ERR_NO_ENT) {
        panic("ledger: a held-for came back %d", (int)rep.lp_status);
    }

    return rep.lp_status == SYS_OK;
}

/*
 * Releasing the right record, per M18 stage three.
 *
 * A count is not enough. A server that released whichever record it
 * looked at first would end holding the right number of them and the
 * wrong ones, and every client whose record it took would find itself
 * unknown to a server it is still talking to. So the check asks the
 * server about each client by name, and the name is the one thing it
 * has: `SYS_ID_TAG32` of the thread, which is what a notice carries.
 *
 * Two of the three clients share a connection, which is the case that
 * says what the key is. Keyed by thread, one of them goes and the other
 * stays. Keyed by connection, both would have gone, and the server
 * would have been right about the count and wrong about everything
 * else.
 */
static void
ledger_identity_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct sys_op_connect_args cargs;
    ledger_reply_t rep;
    sys_ns_spec_t ns[2];
    struct channel *ch;
    struct child sc, c;
    const char *argv[2];
    sys_thread_id_t a_id, b_id, c_id, d_id;
    sys_handle_t give, keep, akeep;
    sys_id_t sid;
    uint64_t waited;
    int32_t code;

    cnputs("a server: releasing the record of the client that died\n");

    ch = channel_create("ledger-id", self);
    if (ch == NULL) {
        panic("ledger: no channel for the identity check");
    }
    expect("a channel for a second server",
      handle_insert(ht, ch, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &h_ledger_recv), SYS_OK);

    if (handle_insert(ht, ch, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &give) != SYS_OK) {
        panic("ledger: cannot hand the second channel on");
    }
    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_SERVICE;
    ns[0].handle = give;
    ns[1].id = SYS_NS_PRIVATE + 0;
    if (handle_insert(ht, ledger_never, SYS_OBJ_TIMELINE,
      SYS_RIGHT_WAIT | SYS_RIGHT_TRANSFER, &ns[1].handle) != SYS_OK) {
        panic("ledger: no handle for the second server's timeline");
    }

    /*
     * A second server, with an empty table. The first one was killed
     * and its slot given back, which is what makes loading the same
     * image again legal at Tier 0.
     */
    sched_defer();
    child_watch(&sc, proc_spawn_image("ledger", 21, ns, 2, NULL, 0));
    if (handle_insert(ht, sc.c_proc, SYS_OBJ_PROCESS, SYS_RIGHT_STAT,
      &keep) != SYS_OK) {
        panic("ledger: no handle slot for the second server");
    }
    sched_undefer();

    /*
     * The check's own connection first, so that there is a way to ask
     * the server anything at all.
     */
    memset(&cargs, 0, sizeof(cargs));
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    cargs.cookie = LEDGER_COOKIE_CHECK;
    expect("a connection to it, for the check",
      run(SYS_OP_CHAN_CONNECT, h_ledger_recv, &cargs, sizeof(cargs)),
      SYS_OK);
    h_ledger_ask = cargs.out_handle;

    ledger_ask(LEDGER_OP_COUNT, &rep);
    if (rep.lp_held != 0) {
        panic("ledger: the second server started with %u records",
          (unsigned int)rep.lp_held);
    }

    /*
     * A client that is held and stays held. It takes the first record,
     * which is what makes the record of the client that dies not the
     * first one: a server releasing whichever record it looked at
     * first would take this one, and this one is still there.
     */
    memset(&cargs, 0, sizeof(cargs));
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    cargs.cookie = LEDGER_COOKIE_HELD;
    expect("a connection for a client that is held",
      run(SYS_OP_CHAN_CONNECT, h_ledger_recv, &cargs, sizeof(cargs)),
      SYS_OK);
    h_ident_hold = cargs.out_handle;

    ident_took = 0;
    ident_a = thread_create("ident-a", PRIO_BOOT + 1, ident_client,
      (void *)1);
    a_id = thread_id(ident_a);
    sched_ready(ident_a);

    waited = time_now() + time_from_ms(JOIN_MS);
    for (;;) {
        ledger_ask(LEDGER_OP_COUNT, &rep);
        if (rep.lp_held == 1) {
            break;
        }
        if (time_now() > waited) {
            panic("ledger: the held client never reached the server");
        }
        sched_yield();
    }

    /*
     * Two more clients, sharing one connection between them. One
     * connection and two threads is what tells a record keyed by thread
     * from a record keyed by the connection it arrived on.
     */
    memset(&cargs, 0, sizeof(cargs));
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    cargs.cookie = LEDGER_COOKIE_PAIR;
    expect("one connection for two more clients",
      run(SYS_OP_CHAN_CONNECT, h_ledger_recv, &cargs, sizeof(cargs)),
      SYS_OK);
    h_ident_send = cargs.out_handle;

    /*
     * One of the pair is held and one is answered, so that the one
     * that dies below is a client of the connection the other is still
     * using. Without that the pair is set up and never exercised: a
     * server keyed by the connection would behave identically, which
     * is what the first version of this check did.
     */
    ident_b = thread_create("ident-b", PRIO_BOOT + 1, ident_client,
      (void *)2);
    ident_c = thread_create("ident-c", PRIO_BOOT + 1, ident_client, NULL);
    b_id = thread_id(ident_b);
    c_id = thread_id(ident_c);
    sched_ready(ident_b);
    sched_ready(ident_c);

    waited = time_now() + time_from_ms(JOIN_MS);
    for (;;) {
        ledger_ask(LEDGER_OP_COUNT, &rep);
        if (rep.lp_held == 3) {
            break;
        }
        if (time_now() > waited) {
            panic("ledger: %u records after the sharing pair, not 3",
              (unsigned int)rep.lp_held);
        }
        sched_yield();
    }

    /*
     * And the client that dies, which is a program because it has to be
     * killed as a process. Its record is the fourth, not the first.
     */
    memset(&cargs, 0, sizeof(cargs));
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    cargs.cookie = LEDGER_COOKIE_DIES;
    expect("a connection for the client that dies",
      run(SYS_OP_CHAN_CONNECT, h_ledger_recv, &cargs, sizeof(cargs)),
      SYS_OK);

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_PRIVATE + 1;
    ns[0].handle = cargs.out_handle;
    ns[1].id = SYS_NS_PRIVATE + 0;
    if (handle_insert(ht, ledger_never, SYS_OBJ_TIMELINE,
      SYS_RIGHT_WAIT | SYS_RIGHT_TRANSFER, &ns[1].handle) != SYS_OK) {
        panic("ledger: no handle for the client's timeline");
    }
    argv[0] = "patron";
    argv[1] = "hold";

    sched_defer();
    child_watch(&c, proc_spawn_image("patron", 20, ns, 2, argv, 2));
    if (handle_insert(ht, c.c_proc, SYS_OBJ_PROCESS, SYS_RIGHT_STAT,
      &akeep) != SYS_OK) {
        panic("ledger: no handle slot for the client that dies");
    }
    sched_undefer();

    waited = time_now() + time_from_ms(JOIN_MS);
    {
        struct thread *ct = proc_threads(c.c_proc);

        d_id = thread_id(ct);
        while (ct->t_wait != THREAD_WAIT_REPLY) {
            if (time_now() > waited) {
                panic("ledger: the client that dies never reached the "
                  "server");
            }
            sched_yield();
        }
    }

    ledger_ask(LEDGER_OP_COUNT, &rep);
    if (rep.lp_held != 4) {
        panic("ledger: %u records before the death, not 4",
          (unsigned int)rep.lp_held);
    }
    if (!ledger_holds(a_id) || !ledger_holds(b_id) ||
      !ledger_holds(c_id) || !ledger_holds(d_id)) {
        panic("ledger: the server does not know one of its four clients");
    }

    if (proc_kill(c.c_proc, KILL_CODE) != SYS_OK) {
        panic("ledger: the client that dies refused to be killed");
    }
    code = child_join(&c, JOIN_MS);
    if (code != KILL_CODE) {
        panic("ledger: the client that dies came back %d", (int)code);
    }

    /*
     * One round trip, per stage two: a pulse is collected before a
     * message, so the notice has been acted on by the time this is
     * answered.
     */
    ledger_ask(LEDGER_OP_COUNT, &rep);
    if (rep.lp_released != 1) {
        panic("ledger: %u records released by a notice, not 1",
          (unsigned int)rep.lp_released);
    }
    if (rep.lp_held != 3) {
        panic("ledger: %u records after one death, not 3",
          (unsigned int)rep.lp_held);
    }

    /*
     * And the three it holds are the three that are still there. This
     * is the assertion the stage exists for: the count above would pass
     * for a server that released whichever record it saw first, and the
     * first is the held client's.
     */
    if (ledger_holds(d_id)) {
        panic("ledger: the server still holds the dead client's record");
    }
    if (!ledger_holds(a_id) || !ledger_holds(b_id) || !ledger_holds(c_id)) {
        panic("ledger: the server released a living client's record");
    }

    cnprintf("    %-38s %s\n", "the record the notice named",
      "released, and only that one");

    /*
     * And the sibling case, which is what the shared connection is for.
     * `ident-b` is held on the connection `ident-c` is also using, and
     * cancelling it out of that rendezvous is a death section 12.5
     * announces exactly as it announced the program's. A server keyed
     * by the connection would drop both records here.
     */
    expect("cancelling one of the two on one connection",
      thread_cancel(ident_b), SYS_OK);

    ledger_ask(LEDGER_OP_COUNT, &rep);
    if (rep.lp_released != 2) {
        panic("ledger: %u records released by a notice, not 2",
          (unsigned int)rep.lp_released);
    }
    if (rep.lp_held != 2) {
        panic("ledger: %u records after the sibling died, not 2",
          (unsigned int)rep.lp_held);
    }
    if (ledger_holds(b_id)) {
        panic("ledger: the server still holds the cancelled sibling's "
          "record");
    }
    if (!ledger_holds(c_id)) {
        panic("ledger: the server dropped the record of a client whose "
          "sibling died");
    }
    if (!ledger_holds(a_id)) {
        panic("ledger: the server dropped an unrelated client's record");
    }

    cnprintf("    %-38s %s\n", "two clients on one connection",
      "one died, the other kept its record");

    expect("giving the dead client's handle back",
      handle_close(ht, akeep), SYS_OK);
    expect("ending the client that was held", thread_cancel(ident_a),
      SYS_OK);
    expect("ending the client that shared", thread_cancel(ident_c),
      SYS_OK);
    expect("giving their connection back", handle_close(ht, h_ident_send),
      SYS_OK);
    expect("and the held client's", handle_close(ht, h_ident_hold),
      SYS_OK);


    sid = sc.c_proc->p_id;
    expect("ending the second server", proc_kill(sc.c_proc, KILL_CODE),
      SYS_OK);
    if (child_join(&sc, JOIN_MS) != KILL_CODE) {
        panic("ledger: the second server did not end when killed");
    }
    expect("giving the second server back", handle_close(ht, keep),
      SYS_OK);
    expect("giving the check's connection back",
      handle_close(ht, h_ledger_ask), SYS_OK);
    expect("giving the second channel back",
      handle_close(ht, h_ledger_recv), SYS_OK);

    waited = time_now() + time_from_ms(JOIN_MS);
    while (pool_lookup(proc_pool(), sid) != NULL) {
        if (time_now() > waited) {
            panic("ledger: the second server was never reaped");
        }
        sched_yield();
    }
}

/*
 * A death the server is never told about, per M18 stage five.
 *
 * Stage two found the notice strictly edge triggered and found the edge
 * unable to fail safely: a system pulse arriving at a full queue halted
 * the machine, on section 13.1's argument that losing one leaks a dead
 * client's resources for good. Stage four made that argument false, so
 * this stage loses one on purpose and asks whether the server is still
 * right.
 *
 * The queue is filled with notices rather than with user pulses, which
 * is a departure from the plan and is what the plan was wrong about. A
 * client flooding a channel cannot make a notice drop, because that is
 * exactly what the reservation prevents and stage two confirmed it
 * works. The only way to lose a notice is more outstanding deaths than
 * the queue holds, so that is what this arranges, by enqueueing the
 * notices itself: eight deaths nobody has collected, and then a real
 * one.
 */
#define LOST_COOKIE_CHECK       32u
#define LOST_COOKIE_DIES        33u

static void
notice_loss_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct sys_op_connect_args cargs;
    ledger_reply_t rep, before;
    sys_ns_spec_t ns[2];
    struct channel *ch;
    struct child sc, c;
    const char *argv[2];
    sys_thread_id_t d_id;
    sys_handle_t give, keep, akeep;
    uint64_t dropped_before, evicted_before, waited;
    sys_id_t sid;
    int32_t code;
    uint32_t i;

    cnputs("a server: a death it is never told about\n");

    ch = channel_create("ledger-lost", self);
    if (ch == NULL) {
        panic("lost: no channel");
    }
    expect("a channel for a third server",
      handle_insert(ht, ch, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &h_ledger_recv), SYS_OK);

    if (handle_insert(ht, ch, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &give) != SYS_OK) {
        panic("lost: cannot hand the channel on");
    }
    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_SERVICE;
    ns[0].handle = give;
    ns[1].id = SYS_NS_PRIVATE + 0;
    if (handle_insert(ht, ledger_never, SYS_OBJ_TIMELINE,
      SYS_RIGHT_WAIT | SYS_RIGHT_TRANSFER, &ns[1].handle) != SYS_OK) {
        panic("lost: no handle for the server's timeline");
    }

    sched_defer();
    child_watch(&sc, proc_spawn_image("ledger", 21, ns, 2, NULL, 0));
    if (handle_insert(ht, sc.c_proc, SYS_OBJ_PROCESS, SYS_RIGHT_STAT,
      &keep) != SYS_OK) {
        panic("lost: no handle slot for the server");
    }
    sched_undefer();

    memset(&cargs, 0, sizeof(cargs));
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    cargs.cookie = LOST_COOKIE_CHECK;
    expect("a connection to it, for the check",
      run(SYS_OP_CHAN_CONNECT, h_ledger_recv, &cargs, sizeof(cargs)),
      SYS_OK);
    h_ledger_ask = cargs.out_handle;

    /*
     * One client, killed inside a request, which is the one death
     * section 12.5 does announce. That is deliberate: the case where
     * losing the notice costs something is the case where the notice
     * was the only thing that would have said anything.
     */
    memset(&cargs, 0, sizeof(cargs));
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    cargs.cookie = LOST_COOKIE_DIES;
    expect("a connection for the client that dies",
      run(SYS_OP_CHAN_CONNECT, h_ledger_recv, &cargs, sizeof(cargs)),
      SYS_OK);

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_PRIVATE + 1;
    ns[0].handle = cargs.out_handle;
    ns[1].id = SYS_NS_PRIVATE + 0;
    if (handle_insert(ht, ledger_never, SYS_OBJ_TIMELINE,
      SYS_RIGHT_WAIT | SYS_RIGHT_TRANSFER, &ns[1].handle) != SYS_OK) {
        panic("lost: no handle for the client's timeline");
    }
    argv[0] = "patron";
    argv[1] = "hold";

    sched_defer();
    child_watch(&c, proc_spawn_image("patron", 20, ns, 2, argv, 2));
    if (handle_insert(ht, c.c_proc, SYS_OBJ_PROCESS, SYS_RIGHT_STAT,
      &akeep) != SYS_OK) {
        panic("lost: no handle slot for the client");
    }
    sched_undefer();

    waited = time_now() + time_from_ms(JOIN_MS);
    {
        struct thread *ct = proc_threads(c.c_proc);

        d_id = thread_id(ct);
        while (ct->t_wait != THREAD_WAIT_REPLY) {
            if (time_now() > waited) {
                panic("lost: the client never reached the server");
            }
            sched_yield();
        }
    }

    ledger_ask(LEDGER_OP_COUNT, &before);
    if (before.lp_held != 1) {
        panic("lost: %u records held before the death, not 1",
          (unsigned int)before.lp_held);
    }
    if (!ledger_holds(d_id)) {
        panic("lost: the server holds no record for the client");
    }

    /*
     * Eight deaths nobody has collected, and then a ninth. Scheduling
     * is held off across all of it so that the server cannot drain any
     * of them in between: a queue that emptied halfway would leave the
     * ninth a pulse with room, which is the case this is not about.
     *
     * The eight carry a tag that names no record, so what they cost the
     * server is a receive each and nothing else. What matters about
     * them is that they are there.
     */
    dropped_before = channel_pulses_dropped;
    evicted_before = channel_pulses_evicted;

    sched_defer();
    for (i = 0; i < SYS_PULSE_SLOTS; i++) {
        channel_pulse(ch, SYS_PULSE_DEAD_OWNER, 0);
    }
    if (proc_kill(c.c_proc, KILL_CODE) != SYS_OK) {
        panic("lost: the client refused to be killed");
    }
    sched_undefer();

    if (channel_pulses_dropped != dropped_before + 1 ||
      channel_pulses_evicted != evicted_before) {
        panic("lost: %lu pulses dropped and %lu evicted, not one and "
          "none",
          (unsigned long)(channel_pulses_dropped - dropped_before),
          (unsigned long)(channel_pulses_evicted - evicted_before));
    }

    code = child_join(&c, JOIN_MS);
    if (code != KILL_CODE) {
        panic("lost: the client came back %d", (int)code);
    }

    /*
     * And the handle back, which is what reaps it and closes the
     * connection it was holding. That is the level state arriving: the
     * notice is gone and this is not.
     */
    expect("giving the dead client back", handle_close(ht, akeep), SYS_OK);

    ledger_ask(LEDGER_OP_COUNT, &rep);

    /*
     * The eight it could take, and not the ninth. This is the
     * assertion the stage exists for: a check where the notices
     * received equal the deaths has lost nothing and tested nothing.
     */
    if (rep.lp_notices != before.lp_notices + SYS_PULSE_SLOTS) {
        panic("lost: the server saw %u notices for nine deaths, not %u",
          (unsigned int)(rep.lp_notices - before.lp_notices),
          (unsigned int)SYS_PULSE_SLOTS);
    }
    if (rep.lp_losses == 0) {
        panic("lost: the server was never told it had missed one");
    }

    /*
     * And it is right anyway. No record was released by a notice,
     * because the one that would have named this client never arrived;
     * the record went when the connection was collected.
     */
    if (rep.lp_released != before.lp_released) {
        panic("lost: %u records released by a notice, not %u",
          (unsigned int)rep.lp_released, (unsigned int)before.lp_released);
    }
    if (rep.lp_drained != before.lp_drained + 1) {
        panic("lost: collecting let go of %u records, not one",
          (unsigned int)(rep.lp_drained - before.lp_drained));
    }
    if (ledger_holds(d_id)) {
        panic("lost: the server still holds the dead client's record");
    }
    if (rep.lp_held != 0) {
        panic("lost: %u records held after the death, not 0",
          (unsigned int)rep.lp_held);
    }

    cnprintf("    %-38s %u of %u deaths\n", "notices the server saw",
      (unsigned int)(rep.lp_notices - before.lp_notices),
      (unsigned int)SYS_PULSE_SLOTS + 1);
    cnprintf("    %-38s %s\n", "the record of the death it missed",
      "released when the connection was collected");

    /*
     * And which of them a full queue gives up, per this stage's
     * review. A notice and a hint are not worth the same. The hint says
     * there is something to collect and the connection says so too; a
     * notice names a thread, and for a thread that dies with a live
     * sibling on its connection it is the only thing that does. So the
     * hint is what goes.
     *
     * Arranged by hand, because a run cannot be made to produce this
     * ordering: seven notices, a hint, and then one more notice
     * arriving at a queue with no room.
     */
    {
        uint32_t queued_hints = 0;
        uint32_t queued_notices = 0;

        if (ch->c_pulse_count != 0) {
            panic("lost: %u pulses left over before the second case",
              (unsigned int)ch->c_pulse_count);
        }

        sched_defer();
        for (i = 0; i + 1 < SYS_PULSE_SLOTS; i++) {
            channel_pulse(ch, SYS_PULSE_DEAD_OWNER, 0);
        }
        channel_pulse(ch, SYS_PULSE_PEER_GONE, 0);

        dropped_before = channel_pulses_dropped;
        evicted_before = channel_pulses_evicted;
        channel_pulse(ch, SYS_PULSE_DEAD_OWNER, 0);

        for (i = 0; i < ch->c_pulse_count; i++) {
            uint32_t k = (ch->c_pulse_head + i) % SYS_PULSE_SLOTS;

            if (ch->c_pulses[k].code == SYS_PULSE_PEER_GONE) {
                queued_hints++;
            }
            if (ch->c_pulses[k].code == SYS_PULSE_DEAD_OWNER) {
                queued_notices++;
            }
        }
        sched_undefer();

        /*
         * Evicted and not dropped, which is the whole of the second
         * case: the arriving notice was kept and something already
         * queued made room for it.
         */
        if (channel_pulses_evicted != evicted_before + 1 ||
          channel_pulses_dropped != dropped_before) {
            panic("lost: %lu evicted and %lu dropped by the second "
              "case, not one and none",
              (unsigned long)(channel_pulses_evicted - evicted_before),
              (unsigned long)(channel_pulses_dropped - dropped_before));
        }
        if (queued_hints != 0) {
            panic("lost: the queue gave up a notice and kept the hint");
        }
        if (queued_notices != SYS_PULSE_SLOTS) {
            panic("lost: %u notices queued after the hint went, not %u",
              (unsigned int)queued_notices, (unsigned int)SYS_PULSE_SLOTS);
        }

        cnprintf("    %-38s %s\n", "what a full queue gave up",
          "the hint, and it kept the notice");
    }

    expect("giving the check's connection back",
      handle_close(ht, h_ledger_ask), SYS_OK);

    sid = sc.c_proc->p_id;
    expect("ending the third server", proc_kill(sc.c_proc, KILL_CODE),
      SYS_OK);
    if (child_join(&sc, JOIN_MS) != KILL_CODE) {
        panic("lost: the server did not end when it was killed");
    }
    expect("giving the third server back", handle_close(ht, keep), SYS_OK);
    expect("giving the third channel back",
      handle_close(ht, h_ledger_recv), SYS_OK);

    waited = time_now() + time_from_ms(JOIN_MS);
    while (pool_lookup(proc_pool(), sid) != NULL) {
        if (time_now() > waited) {
            panic("lost: the third server was never reaped");
        }
        sched_yield();
    }
}

/*
 * A queue with a depth, and the clients it turns away, per M18 stage
 * six and section 12.1.
 *
 * A channel's sender queue was unbounded, so a server that fell behind
 * accumulated clients until the machine ran out of threads: every
 * queued sender is one. A client had no way to learn it was joining a
 * backlog rather than being served.
 *
 * Kernel threads as clients, on a channel nothing receives on, because
 * what is being measured is the queue rather than a service. Nothing
 * here carries a deadline: a client that is refused comes back with no
 * deadline to come back at, which is the whole claim, and a client that
 * queues stays queued until this check ends it.
 */
#define DEPTH_CLIENTS   9u

static sys_handle_t h_depth_send;
static volatile sys_err_t depth_res[DEPTH_CLIENTS];
static volatile bool depth_touched[DEPTH_CLIENTS];
static volatile uint32_t depth_done;

static void
depth_client(void *arg)
{
    unsigned long which = (unsigned long)arg;
    struct sys_op_send_args args;
    uint32_t mark = 0xd0d0d000u + (uint32_t)which;
    uint32_t *word;
    uint32_t i;
    bool touched = false;

    memset(&args, 0, sizeof(args));
    args.request.handle = SYS_HANDLE_INVALID;
    args.request.inline_len = sizeof(*word);
    word = (uint32_t *)args.request.inline_data;
    *word = mark;

    depth_res[which] = run(SYS_OP_IPC_SEND, h_depth_send, &args,
      sizeof(args));

    /*
     * And its message is still its own. A queued sender lends nothing:
     * section 12.1 hands the buffer over when a server takes the
     * message, and nothing here ever took one. So the request reads
     * back as it was written and the reply was never filled in.
     */
    if (*word != mark) {
        touched = true;
    }
    if (args.reply.inline_len != 0 || args.reply.handle != 0) {
        touched = true;
    }
    for (i = 0; i < sizeof(args.reply.inline_data); i++) {
        if (args.reply.inline_data[i] != 0) {
            touched = true;
        }
    }
    depth_touched[which] = touched;

    depth_done++;
    thread_exit(0);
}

/* The senders queued on this channel, in the order they would be served. */
static uint32_t
depth_queue(const struct channel *ch, const struct thread **out,
  uint32_t max)
{
    const struct thread *t;
    uint32_t n = 0;

    for (t = ch->c_senders; t != NULL; t = t->t_snext) {
        if (n < max) {
            out[n] = t;
        }
        n++;
    }

    return n;
}

/* Start one and let it get to wherever it is going. */
static struct thread *
depth_start(const char *name, uint8_t prio, unsigned long which)
{
    struct thread *t = thread_create(name, prio, depth_client,
      (void *)which);

    if (t == NULL) {
        panic("depth: no thread for %s", name);
    }
    sched_ready(t);

    return t;
}

static void
depth_wait(uint32_t want)
{
    uint64_t waited = time_now() + time_from_ms(JOIN_MS);

    while (depth_done < want) {
        if (time_now() > waited) {
            panic("depth: %u clients came back, not %u",
              (unsigned int)depth_done, (unsigned int)want);
        }
        sched_yield();
    }
}

static void
depth_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct sys_op_connect_args cargs;
    struct sys_op_chan_listen_args largs;
    const struct thread *queued[4];
    struct channel *ch, *open_ch;
    struct thread *a, *b, *d, *x, *y, *z, *open_t;
    sys_handle_t h_recv, h_norecv, h_first, h_open_recv, h_open_send;
    uint64_t refused, displaced;
    uint32_t n;

    cnputs("a queue: how deep, and who is turned away\n");

    ch = channel_create("depth", self);
    if (ch == NULL) {
        panic("depth: no channel");
    }
    expect("a channel nothing receives on",
      handle_insert(ht, ch, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &h_recv), SYS_OK);

    memset(&cargs, 0, sizeof(cargs));
    cargs.rights = SYS_RIGHT_SEND;
    expect("one connection for every client",
      run(SYS_OP_CHAN_CONNECT, h_recv, &cargs, sizeof(cargs)), SYS_OK);
    h_first = cargs.out_handle;
    h_depth_send = h_first;

    /*
     * Through the operation, so that the right is checked the way a
     * program's would be. A send handle cannot set the depth of the
     * queue it is waiting in.
     */
    if (handle_insert(ht, ch, SYS_OBJ_CHANNEL, SYS_RIGHT_TRANSFER,
      &h_norecv) != SYS_OK) {
        panic("depth: no handle without the receive right");
    }
    memset(&largs, 0, sizeof(largs));
    largs.depth = 2;
    expect("a handle that cannot receive cannot bound",
      run(SYS_OP_CHAN_LISTEN, h_norecv, &largs, sizeof(largs)),
      SYS_ERR_PERM_DENIED);
    expect("a depth of two, set by the receiving side",
      run(SYS_OP_CHAN_LISTEN, h_recv, &largs, sizeof(largs)), SYS_OK);

    refused = channel_senders_refused;
    displaced = channel_senders_displaced;

    /*
     * Two that fit, and a third that does not. Started one at a time,
     * each above the boot thread so that it reaches its send before
     * this one looks again: a check that had to reason about which of
     * three arrived first would be a check with a second thing in it.
     */
    a = depth_start("depth-a", PRIO_BOOT + 1, 0);
    b = depth_start("depth-b", PRIO_BOOT + 1, 1);
    (void)depth_start("depth-c", PRIO_BOOT + 1, 2);

    depth_wait(1);
    if (depth_res[2] != SYS_ERR_WOULDBLOCK) {
        panic("depth: the third client came back %d, not WOULDBLOCK",
          (int)depth_res[2]);
    }
    if (depth_touched[2]) {
        panic("depth: the refused client's message was not its own");
    }
    if (channel_senders_refused != refused + 1 ||
      channel_senders_displaced != displaced) {
        panic("depth: %lu refused and %lu displaced, not one and none",
          (unsigned long)(channel_senders_refused - refused),
          (unsigned long)(channel_senders_displaced - displaced));
    }

    n = depth_queue(ch, queued, 4);
    if (n != 2 || queued[0] != a || queued[1] != b) {
        panic("depth: %u senders queued after the refusal, not two",
          (unsigned int)n);
    }

    /*
     * And one that outranks the queue, which is the other end of the
     * same rule: it joins, and the queue gives up its tail rather than
     * refusing the arrival. The tail is `depth-b`, which arrived last
     * at the priority the two of them share.
     */
    d = depth_start("depth-d", PRIO_BOOT + 2, 3);

    depth_wait(2);
    if (depth_res[1] != SYS_ERR_WOULDBLOCK) {
        panic("depth: the displaced client came back %d, not WOULDBLOCK",
          (int)depth_res[1]);
    }
    if (depth_touched[1]) {
        panic("depth: the displaced client's message was not its own");
    }
    if (channel_senders_displaced != displaced + 1) {
        panic("depth: %lu displaced, not one",
          (unsigned long)(channel_senders_displaced - displaced));
    }

    n = depth_queue(ch, queued, 4);
    if (n != 2 || queued[0] != d || queued[1] != a) {
        panic("depth: the queue after the displacement is not the "
          "survivors in priority order");
    }

    cnprintf("    %-38s %s\n", "a queue two deep, three clients",
      "the third refused, nothing lent");
    cnprintf("    %-38s %s\n", "and one that outranked the queue",
      "it queued, the tail was displaced");

    /*
     * A depth of nothing, which is a depth like any other: this is what
     * the flag beside it is for, because a channel that has said zero
     * and a channel that has said nothing are opposites.
     */
    expect("ending the two that are still queued", thread_cancel(a),
      SYS_OK);
    expect("and the one that outranked them", thread_cancel(d), SYS_OK);
    depth_wait(4);

    /*
     * And the lowest is not always the last, which is this stage's
     * review. The queue is ordered when a sender is inserted and not
     * afterwards, and section 12.3's chain raises a queued sender where
     * it stands: a client that took a request and sent on is raised by
     * whoever arrives behind it, and its position does not move. A
     * queue that gave up its tail would give up exactly that client.
     *
     * The raise is done here by hand rather than by building the chain,
     * because a chain that reaches a queued sender is M12's check and
     * putting one inside this one would be two checks in the place of
     * either.
     */
    x = depth_start("depth-x", PRIO_BOOT + 1, 6);
    y = depth_start("depth-y", PRIO_BOOT + 1, 7);

    n = depth_queue(ch, queued, 4);
    if (n != 2 || queued[0] != x || queued[1] != y) {
        panic("depth: %u senders queued before the raise, not two",
          (unsigned int)n);
    }

    {
        intr_state_t is = md_intr_disable();

        y->t_prio = PRIO_BOOT + 3;

        md_intr_restore(is);
    }

    displaced = channel_senders_displaced;
    z = depth_start("depth-z", PRIO_BOOT + 2, 8);

    depth_wait(5);
    if (depth_res[6] != SYS_ERR_WOULDBLOCK) {
        panic("depth: the lowest client came back %d, not WOULDBLOCK",
          (int)depth_res[6]);
    }
    if (channel_senders_displaced != displaced + 1) {
        panic("depth: %lu displaced by the arrival after the raise, not one",
          (unsigned long)(channel_senders_displaced - displaced));
    }

    n = depth_queue(ch, queued, 4);
    if (n != 2 || queued[0] != z || queued[1] != y) {
        panic("depth: the queue gave up the raised client, not the "
          "lowest one");
    }

    cnprintf("    %-38s %s\n", "a queued client raised in place",
      "kept, and the lowest went instead");

    expect("ending the raised client", thread_cancel(y), SYS_OK);
    expect("and the one that arrived after it", thread_cancel(z), SYS_OK);
    depth_wait(7);

    largs.depth = 0;
    expect("a depth of nothing",
      run(SYS_OP_CHAN_LISTEN, h_recv, &largs, sizeof(largs)), SYS_OK);

    (void)depth_start("depth-e", PRIO_BOOT + 1, 4);
    depth_wait(8);
    if (depth_res[4] != SYS_ERR_WOULDBLOCK) {
        panic("depth: a queue of nothing took a client, %d",
          (int)depth_res[4]);
    }
    if (depth_queue(ch, queued, 4) != 0) {
        panic("depth: a queue of nothing has somebody in it");
    }

    /*
     * And a channel nobody bounded, which is every channel before this
     * stage. It queues, which is what says the depth is something a
     * server asks for rather than something the kernel now does.
     */
    open_ch = channel_create("depth-open", self);
    if (open_ch == NULL) {
        panic("depth: no second channel");
    }
    expect("a second channel, unbounded",
      handle_insert(ht, open_ch, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &h_open_recv), SYS_OK);
    memset(&cargs, 0, sizeof(cargs));
    cargs.rights = SYS_RIGHT_SEND;
    expect("a connection to it",
      run(SYS_OP_CHAN_CONNECT, h_open_recv, &cargs, sizeof(cargs)),
      SYS_OK);
    h_open_send = cargs.out_handle;

    depth_done = 0;
    h_depth_send = h_open_send;
    open_t = depth_start("depth-f", PRIO_BOOT + 1, 5);

    if (depth_queue(open_ch, queued, 4) != 1 || queued[0] != open_t) {
        panic("depth: a channel nobody bounded refused a client");
    }
    if (depth_done != 0) {
        panic("depth: a channel nobody bounded answered a client");
    }

    cnprintf("    %-38s %s\n", "a depth of nothing, and no depth",
      "refused everything, queued everything");

    expect("ending the one that queued", thread_cancel(open_t), SYS_OK);
    depth_wait(1);

    expect("giving the connections back",
      handle_close(ht, h_open_send), SYS_OK);
    expect("and the bounded one's", handle_close(ht, h_first), SYS_OK);
    expect("giving the channels back", handle_close(ht, h_open_recv),
      SYS_OK);
    expect("and the first one", handle_close(ht, h_recv), SYS_OK);
}

#if SYS_TIER == 2

/*
 * What a whole-TLB flush costs, per M19 stage one.
 *
 * Measured before anything changes, because a milestone that removes a
 * cost it never measured is a milestone reporting a number rather than
 * an improvement. The boot prints how many address space switches it
 * makes and says each one is a flush; this says what one of those is
 * worth.
 *
 * Tier 2 only, and that is the measurement rather than a gap in it.
 * There is one address space at Tier 0 and nothing to throw away.
 *
 * A flush does not have one price, and the first version of this check
 * reported as though it did. What it costs is a fixed part, which is
 * the flush itself, and a part that grows with how much the next thread
 * touches before it blocks, because every translation it wants back it
 * has to walk for again. So the walk is timed at two lengths and the
 * two parts are separated: multiplying one working set's answer by the
 * number of switches in a boot is what this arrangement exists to stop
 * a reader doing.
 *
 * What the numbers are the price of, exactly. The pages are frames read
 * through the identity map, and that map is made of megapages, so on
 * real hardware this walk would want one or two translations and not
 * two hundred and fifty-six. It costs what it costs here because an
 * emulator's translation cache is per page of virtual address whatever
 * the guest's leaf describes. That makes these figures the emulator's
 * price rather than any machine's, and the reason to keep them anyway
 * is that a user address space is page granular, so what is measured
 * has the shape of what a program pays even though the absolute numbers
 * are the host's. Every later stage is compared against this same
 * arrangement on this same host, which is what the baseline is for.
 */
#define FLUSH_PAGES     256u
#define FLUSH_SHORT     64u
#define FLUSH_PASSES    8u
#define FLUSH_TRIALS    7u

/*
 * Walk the chain and add up what each page holds. The sum is what says
 * the reads happened: a walk the compiler decided to skip, or one that
 * stopped early, cannot produce it. M11's clock check found the hard
 * way that timing two runs and asserting one took longer passes with
 * the work taken out, so every timed thing here answers with a count
 * as well.
 *
 * A chain rather than an array of addresses, because two hundred and
 * fifty-six of those is two kilobytes and a kernel stack is four. Each
 * page holds the address of the next one and a byte of its own, so the
 * walk carries no index of where it is going and the only memory it
 * touches is the memory being measured. Walking a prefix of it is what
 * makes the short pass the same walk over fewer pages.
 */
static uint32_t
flush_walk(uintptr_t head, uint32_t n)
{
    uintptr_t at = head;
    uint32_t sum = 0;
    uint32_t i;

    for (i = 0; i < n; i++) {
        sum += *(const volatile uint8_t *)(at + sizeof(uintptr_t));
        at = *(const volatile uintptr_t *)at;
    }

    return sum;
}

/* Ticks to nanoseconds, which is the only unit any of this is read in. */
static uint64_t
flush_ns(uint64_t ticks)
{
    return ticks * 1000000000ULL / time_hz();
}

/*
 * Build the chain, and say what a walk of it should add up to.
 *
 * Frames rather than memory of the kernel's own, because a megabyte
 * that exists so that a check can walk it is a megabyte every boot
 * carries for one measurement. Every space maps them, because they are
 * ordinary memory and the identity map is in every space, which is what
 * lets the same chain be walked either side of a switch.
 */
static uintptr_t
chain_build(uint32_t n, uint32_t *want)
{
    uintptr_t head = 0;
    uint32_t i;

    *want = 0;
    for (i = 0; i < n; i++) {
        uintptr_t pg = page_alloc();

        if (pg == 0) {
            panic("chain: no frame for page %u", (unsigned int)i);
        }
        *(volatile uintptr_t *)pg = head;
        *(volatile uint8_t *)(pg + sizeof(uintptr_t)) = (uint8_t)(i + 1);
        *want += (uint8_t)(i + 1);
        head = pg;
    }

    return head;
}

/*
 * And take it apart, answering with how many frames went back.
 *
 * That count and not page_used() before against page_used() after: the
 * frame count is the whole machine's, the other core is running, and
 * M13 stage four already settled that asserting such a count is
 * unchanged is asserting that nothing else moved a frame in the instant
 * this looked.
 */
static uint32_t
chain_free(uintptr_t head)
{
    uint32_t given = 0;

    while (head != 0) {
        uintptr_t next = *(const volatile uintptr_t *)head;

        page_free(head);
        given++;
        head = next;
    }

    return given;
}

/*
 * Whether a switch keeps what it learned, per M19 stage two.
 *
 * Two spaces, one virtual address, two frames behind it. The address is
 * the base of the window programs live in, which is the one range no
 * space maps for the kernel, so what answers at it is whatever the
 * space being asked put there and nothing the kernel arranged.
 *
 * Read from the kernel rather than from two programs, because what is
 * being tested is the hardware's filing and not the loader's. A program
 * would prove the same thing through five other mechanisms, and when it
 * failed it would be the five that were suspected first.
 *
 * The read is what makes this a check. A switch that dropped the
 * identifier still boots, still runs, and still prints, because a space
 * whose translations were thrown away simply walks its tables again.
 * Nothing goes wrong until two spaces disagree about one address, so
 * that is what is asked, and the answer is a byte that says which space
 * it came from.
 */
/*
 * A name handed on, per M19 stage four.
 *
 * There are more spaces in a boot than there are identifiers to give
 * them, because SYS_ASID_LIMIT says so and says why. So a space is
 * built with a name somebody else has been running under, and what it
 * inherits along with the name is whatever the hardware learned while
 * the other one had it. That is the hazard, and it is the reason a name
 * cannot simply be a number handed out twice.
 *
 * Read from the kernel, in one region with interrupts off, the same way
 * stage two's two spaces were read: what is being tested is what
 * happens to a translation cache, and a program would prove it through
 * five other mechanisms and be suspected of them first.
 */
/*
 * And what happens when there are none left, per M19 stage four.
 *
 * A name is a scarce thing and this system caps it well below what the
 * hardware offers, so a machine with enough spaces alive at once runs
 * out. What a space then gets is zero, which is not a name: it is
 * entered by throwing every translation away, so any number of spaces
 * may hold it at once and each of them is correct.
 *
 * Reached by taking them all rather than by making the cap small enough
 * that a boot reaches it by accident. A cap that low would leave the
 * whole system running nameless in order to test one branch of it, and
 * what a check should cost is the check.
 */
/*
 * What the other core is still holding, per M19 stage five.
 *
 * riscv64's fences reach only the core that issues them and aarch64's
 * are inner-shareable and reach both. That is a claim each port makes,
 * and until now nothing had asked the machine whether it was true. It
 * is asked here in the only way that means anything: this core changes
 * what an address maps to, invalidates it, and the other core reads the
 * address without having been dispatched in between. What comes back
 * says which kind of port this is.
 *
 * The read has to be the other core's own, with no switch between it
 * and the one before it. That is not fussiness about isolation, it is
 * the whole window the emulator gives: a translation it is holding
 * survives a handful of instructions and does not survive a thread
 * being dispatched, whatever the kernel does. So this measures what a
 * core is holding, and the rule about what a switch does with that is
 * checked where a rule can be checked, in asid_checks().
 *
 * Both threads run in the kernel's own address space, because that is
 * the space kernel threads run in and this is a check about one core's
 * cache and not about spaces at all.
 */
/*
 * Two bounds, because the two waits are not the same wait. One core
 * waits for the other to be dispatched at all, which takes as long as a
 * scheduler takes and is done by yielding. The other waits for a
 * handshake with a core that is already running and about to answer,
 * which is a few thousand instructions and is done by spinning.
 *
 * Neither is a timeout in the sense of a system that carries on. A wait
 * that runs out here means the arrangement did not happen and every
 * number below it would be about something else, so it panics.
 */
#define STALE_YIELDS    4000000u
#define STALE_SPINS     100000000u

static volatile uint32_t stale_step;
static volatile uint8_t stale_before;
static volatile uint8_t stale_during;
static volatile uint8_t stale_after;
static uintptr_t stale_va;

static void
stale_first(void *arg)
{
    intr_state_t is;
    uint32_t spin;

    (void)arg;

    /*
     * Running on the other core, and saying so with interrupts still
     * on. Waiting to be dispatched is a scheduler's business and takes
     * a scheduler's time, so it is done out here where a wait costs
     * nothing.
     */
    md_cpu_fence();
    stale_step = 1;

    for (spin = 0; spin < STALE_SPINS && stale_step < 2; spin++) {
        md_cpu_fence();
    }
    if (stale_step < 2) {
        panic("stale: the other core waited out its turn to start");
    }

    /*
     * Then both reads with interrupts off and nothing but a spin
     * between them. Nothing else may run on this core in that window:
     * the first read is what puts the translation here, and anything
     * dispatched in between takes it away by itself, on this emulator,
     * whatever the kernel did.
     *
     * What the window holds is a handshake with a core that is already
     * spinning for it, and one entry written into a page table that
     * already exists. It used to hold the other core noticing, which is
     * a scheduler away, and that made this the longest region in the
     * boot by an order of magnitude. Section 13.3 counts these.
     */
    is = md_intr_disable();

    stale_before = *(const volatile uint8_t *)stale_va;
    md_cpu_fence();
    stale_step = 3;

    for (spin = 0; spin < STALE_SPINS && stale_step < 4; spin++) {
        md_cpu_fence();
    }

    stale_during = *(const volatile uint8_t *)stale_va;
    md_cpu_fence();
    stale_step = 5;

    md_intr_restore(is);

    if (spin >= STALE_SPINS) {
        panic("stale: the other core waited out the change");
    }

    while (stale_step < 7) {
        sched_yield();
    }

    thread_exit(0);
}

/*
 * And the second reader, which has never run before this and is made
 * ready only after the change. Its first instruction is reached by
 * being dispatched, so what it reads is what the address means to a
 * core that has just been given work.
 */
static void
stale_second(void *arg)
{
    (void)arg;

    stale_after = *(const volatile uint8_t *)stale_va;
    md_cpu_fence();
    stale_step = 7;

    thread_exit(0);
}

/*
 * One broad invalidation against two narrow ones, per M19 stage six.
 *
 * M6's review said that once entries are tagged, one call would stop
 * covering both spaces and the buffer path would need two. It does not:
 * both ports have a by-address form that names no identifier and
 * reaches every space, and it is what the buffer path has always used.
 * So the question is not whether one call still works but whether it is
 * still the cheaper way to move a page, and that is a measurement.
 *
 * What is timed is what the buffer path would do either way: one call
 * that removes an address everywhere, against two that remove it from
 * the two spaces the page is actually moving between. The two spaces
 * are real ones with the address really mapped in both, because an
 * invalidation naming a space nothing holds is not the work being
 * priced.
 *
 * Counts asserted and durations reported, per M9. An empty run of the
 * same length is timed beside them, because a loop of calls the
 * compiler decided to drop would be the fastest of all of them, and
 * M11's clock check found that out the hard way.
 */
#define INVAL_ROUNDS    8u
#define INVAL_TRIALS    16u

static void
invalidate_cost_checks(void)
{
    struct addrspace one, two;
    uintptr_t va, pa_one, pa_two;
    size_t len;
    uint64_t bare = 0, broad = 0, one_narrow = 0, narrow = 0;
    uint32_t i, t;

    cnputs("a page moved: one call that names no space, or two that "
      "do\n");

    vm_user_window(&va, &len);

    if (vm_as_create(&one) != SYS_OK || vm_as_create(&two) != SYS_OK) {
        panic("moved: no room for two address spaces");
    }
    pa_one = page_alloc();
    pa_two = page_alloc();
    if (pa_one == 0 || pa_two == 0) {
        panic("moved: no frame to move");
    }
    if (md_map(&one, va, pa_one, PAGE_SIZE, VM_PROGRAM) != SYS_OK ||
      md_map(&two, va, pa_two, PAGE_SIZE, VM_PROGRAM) != SYS_OK) {
        panic("moved: the address would not map");
    }

    for (t = 0; t < INVAL_TRIALS; t++) {
        intr_state_t is;
        uint64_t t0, t1, t2, t3, t4, t5, t6, t7;

        /*
         * One trial at a time with interrupts off, and the shortest of
         * all of them, which is M19 stage one's discipline and its
         * reasons.
         *
         * Eight rounds and not two hundred and fifty-six, which is M19
         * stage five's lesson about what a check may cost: an
         * invalidation is ten microseconds here, so the longer run
         * held interrupts off for eight milliseconds and became the
         * longest region in the boot by a factor of fifteen. Eight is
         * still hundreds of ticks, which is resolution enough.
         */
        is = md_intr_disable();

        t0 = time_now();
        for (i = 0; i < INVAL_ROUNDS; i++) {
            __asm__ volatile ("" ::: "memory");
        }
        t1 = time_now();

        t2 = time_now();
        for (i = 0; i < INVAL_ROUNDS; i++) {
            md_tlb_invalidate(va);
        }
        t3 = time_now();

        t4 = time_now();
        for (i = 0; i < INVAL_ROUNDS; i++) {
            md_tlb_invalidate_in(one.as_asid, va);
        }
        t5 = time_now();

        t6 = time_now();
        for (i = 0; i < INVAL_ROUNDS; i++) {
            md_tlb_invalidate_in(one.as_asid, va);
            md_tlb_invalidate_in(two.as_asid, va);
        }
        t7 = time_now();

        md_intr_restore(is);

        if (t == 0 || t1 - t0 < bare) {
            bare = t1 - t0;
        }
        if (t == 0 || t3 - t2 < broad) {
            broad = t3 - t2;
        }
        if (t == 0 || t5 - t4 < one_narrow) {
            one_narrow = t5 - t4;
        }
        if (t == 0 || t7 - t6 < narrow) {
            narrow = t7 - t6;
        }
    }

    /*
     * The calls happened, which the durations cannot say on their own.
     */
    if (broad <= bare) {
        panic("moved: %lu ticks for %u broad calls against %lu for "
          "none", (unsigned long)broad, (unsigned int)INVAL_ROUNDS,
          (unsigned long)bare);
    }
    if (narrow <= bare) {
        panic("moved: %lu ticks for %u narrow calls against %lu for "
          "none", (unsigned long)narrow,
          (unsigned int)(INVAL_ROUNDS * 2), (unsigned long)bare);
    }

    cnprintf("    %-38s %u pages, %u times each\n",
      "what is invalidated", 1u,
      (unsigned int)(INVAL_ROUNDS * INVAL_TRIALS));
    if (one_narrow <= bare) {
        panic("moved: %lu ticks for %u narrow calls against %lu for "
          "none", (unsigned long)one_narrow, (unsigned int)INVAL_ROUNDS,
          (unsigned long)bare);
    }

    cnprintf("    %-38s %lu ns\n", "one call naming no space",
      (unsigned long)(flush_ns(broad - bare) / INVAL_ROUNDS));
    cnprintf("    %-38s %lu ns\n", "one that names one space",
      (unsigned long)(flush_ns(one_narrow - bare) / INVAL_ROUNDS));
    cnprintf("    %-38s %lu ns\n", "two that name the two spaces",
      (unsigned long)(flush_ns(narrow - bare) / INVAL_ROUNDS));
    cnprintf("    %-38s %s\n", "so a page moved is cheaper with",
      broad - bare <= narrow - bare ? "the one broad call" :
      "the two narrow ones");

    /*
     * And whether the narrow call does anything, which the timings
     * cannot say and the first version of this stage never asked. A
     * call that dropped its argument on the floor would cost the same,
     * come out of the comparison the same way, and invalidate nothing.
     *
     * Asked of the space this core is running in, because that is the
     * one whose translations this core is holding. The address is
     * mapped in the kernel's own space, read once to put it in the
     * cache, then made to name a different frame twice over: once
     * invalidated in the space that holds it, and once in a space that
     * does not.
     *
     * The first must show the new frame and is asserted. The second is
     * reported and not asserted, because over-invalidating is something
     * a machine is allowed to do: an implementation may throw away more
     * than it was asked to and stay correct. Both of these do. Naming
     * another space drops this one's entry on both ports, so the narrow
     * form is not narrow here whatever the instruction means, which is
     * the same thing the timings say from the other side.
     *
     * Both reads are in one region with interrupts off, because a
     * thread dispatched between them takes the translation away by
     * itself on this emulator, which is M19 stage five's finding.
     * Rewriting the entry does not: with the invalidation taken out,
     * the second read still shows the frame the first one cached.
     */
    {
        intr_state_t is;
        uint32_t mine = md_as_current_asid();
        uint8_t named, other;

        *(volatile uint8_t *)pa_one = 0x11;
        *(volatile uint8_t *)pa_two = 0x22;

        if (md_map(vm_kernel(), va, pa_one, PAGE_SIZE,
          VM_READ | VM_WRITE) != SYS_OK) {
            panic("moved: the kernel's own space would not map it");
        }
        md_tlb_flush_all();

        is = md_intr_disable();

        (void)*(const volatile uint8_t *)va;

        (void)md_map(vm_kernel(), va, pa_two, PAGE_SIZE,
          VM_READ | VM_WRITE);
        md_tlb_invalidate_in(mine, va);
        named = *(const volatile uint8_t *)va;

        (void)md_map(vm_kernel(), va, pa_one, PAGE_SIZE,
          VM_READ | VM_WRITE);
        md_tlb_invalidate_in(one.as_asid, va);
        other = *(const volatile uint8_t *)va;

        md_intr_restore(is);

        md_unmap(vm_kernel(), va, PAGE_SIZE);
        md_tlb_flush_all();

        if (named != 0x22) {
            panic("moved: naming this space read 0x%x, not 0x22, so the "
              "narrow call reached nothing", (unsigned int)named);
        }

        cnprintf("    %-38s %s\n", "naming this space drops its entry",
          "yes");
        cnprintf("    %-38s %s\n", "naming another space drops it too",
          other == 0x11 ? "yes, so nothing here is narrower" :
          "no, so the narrow form is narrower");
    }

    md_unmap(&one, va, PAGE_SIZE);
    md_unmap(&two, va, PAGE_SIZE);
    md_tlb_flush_all();
    vm_as_destroy(&one);
    vm_as_destroy(&two);
    page_free(pa_one);
    page_free(pa_two);

    /*
     * And nothing is asserted about page_used() here, which the first
     * version of this check did. The frame count is the whole machine's
     * and the other core is running, so an assertion that it is exactly
     * what it was is an assertion that nothing else moved a frame in
     * the instant this looked, which M13 stage four ruled out. What
     * would catch a leak is the boot's own frame ledger, which accounts
     * for every frame still held when the run is over.
     */
}

static void
cross_core_stale_checks(void)
{
    struct thread *first, *second;
    uintptr_t pa_old, pa_new;
    size_t len;
    intr_state_t is;
    uint64_t noted = 0;
    uint32_t spin;
    bool local = false;

    cnputs("the other core: what it is still holding\n");

    if (cpu_count() < 2) {
        cnprintf("    %-38s %s\n", "one core, so nothing to hold it",
          "skipped");
        return;
    }

    vm_user_window(&stale_va, &len);

    pa_old = page_alloc();
    pa_new = page_alloc();
    if (pa_old == 0 || pa_new == 0) {
        panic("stale: no frame to put behind the address");
    }
    *(volatile uint8_t *)pa_old = 0xF1;
    *(volatile uint8_t *)pa_new = 0xF2;

    /*
     * Into the kernel's own space, at the base of the window programs
     * live in, which is the one range the kernel maps nowhere else.
     *
     * It costs one page table that stays. md_unmap takes the entry out
     * and leaves the table it was in, so the boot's ledger reports one
     * more frame added to the kernel's own address space than it did.
     * That is a real frame and it is counted where it went.
     */
    if (md_map(vm_kernel(), stale_va, pa_old, PAGE_SIZE,
      VM_READ | VM_WRITE) != SYS_OK) {
        panic("stale: the address would not map");
    }
    md_tlb_flush_all();

    stale_step = 0;
    stale_before = 0;
    stale_during = 0;
    stale_after = 0;
    md_cpu_fence();

    first = thread_create("stale-a", PRIO_BOOT, stale_first, NULL);
    second = thread_create("stale-b", PRIO_BOOT, stale_second, NULL);
    if (first == NULL || second == NULL) {
        panic("stale: no thread to read with");
    }
    expect("two readers on the other core", sched_migrate(first, 1),
      SYS_OK);
    expect("and the one that follows it", sched_migrate(second, 1),
      SYS_OK);
    sched_ready(first);

    for (spin = 0; spin < STALE_YIELDS && stale_step < 1; spin++) {
        sched_yield();
    }
    if (stale_step < 1) {
        panic("stale: the other core never started");
    }
    md_cpu_fence();

    /*
     * From here to the restore is this core's half of the window, and
     * it is deliberately the same shape as the other core's: a
     * handshake with a core already spinning for it, and one entry
     * written into a page table that already exists, because the
     * mapping above built it.
     *
     * Interrupts off for it, and not only to keep the region short.
     * What the count of maintenance says has to be what this
     * invalidation did, and a thread dispatched here could invalidate
     * something of its own and make a port with inner-shareable fences
     * look like a port without them.
     */
    is = md_intr_disable();

    md_cpu_fence();
    stale_step = 2;

    for (spin = 0; spin < STALE_SPINS && stale_step < 3; spin++) {
        md_cpu_fence();
    }

    if (spin < STALE_SPINS) {
        if (md_map(vm_kernel(), stale_va, pa_new, PAGE_SIZE,
          VM_READ | VM_WRITE) != SYS_OK) {
            panic("stale: the address would not map again");
        }

        /*
         * The address now names a different frame, and this core throws
         * away what it had for it. Whether that reached the other core
         * is the question, and whether this port thinks it did is the
         * count: a port whose fences are inner-shareable notes nothing
         * here, because there is nothing for anybody else to hear
         * about.
         */
        noted = vm_maint_here();
        md_tlb_invalidate(stale_va);
        local = vm_maint_here() != noted;
    }

    md_cpu_fence();
    stale_step = 4;

    md_intr_restore(is);

    if (spin >= STALE_SPINS) {
        panic("stale: the other core never reached its first read");
    }

    for (spin = 0; spin < STALE_YIELDS && stale_step < 5; spin++) {
        sched_yield();
    }
    if (stale_step < 5) {
        panic("stale: the other core never read the second time");
    }
    md_cpu_fence();

    if (stale_before != 0xF1) {
        panic("stale: the other core read 0x%x before the change, not "
          "0xf1", (unsigned int)stale_before);
    }

    /*
     * And the port's claim against what the machine did.
     *
     * A port that says its fences are local must leave the other core
     * holding what it had, because nothing has reached that core yet.
     * A port that says they are shareable must not, because the
     * hardware carried the invalidation across without being asked. A
     * port that is wrong about itself fails here, and this is the only
     * check in M19 the emulator will let fail: everywhere else it
     * throws every translation away on its own and hides the answer.
     */
    if (local && stale_during != 0xF1) {
        panic("stale: this port says its fences are its own, and the "
          "other core read 0x%x rather than what it was holding",
          (unsigned int)stale_during);
    }
    if (!local && stale_during != 0xF2) {
        panic("stale: this port says its fences reach the other core, "
          "and that core read 0x%x", (unsigned int)stale_during);
    }

    /*
     * Then a thread that has to be dispatched to run at all, which is
     * where the kernel's rule takes over from the hardware's. It reads
     * the new page on both ports. That cannot fail here and is asserted
     * anyway: the emulator loses a translation across a dispatch by
     * itself, so what this says is that nothing else went wrong, and
     * not that the rule works. The rule is checked in asid_checks().
     */
    sched_ready(second);
    md_cpu_fence();
    stale_step = 6;

    for (spin = 0; spin < STALE_YIELDS && stale_step < 7; spin++) {
        sched_yield();
    }
    if (stale_step < 7) {
        panic("stale: the other core never read after a dispatch");
    }
    md_cpu_fence();

    if (stale_after != 0xF2) {
        panic("stale: the other core read 0x%x after a dispatch, not "
          "0xf2", (unsigned int)stale_after);
    }

    cnprintf("    %-38s %s\n", "this port's fences reach",
      local ? "the core that issues them" : "every core");
    cnprintf("    %-38s 0x%x, 0x%x, then 0x%x\n",
      "one address read three times there", (unsigned int)stale_before,
      (unsigned int)stale_during, (unsigned int)stale_after);

    md_unmap(vm_kernel(), stale_va, PAGE_SIZE);
    md_tlb_flush_all();
    page_free(pa_old);
    page_free(pa_new);
}

static void
asid_exhaustion_checks(void)
{
    struct addrspace spare[SYS_ASID_LIMIT];
    struct addrspace nameless;
    uintptr_t va, pa;
    size_t len;
    intr_state_t is;
    uint32_t given, reused, none, live;
    uint32_t taken = 0;
    uint32_t frames = page_used();
    uint32_t i;
    uint8_t saw;
    bool flushed;

    vm_user_window(&va, &len);

    /*
     * Every name that is left, and then one space more. The loop is
     * bounded by the cap and not by trust: a create that kept answering
     * with a name would otherwise run until the frames ran out.
     */
    while (taken < SYS_ASID_LIMIT) {
        if (vm_as_create(&spare[taken]) != SYS_OK) {
            panic("names: no room to take the %uth name",
              (unsigned int)taken);
        }
        taken++;
        if (spare[taken - 1].as_asid == 0) {
            break;
        }
    }

    if (spare[taken - 1].as_asid != 0) {
        panic("names: %u spaces built and every one of them got a name",
          (unsigned int)taken);
    }

    nameless = spare[taken - 1];

    /*
     * And a space with no name still runs. Entering it throws
     * everything away, which is what makes it safe, and it reads what
     * it mapped: a name it did not get is not a mapping it did not get.
     */
    pa = page_alloc();
    if (pa == 0) {
        panic("names: no frame for the nameless space");
    }
    *(volatile uint8_t *)pa = 0xE5;
    if (md_map(&nameless, va, pa, PAGE_SIZE, VM_PROGRAM) != SYS_OK) {
        panic("names: the nameless space would not map");
    }

    is = md_intr_disable();
    flushed = md_as_activate(&nameless);
    saw = *(const volatile uint8_t *)va;
    (void)md_as_activate(proc_as(thread_current()->t_proc));
    md_intr_restore(is);

    if (!flushed) {
        panic("names: entering a space with no name kept what it had");
    }
    if (saw != 0xE5) {
        panic("names: the nameless space read 0x%x, not 0xe5",
          (unsigned int)saw);
    }

    vm_asid_counts(&given, &reused, &none, &live);
    cnprintf("    %-38s %u built, %u went without\n",
      "taking every name there is", (unsigned int)taken,
      (unsigned int)none);

    md_unmap(&nameless, va, PAGE_SIZE);
    md_tlb_flush_all();
    page_free(pa);

    for (i = 0; i < taken; i++) {
        vm_as_destroy(&spare[i]);
    }

    /*
     * And the names came back, which is the other half of running out.
     * A pool that hands nothing back after a burst is a pool that has
     * run out permanently, and nothing above here would say so.
     */
    if (vm_as_create(&nameless) != SYS_OK) {
        panic("names: no room for the space that asks for one back");
    }
    if (nameless.as_asid == 0) {
        panic("names: still nothing to give after %u went back",
          (unsigned int)taken);
    }
    vm_as_destroy(&nameless);

    if (page_used() != frames) {
        panic("names: %u frames in use, started with %u",
          (unsigned int)page_used(), (unsigned int)frames);
    }
}

static void
asid_reuse_checks(void)
{
    struct addrspace one, two;
    struct addrspace *back;
    uintptr_t va, pa_one, pa_two;
    size_t len;
    intr_state_t is;
    struct vm_cost before, after;
    uint32_t given, reused, none, live;
    uint32_t asid;
    uint8_t saw, saw_first;

    cnputs("names: what a space inherits with a second-hand one\n");

    vm_user_window(&va, &len);
    back = proc_as(thread_current()->t_proc);

    pa_one = page_alloc();
    pa_two = page_alloc();
    if (pa_one == 0 || pa_two == 0) {
        panic("names: no frame to put behind the address");
    }
    *(volatile uint8_t *)pa_one = 0xC3;
    *(volatile uint8_t *)pa_two = 0xD4;

    /*
     * The first space, run in long enough for the hardware to have
     * learned the one address that matters. Reading it is what puts a
     * translation in the cache under this name; without the read there
     * would be nothing to inherit and the check would pass on a system
     * that recycles names wrongly.
     */
    if (vm_as_create(&one) != SYS_OK) {
        panic("names: no room for the first space");
    }
    asid = one.as_asid;
    if (asid == 0) {
        panic("names: the first space got no identifier at all");
    }
    if (md_map(&one, va, pa_one, PAGE_SIZE, VM_PROGRAM) != SYS_OK) {
        panic("names: the address would not map");
    }

    is = md_intr_disable();
    (void)md_as_activate(&one);
    saw = *(const volatile uint8_t *)va;
    (void)md_as_activate(back);
    md_intr_restore(is);

    saw_first = saw;
    if (saw_first != 0xC3) {
        panic("names: the first space read 0x%x, not 0xc3",
          (unsigned int)saw_first);
    }

    md_unmap(&one, va, PAGE_SIZE);

    vm_cost_read(&before);
    vm_as_destroy(&one);
    vm_cost_read(&after);

    /*
     * The name went back, and going back is what throws its
     * translations away. Asserted as a count, because on this host the
     * consequence cannot be observed: writing the root register makes
     * the emulator discard everything whatever the identifier says, so
     * the read below would come out right even from a system that never
     * invalidated anything. What can be false is whether the kernel
     * asked.
     */
    if (after.vc_asid_inval - before.vc_asid_inval != 1) {
        panic("names: %lu identifiers thrown away when one space died",
          (unsigned long)(after.vc_asid_inval - before.vc_asid_inval));
    }

    /*
     * The next space built takes it, because names go back last-first
     * and this one went back last. That is arranged rather than hoped
     * for: a check that took whichever name came up would be a check
     * that usually tested nothing.
     */
    if (vm_as_create(&two) != SYS_OK) {
        panic("names: no room for the second space");
    }
    if (two.as_asid != asid) {
        panic("names: the second space got %u, not the %u the first "
          "handed back", (unsigned int)two.as_asid, (unsigned int)asid);
    }
    if (md_map(&two, va, pa_two, PAGE_SIZE, VM_PROGRAM) != SYS_OK) {
        panic("names: the address would not map twice");
    }

    is = md_intr_disable();
    (void)md_as_activate(&two);
    saw = *(const volatile uint8_t *)va;
    (void)md_as_activate(back);
    md_intr_restore(is);

    if (saw != 0xD4) {
        panic("names: the second space read 0x%x at the address the "
          "first had, not 0xd4", (unsigned int)saw);
    }

    cnprintf("    %-38s %u, twice\n", "one name, two spaces",
      (unsigned int)asid);
    cnprintf("    %-38s 0x%x, then 0x%x\n",
      "the same address in each of them", (unsigned int)saw_first,
      (unsigned int)saw);

    md_unmap(&two, va, PAGE_SIZE);
    md_tlb_flush_all();
    vm_as_destroy(&two);
    page_free(pa_one);
    page_free(pa_two);

    /*
     * And what the boot did on its own, which is the half this check
     * cannot arrange. Reported rather than compared, because how many
     * spaces a boot builds depends on how far the session got.
     *
     * Reuse is not what the cap causes and the count is not evidence
     * that it does: a name goes back the moment its space dies and the
     * next space built takes it, which happens whatever the cap is.
     * What is asserted is that no space has had to run without a name,
     * because the cap is chosen to be above the number live at once and
     * a boot that reached zero would mean it is not.
     */
    vm_asid_counts(&given, &reused, &none, &live);

    cnprintf("    %-38s %u of %u, %u given a second time\n",
      "names in use, of the cap", (unsigned int)live,
      (unsigned int)SYS_ASID_LIMIT, (unsigned int)reused);

    if (reused == 0) {
        panic("names: %u handed out and none of them twice",
          (unsigned int)given);
    }
    if (none != 0) {
        panic("names: %u spaces got none at all, so %u is too few",
          (unsigned int)none, (unsigned int)SYS_ASID_LIMIT);
    }

    asid_exhaustion_checks();
}


static void
asid_checks(void)
{
    struct addrspace one, two;
    struct addrspace *back;
    uintptr_t va, pa_one, pa_two;
    size_t len;
    intr_state_t is;
    uint8_t saw_one, saw_two, saw_again;
    uint32_t told_one, told_two;
    uint32_t flushed = 0;
    bool tagged;
    uint32_t want;
    uint32_t bits = md_asid_bits();

    cnputs("spaces: whether a switch keeps what it learned\n");

    if (bits == 0) {
        cnprintf("    %-38s %s\n", "identifiers this port has",
          "none, so every switch still flushes");
    } else {
        cnprintf("    %-38s %u bits, %u to give\n",
          "identifiers this port has", (unsigned int)bits,
          (unsigned int)(1u << bits) - 1u);
    }

    vm_user_window(&va, &len);

    if (vm_as_create(&one) != SYS_OK || vm_as_create(&two) != SYS_OK) {
        panic("spaces: no room for two address spaces");
    }
    if (bits != 0 && one.as_asid == two.as_asid) {
        panic("spaces: both were given identifier %u",
          (unsigned int)one.as_asid);
    }

    pa_one = page_alloc();
    pa_two = page_alloc();
    if (pa_one == 0 || pa_two == 0) {
        panic("spaces: no frame to put behind the address");
    }
    *(volatile uint8_t *)pa_one = 0xA1;
    *(volatile uint8_t *)pa_two = 0xB2;

    if (md_map(&one, va, pa_one, PAGE_SIZE, VM_PROGRAM) != SYS_OK ||
      md_map(&two, va, pa_two, PAGE_SIZE, VM_PROGRAM) != SYS_OK) {
        panic("spaces: the address would not map");
    }

    /*
     * What the leaves say, before anything is run in either space.
     *
     * This is the half of the stage the two ports do in opposite
     * directions, and the half a working system does not notice is
     * missing. An identifier written into the root register while the
     * entries call themselves global is a number nothing reads: every
     * space would share every translation and the machine would run,
     * because a translation cache that answers for the wrong space
     * answers with the right page until two spaces disagree. So the
     * descriptor is read back rather than inferred.
     *
     * The kernel's own address as well as the program's, because M19
     * stage two decided that nothing is global and a grant is the
     * reason. An entry the kernel put in every space is exactly the one
     * somebody would be tempted to mark shared.
     */
    if (md_leaf_tagged(&one, va, &tagged) != SYS_OK || !tagged) {
        panic("spaces: the leaf for a program's page is not tagged");
    }
    if (md_leaf_tagged(&one, pa_one, &tagged) != SYS_OK || !tagged) {
        panic("spaces: the leaf for the kernel's own memory is not "
          "tagged");
    }

    cnprintf("    %-38s %s\n", "leaves say which space they belong to",
      "the program's and the kernel's");

    back = proc_as(thread_current()->t_proc);

    /*
     * Counted here and not through vm_count_switch(), because these are
     * not dispatches. The boot report's switch count is what a thread
     * being put on a core costs, and four activations a check made for
     * itself have no business in it.
     *
     * Interrupts off for all four, because the thing being measured is
     * what survives between two of them and a dispatch in the middle
     * would be a third space nobody asked for. Restored to the space
     * this thread was running in before anything else can look, so the
     * dispatcher's idea of what is current is left as it found it.
     */
    is = md_intr_disable();

    flushed += md_as_activate(&one) ? 1u : 0u;
    told_one = md_as_current_asid();
    saw_one = *(const volatile uint8_t *)va;

    flushed += md_as_activate(&two) ? 1u : 0u;
    told_two = md_as_current_asid();
    saw_two = *(const volatile uint8_t *)va;

    flushed += md_as_activate(&one) ? 1u : 0u;
    saw_again = *(const volatile uint8_t *)va;

    flushed += md_as_activate(back) ? 1u : 0u;

    md_intr_restore(is);

    /*
     * What the hardware was told, read back out of the register rather
     * than taken from the structure that was written into it. This is
     * the half of the stage that can fail on this host: the reads below
     * cannot, and the number further down says why.
     */
    if (told_one != one.as_asid || told_two != two.as_asid) {
        panic("spaces: the hardware was running under %u and %u, not %u "
          "and %u", (unsigned int)told_one, (unsigned int)told_two,
          (unsigned int)one.as_asid, (unsigned int)two.as_asid);
    }

    if (saw_one != 0xA1 || saw_two != 0xB2 || saw_again != 0xA1) {
        panic("spaces: read 0x%x, 0x%x and 0x%x, wanted 0xa1, 0xb2 and "
          "0xa1", (unsigned int)saw_one, (unsigned int)saw_two,
          (unsigned int)saw_again);
    }

    cnprintf("    %-38s %u, then %u\n", "the identifier the hardware ran "
      "under", (unsigned int)told_one, (unsigned int)told_two);
    cnprintf("    %-38s 0x%x, 0x%x, then 0x%x again\n",
      "one address in two spaces reads", (unsigned int)saw_one,
      (unsigned int)saw_two, (unsigned int)saw_again);

    /*
     * And what it cost to be right about it, which is the half of this
     * stage the reads cannot show. All four threw everything away
     * before, and none of them does now.
     */
    if (flushed != (bits == 0 ? 4u : 0u)) {
        panic("spaces: %u of 4 switches flushed, with %u bits",
          (unsigned int)flushed, (unsigned int)bits);
    }

    cnprintf("    %-38s %u of 4\n", "switches that flushed everything",
      (unsigned int)flushed);

    /*
     * And the one thing about the maintenance counter a single core can
     * check: its own work does not make it flush.
     *
     * A core that invalidated something is not the core still holding
     * it, so what the counter answers is what the other cores have
     * done. One counter for the machine would answer differently here,
     * and the difference is 2546 flushes in a boot against 7. What this
     * cannot check is the direction that matters, which is the other
     * core's maintenance reaching this one; that needs two cores
     * arranged on purpose and is M19 stage five's.
     */
    {
        uint64_t seen = vm_maint_epoch();

        md_tlb_flush_all();

        if (vm_maint_epoch() != seen) {
            panic("spaces: this core's own maintenance moved its own "
              "count, %lu to %lu", (unsigned long)seen,
              (unsigned long)vm_maint_epoch());
        }
    }

    /*
     * And whether the host agrees, which the reads above cannot say.
     *
     * The kernel not flushing is a property of the kernel and the count
     * says so. Whether anything was kept is a property of the machine
     * underneath, and the way to ask is to walk the same pages twice:
     * once without leaving the space, and once having just entered
     * another one. Two numbers that match mean the switch kept what it
     * learned. A switch that costs what a flush costs means the machine
     * threw it away whatever the identifier said.
     *
     * The chain is ordinary memory and the identity map is in every
     * space, so it is the same walk over the same frames on both sides.
     */
    {
        uintptr_t chain = chain_build(FLUSH_PAGES, &want);
        uint64_t kept = 0, moved = 0;
        uint32_t sum = 0;
        uint32_t t;

        for (t = 0; t < FLUSH_TRIALS; t++) {
            uint64_t t0, t1, t2, t3;

            is = md_intr_disable();

            (void)md_as_activate(&one);
            sum += flush_walk(chain, FLUSH_PAGES);

            t0 = time_now();
            sum += flush_walk(chain, FLUSH_PAGES);
            t1 = time_now();

            (void)md_as_activate(&two);

            t2 = time_now();
            sum += flush_walk(chain, FLUSH_PAGES);
            t3 = time_now();

            (void)md_as_activate(back);

            md_intr_restore(is);

            if (t == 0 || t1 - t0 < kept) {
                kept = t1 - t0;
            }
            if (t == 0 || t3 - t2 < moved) {
                moved = t3 - t2;
            }
        }

        if (sum != want * FLUSH_TRIALS * 3) {
            panic("spaces: the walks read %u, not %u", (unsigned int)sum,
              (unsigned int)(want * FLUSH_TRIALS * 3));
        }
        if (chain_free(chain) != FLUSH_PAGES) {
            panic("spaces: the chain did not all come back");
        }

        cnprintf("    %-38s %lu ns against %lu\n",
          "a walk after a switch, and without one",
          (unsigned long)flush_ns(moved), (unsigned long)flush_ns(kept));
    }

    /*
     * And the path neither port takes any more.
     *
     * Zero means the absence of an identifier, and a space holding it
     * is entered by throwing every translation away. That is what a
     * port with none to give does for every space and what this system
     * will do when it runs out, which is M19 stage four. Both ports
     * answer sixteen bits now, so nothing reaches that path in a boot
     * and it would sit unrun until the day it mattered. One space is
     * asked to hold zero here, and what is checked is that entering it
     * flushes and that it still reads its own page: an identifier taken
     * away is not a mapping taken away.
     */
    {
        uint32_t asid = one.as_asid;
        uint8_t saw;
        bool flushed_zero;

        one.as_asid = 0;

        is = md_intr_disable();
        flushed_zero = md_as_activate(&one);
        saw = *(const volatile uint8_t *)va;
        (void)md_as_activate(back);
        md_intr_restore(is);

        one.as_asid = asid;

        if (!flushed_zero) {
            panic("spaces: entering a space with no identifier kept "
              "what it had");
        }
        if (saw != 0xA1) {
            panic("spaces: a space with no identifier read 0x%x, not "
              "0xa1", (unsigned int)saw);
        }

        cnprintf("    %-38s %s\n", "a space with no identifier at all",
          "flushed, and read its own page");
    }

    /*
     * And the rule that decides whether a switch enters a space at all,
     * asked directly, about the two cases a boot does not reach.
     *
     * The first is a space whose root and name a core already has,
     * which must not be entered again: that is the saving, and if it
     * were false every switch would still flush and nothing above here
     * would notice. The second is a space with no name, which must be
     * entered every time. Zero is not a name, so any number of spaces
     * hold it, and two of them can share a root once a frame has been
     * freed and handed out again; a core that recognised the pair would
     * run one of them through the other's translations. Nothing in a
     * boot builds two nameless spaces, so nothing in a boot would find
     * that out.
     */
    {
        uint64_t heard = vm_maint_epoch();
        uint32_t asid = one.as_asid;

        if (vm_as_needs_entry(&one, one.as_root, one.as_asid, heard,
          heard)) {
            panic("spaces: a space a core is already running would be "
              "entered again");
        }
        if (!vm_as_needs_entry(&two, one.as_root, one.as_asid, heard,
          heard)) {
            panic("spaces: a different space would not be entered");
        }

        one.as_asid = 0;
        if (!vm_as_needs_entry(&one, one.as_root, 0, heard, heard)) {
            panic("spaces: a space with no name would not be entered, "
              "so two of them could share a core's translations");
        }
        one.as_asid = asid;

        cnprintf("    %-38s %s\n", "what a switch enters and skips",
          "the same space no, a nameless one yes");
    }


    md_unmap(&one, va, PAGE_SIZE);
    md_unmap(&two, va, PAGE_SIZE);

    /*
     * md_unmap does no maintenance and says so: an entry gone from the
     * table and still in the translation cache is a mapping that is
     * still there. These two spaces are about to be destroyed and their
     * frames handed back, so what would be left behind is a translation
     * to somebody else's memory, filed under an identifier nothing
     * hands out twice yet. That last clause is the whole of why it does
     * not matter today and none of why it would be right to leave.
     */
    md_tlb_flush_all();

    vm_as_destroy(&one);
    vm_as_destroy(&two);
    page_free(pa_one);
    page_free(pa_two);
}

static void
flush_cost_checks(void)
{
    uintptr_t head = 0;
    uint64_t warm = 0;
    uint64_t cold = 0;
    uint64_t part = 0;
    uint64_t step, fixed;
    uint32_t want = 0;
    uint32_t want_short = 0;
    uint32_t sum = 0;
    uint32_t given = 0;
    uint32_t i, t;

    cnputs("a switch: what throwing every translation away costs\n");

    head = chain_build(FLUSH_PAGES, &want);

    /*
     * What a walk of the last sixty-four links adds up to, which is the
     * short pass's answer. The chain runs newest first, so the prefix a
     * short walk covers is what the build put in last.
     */
    for (i = FLUSH_PAGES - FLUSH_SHORT; i < FLUSH_PAGES; i++) {
        want_short += (uint8_t)(i + 1);
    }

    for (t = 0; t < FLUSH_TRIALS; t++) {
        uint32_t p;

        for (p = 0; p < FLUSH_PASSES; p++) {
            intr_state_t is;
            uint64_t t0, t1, t2, t3, t4, t5;

            /*
             * The flushed walk first and the warm one straight after
             * it, in one region with interrupts off.
             *
             * The warm walk is warm because the flushed walk it
             * follows installed exactly the translations it wants,
             * and nothing at all runs between them. Keeping that so
             * is not about interrupts. What runs between two samples
             * is whatever the scheduler had, and dispatching anything
             * in another address space is itself a whole-TLB flush:
             * with the samples in separate regions the burst session
             * reported a warm walk eleven times slower than the paced
             * one, because a console with input waiting gets
             * dispatched in between and the warm sample was not warm.
             * That is the cost this stage is measuring, arriving
             * uninvited in the measurement of it.
             *
             * One pass at a time and the shortest of all of them,
             * because timing the whole run at once measures how much
             * of the host the emulator was getting. The pages are
             * counted in hundreds for the same reason from the other
             * side: the warm walk has to be long enough that a hiccup
             * of the host's is small next to it, and at sixty-four
             * pages it was not, so the burst session's shortest warm
             * walk was eight times the paced session's.
             */
            is = md_intr_disable();

            t0 = time_now();
            md_tlb_flush_all();
            sum += flush_walk(head, FLUSH_PAGES);
            t1 = time_now();

            t2 = time_now();
            sum += flush_walk(head, FLUSH_PAGES);
            t3 = time_now();

            t4 = time_now();
            md_tlb_flush_all();
            sum += flush_walk(head, FLUSH_SHORT);
            t5 = time_now();

            md_intr_restore(is);

            if ((t == 0 && p == 0) || t1 - t0 < cold) {
                cold = t1 - t0;
            }
            if ((t == 0 && p == 0) || t3 - t2 < warm) {
                warm = t3 - t2;
            }
            if ((t == 0 && p == 0) || t5 - t4 < part) {
                part = t5 - t4;
            }
        }
    }

    /*
     * The reads happened, which the durations cannot say: a walk the
     * compiler decided to skip would be the fastest of all of them.
     */
    if (sum != (want * 2 + want_short) * FLUSH_PASSES * FLUSH_TRIALS) {
        panic("flush: the walks read %u, not %u", (unsigned int)sum,
          (unsigned int)((want * 2 + want_short) * FLUSH_PASSES *
          FLUSH_TRIALS));
    }

    /*
     * Three relations, which is all that is asserted about durations
     * here. A flushed walk costs more than a warm one; a flushed walk
     * over more pages costs more than one over fewer; and a flushed
     * walk over the short set costs more than its pages alone, which is
     * the flush having a price of its own. If the third were false the
     * two parts below would not be two parts.
     */
    if (cold < warm * 2) {
        panic("flush: %lu ticks flushed against %lu warm, which is not "
          "a cost", (unsigned long)cold, (unsigned long)warm);
    }
    if (cold <= part) {
        panic("flush: %lu ticks over %u pages against %lu over %u, "
          "which does not grow", (unsigned long)cold,
          (unsigned int)FLUSH_PAGES, (unsigned long)part,
          (unsigned int)FLUSH_SHORT);
    }

    step = (flush_ns(cold) - flush_ns(part)) / (FLUSH_PAGES - FLUSH_SHORT);
    if (flush_ns(part) <= step * FLUSH_SHORT) {
        panic("flush: %lu ns over %u pages is %lu a page and nothing "
          "for the flush", (unsigned long)flush_ns(part),
          (unsigned int)FLUSH_SHORT, (unsigned long)step);
    }
    fixed = flush_ns(part) - step * FLUSH_SHORT;

    cnprintf("    %-38s %u pages, %u walks and %u short\n",
      "what the walk touches", (unsigned int)FLUSH_PAGES,
      (unsigned int)(FLUSH_PASSES * FLUSH_TRIALS * 2),
      (unsigned int)(FLUSH_PASSES * FLUSH_TRIALS));
    cnprintf("    %-38s %lu ns against %lu\n",
      "the shortest walk flushed, and warm",
      (unsigned long)flush_ns(cold), (unsigned long)flush_ns(warm));
    cnprintf("    %-38s %lu ns\n", "the same walk over 64 of the pages",
      (unsigned long)flush_ns(part));
    cnprintf("    %-38s about %lu ns\n", "so the flush itself",
      (unsigned long)fixed);
    cnprintf("    %-38s about %lu ns\n",
      "and each page it makes the walk want",
      (unsigned long)(step - flush_ns(warm) / FLUSH_PAGES));

    given = chain_free(head);
    if (given != FLUSH_PAGES) {
        panic("flush: handed back %u frames of %u", (unsigned int)given,
          (unsigned int)FLUSH_PAGES);
    }
}

#endif /* SYS_TIER == 2 */

/*
 * The deaths a server is not told about, per M18 stage two.
 *
 * Section 12.5 announces one death and is explicit about which: a
 * client whose request a server is holding. A client that dies on the
 * queue is taken off a queue nobody looked at, and section 12.1 has a
 * client that gives up at its deadline abandon work in progress with
 * the server finding out when its reply is refused. Neither is a
 * notice, and both are cases a server keeping records has to survive.
 *
 * Arranged on channels this thread serves itself rather than through
 * `ledger`, because the ledger has one thread that holds and one that
 * answers, and neither can be made to leave a client queued or to sit
 * on a request past a deadline. What is read is the channel's pulse
 * queue, which is where a notice would be whether anyone collected it
 * or not.
 */
static void
notice_absence_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct sys_op_connect_args cargs;
    struct sys_msg msg, rep;
    sys_thread_id_t peer;
    sys_ns_spec_t ns[2];
    struct channel *ch;
    struct child c;
    const char *argv[2];
    sys_handle_t hrecv, keep;
    uint64_t cookie, waited;
    int32_t code;

    cnputs("a server: the deaths it is not told about\n");

    ch = channel_create("notice", self);
    if (ch == NULL) {
        panic("notice: no channel");
    }
    expect("a channel with nobody receiving on it",
      handle_insert(ht, ch, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &hrecv), SYS_OK);

    /*
     * A client killed while queued. Nothing is at the receive point, so
     * its message is on the channel's sender list and no server has
     * ever seen it.
     */
    memset(&cargs, 0, sizeof(cargs));
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a connection for a client that will queue",
      run(SYS_OP_CHAN_CONNECT, hrecv, &cargs, sizeof(cargs)), SYS_OK);

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_PRIVATE + 1;
    ns[0].handle = cargs.out_handle;
    ns[1].id = SYS_NS_PRIVATE + 0;
    if (handle_insert(ht, ledger_never, SYS_OBJ_TIMELINE,
      SYS_RIGHT_WAIT | SYS_RIGHT_TRANSFER, &ns[1].handle) != SYS_OK) {
        panic("notice: no handle for the timeline");
    }
    argv[0] = "patron";
    argv[1] = "hold";

    sched_defer();
    child_watch(&c, proc_spawn_image("patron", 20, ns, 2, argv, 2));
    if (handle_insert(ht, c.c_proc, SYS_OBJ_PROCESS, SYS_RIGHT_STAT,
      &keep) != SYS_OK) {
        panic("notice: no handle slot for the client");
    }
    sched_undefer();

    /*
     * Queued, and not being served. The state is what says which of
     * section 12.1's two cases this is, and a check that killed a
     * client which had not reached the queue would be testing nothing.
     */
    waited = time_now() + time_from_ms(JOIN_MS);
    {
        struct thread *ct = proc_threads(c.c_proc);

        while (ct->t_wait != THREAD_WAIT_SEND) {
            if (time_now() > waited) {
                panic("notice: the client never queued, wait %u",
                  (unsigned int)ct->t_wait);
            }
            sched_yield();
        }
    }

    if (proc_kill(c.c_proc, KILL_CODE) != SYS_OK) {
        panic("notice: the queued client refused to be killed");
    }
    code = child_join(&c, JOIN_MS);
    if (code != KILL_CODE) {
        panic("notice: the queued client came back %d", (int)code);
    }

    if (ch->c_pulse_count != 0) {
        panic("notice: %u pulses for a client that died on the queue",
          (unsigned int)ch->c_pulse_count);
    }
    if (ch->c_senders != NULL) {
        panic("notice: the queue still names a client that has gone");
    }
    expect("giving the queued client back", handle_close(ht, keep),
      SYS_OK);

    cnprintf("    %-38s %s\n", "a client that died on the queue",
      "no notice, and off the queue");

    /*
     * And a client that gives up at its deadline while this thread
     * holds its request. Section 12.1: it abandons work in progress,
     * the server is not told, and the reply is what discovers it.
     */
    memset(&cargs, 0, sizeof(cargs));
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a connection for a client that will give up",
      run(SYS_OP_CHAN_CONNECT, hrecv, &cargs, sizeof(cargs)), SYS_OK);

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_PRIVATE + 1;
    ns[0].handle = cargs.out_handle;
    ns[1].id = SYS_NS_PRIVATE + 0;
    if (handle_insert(ht, ledger_never, SYS_OBJ_TIMELINE,
      SYS_RIGHT_WAIT | SYS_RIGHT_TRANSFER, &ns[1].handle) != SYS_OK) {
        panic("notice: no handle for the timeline");
    }
    argv[1] = "giveup";

    sched_defer();
    child_watch(&c, proc_spawn_image("patron", 20, ns, 2, argv, 2));
    if (handle_insert(ht, c.c_proc, SYS_OBJ_PROCESS, SYS_RIGHT_STAT,
      &keep) != SYS_OK) {
        panic("notice: no handle slot for the client");
    }
    sched_undefer();

    /*
     * This thread becomes the server, and owes a reply from here until
     * the end of this block. It may not receive again while it does,
     * which is stage one's review, and it does not.
     */
    memset(&msg, 0, sizeof(msg));
    expect("taking the request it will not answer",
      channel_recv(ch, &msg, &peer, &cookie), SYS_OK);

    code = child_join(&c, JOIN_MS);
    if (code != 0) {
        panic("notice: the client that gave up came back %d", (int)code);
    }

    if (ch->c_pulse_count != 0) {
        panic("notice: %u pulses for a client that gave up",
          (unsigned int)ch->c_pulse_count);
    }

    /*
     * And this is how the server finds out, which is the whole of what
     * section 12.1 offers it: a refused reply, after the fact.
     */
    memset(&rep, 0, sizeof(rep));
    rep.handle = SYS_HANDLE_INVALID;
    expect("and the reply it makes anyway", channel_reply(peer, &rep),
      SYS_ERR_STALE);

    expect("giving that client back", handle_close(ht, keep), SYS_OK);

    cnprintf("    %-38s %s\n", "a client that gave up at its deadline",
      "no notice, a refused reply");

    expect("giving the channel back", handle_close(ht, hrecv), SYS_OK);
}

/*
 * A server thread that dies holding a message, per M17 stage five.
 *
 * M15 left this: the client waited until its own deadline, because the
 * only thing that released a client early was the death of the whole
 * server process. The reply that would release it can no longer come
 * from anywhere, and the deadline is the wrong length of time to find
 * that out in.
 *
 * The deadline is what tells the two answers apart, which is why it is
 * long. A client released by the death gets its answer in microseconds
 * and one waiting for its deadline gets it in half a second, so the
 * check reads the clock as well as the error: the error alone would
 * pass for a system that simply waited.
 */
static void
kill_server_stale_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct sys_op_send_args args;
    struct sys_op_connect_args cargs;
    sys_op_t send;
    struct thread *srv, *cli;
    sys_handle_t h_recv, to;
    uint64_t sent, took, deadline;

    cnputs("a kill: a server thread that dies holding a message\n");

    stale_ch = channel_create("stale", self);
    if (stale_ch == NULL) {
        panic("a stale reply: no channel");
    }
    expect("a channel for the server that will die",
      handle_insert(ht, stale_ch, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &h_recv), SYS_OK);

    memset(&cargs, 0, sizeof(cargs));
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a connection to it, for the client",
      run(SYS_OP_CHAN_CONNECT, h_recv, &cargs, sizeof(cargs)), SYS_OK);
    to = cargs.out_handle;

    /*
     * Above this thread, so it is already blocked in receive when the
     * send arrives and the send goes straight to the rendezvous. Below
     * it the message would queue instead and the client would be
     * SEND-blocked, which is a different state and not this one: M15
     * stage three paid for that mistake once.
     */
    stale_took = 0;
    srv = thread_create("stale-srv", PRIO_BOOT + 1, stale_server, NULL);
    sched_ready(srv);

    memset(&args, 0, sizeof(args));
    args.request.handle = SYS_HANDLE_INVALID;
    deadline = time_now() + time_from_ms(STALE_DEADLINE_MS);
    args.deadline = deadline;

    memset(&send, 0, sizeof(send));
    send.opcode = SYS_OP_IPC_SEND;
    send.target = to;
    send.flags = SYS_OP_DEADLINE;
    send.arg_ptr = (uintptr_t)&args;
    send.arg_len = sizeof(args);

    sent = time_now();
    expect("a send whose server thread dies holding it",
      (sys_err_t)run_batch(&send, 1), SYS_ERR_STALE);
    took = time_now() - sent;

    /*
     * That the server had it. Without this the check would pass for a
     * send that was refused before any rendezvous happened, which is
     * not a message anybody died holding.
     */
    if (stale_took == 0) {
        panic("a stale reply: the server never took the message");
    }

    /*
     * And it came back before its deadline rather than at it. This is
     * the assertion the stage exists for: the error says what happened
     * and the clock says the client did not sit through the wait to
     * find out.
     */
    if (time_now() >= deadline) {
        panic("a stale reply: the client waited out its deadline");
    }

    /*
     * And nothing is left inside a request on the channel. A server
     * that died holding a message is as finished with it as one whose
     * reply was refused, and the list chain_raise walks has to say so.
     */
    if (stale_ch->c_serving != NULL) {
        panic("a stale reply: the channel still has a server in a request");
    }

    if (thread_current()->t_donee != NULL) {
        panic("a stale reply: the client is still donating to a dead server");
    }

    cnprintf("    %-38s %s\n", "a server thread that never replied",
      "the client was told, not timed out");
    cnprintf("    %-38s %lu ticks, about %lu us, of a %u ms deadline\n",
      "how long the client waited", (unsigned long)took,
      (unsigned long)(took * 1000000ULL / time_hz()),
      (unsigned int)STALE_DEADLINE_MS);


    /*
     * And the same thing reached the other way, per this stage's
     * review: a server thread that is cancelled and then ends. The
     * cancel takes it off the channel's serving list on the way past,
     * so the first version of this recognised its client by a channel
     * pointer that was already NULL and released nobody. Cancellation
     * is how a kill reaches a blocked thread, which makes this the
     * shape M17 actually produces.
     *
     * The client is a thread of its own here, because this one has to
     * still be running in order to do the cancelling.
     */
    stale_took = 0;
    stale_client_err = SYS_ERR_INTERRUPTED;
    timeline_init(&stale_done, "stale-done");

    srv = thread_create("stale-wait", PRIO_BOOT + 2, stale_waiting_server,
      NULL);
    sched_ready(srv);

    cli = thread_create("stale-cli", PRIO_BOOT + 1, stale_client, NULL);
    sched_ready(cli);

    if (stale_took == 0) {
        panic("a stale reply: the second server never took the message");
    }
    if (srv->t_state != THREAD_BLOCKED) {
        panic("a stale reply: the server is not waiting, state %u",
          (unsigned int)srv->t_state);
    }

    expect("cancelling the server thread out of its wait",
      thread_cancel(srv), SYS_OK);

    expect("waiting for the client it left behind",
      timeline_wait(&stale_done, 1, time_now() + time_from_ms(JOIN_MS)),
      SYS_OK);
    expect("what the client was told", stale_client_err, SYS_ERR_STALE);

    if (stale_ch->c_serving != NULL) {
        panic("a stale reply: the channel still has a server in a request");
    }

    cnprintf("    %-38s %s\n", "a server thread cancelled, then ended",
      "its client was told as well");
    expect("giving the connection back", handle_close(ht, to), SYS_OK);
    expect("giving the channel back", handle_close(ht, h_recv), SYS_OK);
    stale_ch = NULL;

    chain_stale_checks();
}






/*
 * What the two regions of a program permit, per M14 stage three.
 *
 * A program's text is read and executed and never written; everything
 * else it has is read and written and never executed. Until this stage
 * one region held all of it with all three permissions, because the
 * loader could not see where text ended, and the comment on VM_PROGRAM
 * said as much.
 *
 * Both checks need a program that does the thing, and neither existed:
 * every program here reads addresses it is handed and jumps only where
 * the linker sent it. poke_addr writes to an address, which is
 * touch_addr's other half, and leap jumps into its own stack.
 *
 * The address is asserted as well as the fault, which matters more here
 * than usual. An unmapped page and a page that refuses a write are both
 * SYS_FAULT_ACCESS, so a check that asked only whether something
 * faulted would pass just as well against a loader that had mapped
 * nothing at all.
 */
static void
permission_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    const struct boot_member *m;
    struct sys_op_status_args sargs;
    char addr[19];
    const char *argv[2];
    sys_handle_t h;
    struct child c;
    void *obj;

    cnputs("permissions: what each half of a program may be used for\n");

    m = boot_lookup("poke_addr");
    if (m == NULL) {
        panic("permissions: no poke_addr in the archive");
    }
    hex_into(addr, (uintptr_t)m->bm_load);

    argv[0] = "poke_addr";
    argv[1] = addr;

    sched_defer();
    if (proc_spawn_op(self, "poke_addr", PRIO_BOOT - 1, NULL, 0,
      argv, 2, &h) != SYS_OK) {
        panic("permissions: could not start poke_addr");
    }
    if (handle_lookup(ht, h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
        panic("permissions: the spawn returned no process");
    }
    child_watch(&c, obj);
    sched_undefer();

    (void)child_join(&c, JOIN_MS);

    memset(&sargs, 0, sizeof(sargs));
    if (run(SYS_OP_PROC_STATUS, h, &sargs, sizeof(sargs)) != SYS_OK) {
        panic("permissions: no status for poke_addr");
    }

#if SYS_TIER == 2
    if (sargs.out_fault != SYS_FAULT_ACCESS) {
        panic("permissions: a program wrote its own text and lived, "
          "kind %u", (unsigned int)sargs.out_fault);
    }
    if (sargs.out_fault_addr != (uintptr_t)m->bm_load) {
        panic("permissions: reported 0x%lx, wrote 0x%lx",
          (unsigned long)sargs.out_fault_addr,
          (unsigned long)m->bm_load);
    }
    cnprintf("    %-38s %s at 0x%lx\n", "a program writing its own text",
      fault_name(sargs.out_fault),
      (unsigned long)sargs.out_fault_addr);
#else
    /*
     * Flat memory permits it, and says so rather than being left out.
     * Section 13.3 buys Tier 0 no MMU, and this is the price named:
     * the same program, the same store, and nothing to refuse it.
     */
    if (sargs.out_fault != SYS_FAULT_NONE) {
        panic("permissions: flat memory refused a write, kind %u",
          (unsigned int)sargs.out_fault);
    }
    cnprintf("    %-38s %s\n", "a program writing its own text",
      "permitted, because nothing here says no");
#endif

    expect("giving poke_addr's handle back", handle_close(ht, h), SYS_OK);

    sched_defer();
    if (proc_spawn_op(self, "leap", PRIO_BOOT - 1, NULL, 0,
      NULL, 0, &h) != SYS_OK) {
        panic("permissions: could not start leap");
    }
    if (handle_lookup(ht, h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
        panic("permissions: the spawn returned no process");
    }
    child_watch(&c, obj);
    sched_undefer();

    (void)child_join(&c, JOIN_MS);

    memset(&sargs, 0, sizeof(sargs));
    if (run(SYS_OP_PROC_STATUS, h, &sargs, sizeof(sargs)) != SYS_OK) {
        panic("permissions: no status for leap");
    }

#if SYS_TIER == 2
    if (sargs.out_fault != SYS_FAULT_ACCESS) {
        panic("permissions: a program ran from its stack and lived, "
          "kind %u", (unsigned int)sargs.out_fault);
    }

    /*
     * Inside the writable half of its own slot, which is where a stack
     * is. The exact address is the compiler's business and not this
     * check's, so what is asserted is the region: a fault somewhere
     * else would be a different failure wearing the same kind.
     */
    {
        uintptr_t base;
        size_t len;

        m = boot_lookup("leap");
        if (m == NULL) {
            panic("permissions: no leap in the archive");
        }
        base = (uintptr_t)m->bm_load;
        len = m->bm_mem;

        if (sargs.out_fault_addr < base + m->bm_rw ||
          sargs.out_fault_addr >= base + len) {
            panic("permissions: ran from 0x%lx, outside its writable "
              "memory", (unsigned long)sargs.out_fault_addr);
        }
    }
    cnprintf("    %-38s %s at 0x%lx\n", "a program running from its stack",
      fault_name(sargs.out_fault),
      (unsigned long)sargs.out_fault_addr);
#else
    /*
     * The fetch is permitted and the instruction is not, which is a
     * distinction worth keeping. Flat memory has nothing to refuse a
     * jump into a stack, so what ends this program is the zeroes it
     * jumped to failing to decode: SYS_FAULT_INSTRUCTION and not
     * SYS_FAULT_ACCESS. The two kinds are how a reader tells "the
     * machine let me run from my stack" from "the machine stopped me",
     * and only the second is a protection.
     */
    if (sargs.out_fault != SYS_FAULT_INSTRUCTION) {
        panic("permissions: flat memory answered a jump into a stack "
          "with kind %u, not an illegal instruction",
          (unsigned int)sargs.out_fault);
    }
    cnprintf("    %-38s %s\n", "a program running from its stack",
      "fetched, then refused as an instruction");
#endif

    expect("giving leap's handle back", handle_close(ht, h), SYS_OK);
}

/*
 * A grant over ordinary memory, given and then taken away.
 *
 * The console driver above holds a grant over a device, which is what
 * section 19.2 is about, and a device is an awkward thing to make an
 * argument with: a register cannot be read twice without meaning
 * something, and which register is safe differs by board. A page of RAM
 * is the same object with none of that. The kernel allocates one,
 * describes it, and hands it over.
 *
 * What this settles is the part the driver cannot show. That a grant is
 * authority a process did not have and now does. That the handle is the
 * authority, so closing it takes the memory back. And that both tiers
 * agree on the first and differ on the second, which is where they
 * always differ.
 */
static void
grant_checks(void)
{
    struct proc *self = proc_current();
    struct sys_op_status_args sargs;
    struct handle_table *ht = &self->p_handles;
    sys_ns_spec_t ns[1];
    struct grant *g;
    sys_handle_t h, hg;
    struct child c;
    uintptr_t pa;
    void *obj;
    sys_err_t err;
    int32_t code;

    pa = page_alloc();
    if (pa == 0) {
        panic("grant: no page to give away");
    }

    g = grant_create("a-page", pa, PAGE_SIZE, false);
    if (g == NULL) {
        panic("grant: no grant to describe it with");
    }
    err = handle_insert(ht, g, SYS_OBJ_GRANT,
      SYS_RIGHT_MAP | SYS_RIGHT_READ | SYS_RIGHT_WRITE |
      SYS_RIGHT_TRANSFER, &hg);
    expect("a grant over a page of memory", err, SYS_OK);

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_DEVICE;
    ns[0].handle = hg;

    sched_defer();
    if (proc_spawn_op(self, "granted", PRIO_BOOT - 1, ns, 1, NULL, 0,
      &h) != SYS_OK) {
        panic("grant: could not start the program");
    }
    if (handle_lookup(ht, h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
        panic("grant: the spawn returned no process");
    }
    child_watch(&c, obj);
    sched_undefer();

    code = child_join(&c, JOIN_MS);

    /*
     * The word it wrote, read through the physical address, which is
     * the kernel's own view of the same page. Without this the check
     * would pass on a program that never touched anything.
     */
    if (*(volatile uint32_t *)pa != 0x5eed1234u) {
        panic("grant: the page holds 0x%x, not what it was given for",
          (unsigned int)*(volatile uint32_t *)pa);
    }

    memset(&sargs, 0, sizeof(sargs));
    if (run(SYS_OP_PROC_STATUS, h, &sargs, sizeof(sargs)) != SYS_OK) {
        panic("grant: no status for it");
    }

#if SYS_TIER == 2
    if (sargs.out_fault != SYS_FAULT_ACCESS ||
      sargs.out_fault_addr != pa) {
        panic("grant: closing the handle left the page reachable, "
          "kind %u code %d", (unsigned int)sargs.out_fault, code);
    }
    cnprintf("    %-38s %s at 0x%lx\n", "and the page after closing it",
      fault_name(sargs.out_fault), (unsigned long)sargs.out_fault_addr);
#else
    if (sargs.out_fault != SYS_FAULT_NONE || code != 0) {
        panic("grant: it died of %u reading its own page, at this tier",
          (unsigned int)sargs.out_fault);
    }
    cnprintf("    %-38s %s\n", "and the page after closing it",
      "still there, because nothing says no");
#endif

    handle_close(ht, h);
}

/*
 * A page fault as a message, and a program whose text arrives one page
 * at a time.
 *
 * This thread is the root pager of section 15.4: it has no pager of its
 * own and is memory-locked, which is what a terminator is. It knows the
 * boot archive because the kernel is the loader, so what it supplies is
 * the image bytes for the page that was asked about.
 *
 * The program adds up its own image and returns the sum. That is what
 * makes this a check rather than an observation: the same sum computed
 * from the archive here has to match, so a pager that answered with
 * blank pages would fail even though the program ran to the end.
 */

/*
 * A thread between the pager and the program it serves, so that the
 * priority the pager runs at is a fact about scheduling rather than a
 * field somebody set.
 *
 * Section 15.3's claim is that a fault is serviced at the faulting
 * thread's priority, because the thread donates to its pager exactly as
 * a client donates to a server. The pager's own priority is below this
 * thread's. If the donation did not happen, this thread would be the
 * highest runnable thing whenever the program blocked on a fault, and
 * the pager would not run until it finished.
 */
static struct thread *paging_spinner_t;
static uint64_t paging_spin_turns;

static void
paging_spinner(void *arg)
{
    uint64_t until = time_now() + time_from_ms(100);

    (void)arg;

    while (time_now() < until) {
        paging_spin_turns++;
    }

    thread_exit(0);
}

static struct channel *ch_pager;
static sys_handle_t h_pager_recv;
static uint32_t pager_pages;

/*
 * The end of the highest page the pager has been asked for, as an offset
 * into the image. Section 15.1 has it supply read-only pages only, so
 * this must stay at or below where the archive says writable memory
 * begins.
 */
static uint32_t pager_high;

/*
 * What the ledger at the end of the boot needs from before any of the
 * checks ran: what the port had already spent, and how many tables the
 * kernel's own address space was.
 */
static uint32_t frames_at_start;
#if SYS_TIER == 2
static uint32_t kernel_tables_at_start;
#endif

/* One reading of the channel pool, per M20 stage one. */
static struct channel_scan channel_report;

static uint8_t pager_prio_seen;
static bool pager_served_over_spinner;

static void
root_pager(void *arg)
{
    struct sys_op_recv_args rx;
    struct sys_op_reply_args tx;

    (void)arg;

    for (;;) {
        const struct sys_fault_msg *fm;
        const struct boot_member *m;
        struct thread *client;
        struct proc *cp;
        struct grant *g;
        uintptr_t pa, off;
        sys_handle_t h;
        size_t n;

        memset(&rx, 0, sizeof(rx));
        if (run(SYS_OP_IPC_RECV, h_pager_recv, &rx, sizeof(rx)) != SYS_OK) {
            panic("pager: the receive failed");
        }

        fm = (const struct sys_fault_msg *)rx.message.inline_data;
        client = thread_by_id(rx.peer);
        cp = client != NULL ? client->t_proc : NULL;
        m = cp != NULL ? boot_lookup(cp->p_name) : NULL;
        if (m == NULL) {
            panic("pager: a fault from something with no image");
        }

        off = fm->addr - (uintptr_t)m->bm_load;
        n = m->bm_size > off ? m->bm_size - off : 0;
        if (n > PAGE_SIZE) {
            n = PAGE_SIZE;
        }

        /*
         * A frame with the image bytes in it. Zeroed first, because the
         * last page of an image is short and what follows it in the page
         * is not part of the program.
         */
        pa = page_alloc();
        if (pa == 0) {
            panic("pager: no frame for %s", cp->p_name);
        }
        if (n > 0) {
            memcpy((void *)pa, boot_payload(m) + off, n);
        }

        g = grant_create("a-text-page", pa, PAGE_SIZE, false);
        if (g == NULL) {
            panic("pager: no grant for %s", cp->p_name);
        }
        if (handle_insert(&proc_current()->p_handles, g, SYS_OBJ_GRANT,
          SYS_RIGHT_READ | SYS_RIGHT_EXEC | SYS_RIGHT_TRANSFER,
          &h) != SYS_OK) {
            panic("pager: no handle for the page");
        }

        pager_pages++;

        /*
         * And where it was asked for, per M14 stage five's review. A
         * count of pages says how many arrived and not which, so a
         * pager that supplied a data page while some text page went
         * untouched would give the same number. The offset of the
         * highest one is what the check compares against the boundary
         * the archive records.
         */
        if (off + PAGE_SIZE > pager_high) {
            pager_high = off + PAGE_SIZE;
        }

        pager_prio_seen = thread_current()->t_prio;
        if (paging_spinner_t != NULL &&
          paging_spinner_t->t_state == THREAD_READY) {
            pager_served_over_spinner = true;
        }

        memset(&tx, 0, sizeof(tx));
        tx.peer = rx.peer;
        tx.reply.handle = h;
        if (run(SYS_OP_IPC_REPLY, 0, &tx, sizeof(tx)) != SYS_OK) {
            panic("pager: the reply failed");
        }
    }
}

/*
 * The program, started with a pager, and what it cost.
 */
static void
paging_checks(void)
{
    struct proc *self = proc_current();
    struct sys_op_connect_args cargs;
    const struct boot_member *m;
    sys_ns_spec_t ns[1];
    struct thread *t;
    struct child c;
    const char *argv[3];
    char abase[19], alen[19];
    uint32_t want = 0;
    uint32_t i;
    sys_handle_t h;
    int32_t code;
    sys_err_t err;

    m = boot_lookup("paged");
    if (m == NULL) {
        panic("paging: no such image");
    }

    ch_pager = channel_create("pager", self);
    if (ch_pager == NULL) {
        panic("paging: no channel");
    }
    err = handle_insert(&self->p_handles, ch_pager, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV, &h_pager_recv);
    expect("a channel for the root pager", err, SYS_OK);

    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = 0;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a connection to it, for the program",
      run(SYS_OP_CHAN_CONNECT, h_pager_recv, &cargs, sizeof(cargs)),
      SYS_OK);

    /*
     * The pager runs below the program, deliberately. Section 15.3's
     * claim is that a fault is serviced at the faulting thread's
     * priority because the thread donates to its pager, and a pager that
     * already outranked its client would prove nothing.
     */
    t = thread_create("pager", 5, root_pager, NULL);
    sched_ready(t);

    hex_into(abase, (uintptr_t)m->bm_load);
    hex_into(alen, m->bm_size);
    argv[0] = "paged";
    argv[1] = abase;
    argv[2] = alen;

    for (i = 0; i < m->bm_size; i++) {
        want += boot_payload(m)[i];
    }
    want &= 0x7fffffffu;

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_PAGER;
    ns[0].handle = cargs.out_handle;

    sched_defer();
    child_watch(&c, proc_spawn_image("paged", 20, ns, 1, argv, 3));

    /*
     * Readied inside the deferral, after the program, so that
     * neither runs until both exist. Started before it, this
     * outranks the boot thread and runs to its end before the
     * program is spawned at all, which is a spinner that proves
     * nothing.
     */
    paging_spinner_t = thread_create("middle", 10, paging_spinner, NULL);
    sched_ready(paging_spinner_t);
    sched_undefer();

    code = child_join(&c, JOIN_MS);

    if ((uint32_t)code != want) {
        panic("paging: it added its image to 0x%x, not 0x%x, after "
          "%u pages, %s fault at 0x%lx pc 0x%lx",
          (unsigned int)code, (unsigned int)want,
          (unsigned int)pager_pages, fault_name(c.c_proc->p_fault),
          (unsigned long)c.c_proc->p_fault_addr,
          (unsigned long)c.c_proc->p_fault_pc);
    }

#if SYS_TIER == 2
    /*
     * And that what arrived was text and only text, per M14 stage five.
     * The program carries a word of initialised data above the boundary
     * the archive records, and section 15.1 has a page supplied to a
     * fault mapped read-only, so a data page left to the pager would
     * arrive read-only and the program would fault writing it. It
     * returns -3 or -4 if its word is wrong or will not take a write,
     * which the comparison above would catch, and this says the same
     * thing from the outside: the pager was asked for the pages below
     * the boundary and for none above it.
     */
    if (pager_pages != m->bm_rw / PAGE_SIZE) {
        panic("paging: the pager supplied %u pages for %u of text",
          (unsigned int)pager_pages,
          (unsigned int)(m->bm_rw / PAGE_SIZE));
    }

    /*
     * And that they were the low ones. The count alone would match a
     * pager that had supplied a data page while a text page went
     * untouched, which is the failure this stage removed rather than
     * one it can assume gone.
     */
    if (pager_high > m->bm_rw) {
        panic("paging: the pager was asked as high as 0x%x, above the "
          "0x%x where writable memory begins",
          (unsigned int)pager_high, (unsigned int)m->bm_rw);
    }
    if (m->bm_size <= m->bm_rw) {
        panic("paging: the program has no initialised data to check");
    }
#endif
#if SYS_TIER == 2
    if (pager_pages == 0) {
        panic("paging: it ran without a page arriving");
    }
    /*
     * The kernel counted a fault for each one it sent and the pager
     * counted a page for each one it answered. They are two counters
     * kept by two pieces of code that do not read each other, and a
     * disagreement means a fault that was answered without being
     * asked or asked without being answered.
     */
    if (c.c_proc->p_faults != pager_pages) {
        panic("paging: %u faults sent and %u pages supplied",
          (unsigned int)c.c_proc->p_faults,
          (unsigned int)pager_pages);
    }

    if (pager_prio_seen != 20) {
        panic("paging: the pager served at %u, not at the priority of "
          "the thread that faulted", (unsigned int)pager_prio_seen);
    }
    if (!pager_served_over_spinner) {
        panic("paging: nothing lower was runnable, so the priority "
          "proves nothing");
    }
    cnprintf("    %-38s %u\n", "pages its text arrived in",
      (unsigned int)pager_pages);
    cnprintf("    %-38s %u, base 5\n", "serviced at the faulting priority",
      (unsigned int)pager_prio_seen);
#else
    /*
     * Section 15.3 is Tier 2 only, and this is what that means from the
     * outside: the same program, given the same pager, runs without ever
     * asking it for anything, because a flat address space has nothing
     * absent to fault on. It still adds up to the same number.
     */
    if (pager_pages != 0) {
        panic("paging: %u pages arrived on a tier with no paging",
          (unsigned int)pager_pages);
    }
    cnprintf("    %-38s %s\n", "pages its text arrived in",
      "none, it was already there");
#endif
    cnprintf("    %-38s 0x%x\n", "and the image adds up to",
      (unsigned int)want);

    /*
     * And a pager that does not terminate, per section 15.4 rule 1.
     *
     * The channel is owned by an ordinary program, which has no pager of
     * its own and is not memory-locked, so a chain ending there ends at
     * something that can fault. That is a deadlock with the timing not
     * yet arranged: the classic shape is a storage driver that faults on
     * its own text and has to reach the device it is driving.
     *
     * Refused before anything moves, which is why the caller is told
     * rather than the child left half-built.
     */
    {
        struct sys_op_connect_args bad;
        struct child bc;
        struct channel *bch;
        sys_handle_t bh, bproc;
        void *obj;

        sched_defer();
        if (proc_spawn_op(self, "status", PRIO_BOOT - 1, NULL, 0, NULL, 0,
          &bproc) != SYS_OK) {
            panic("paging: could not start an ordinary program");
        }
        if (handle_lookup(&self->p_handles, bproc, SYS_OBJ_PROCESS, 0,
          &obj) != SYS_OK) {
            panic("paging: the spawn returned no process");
        }
        child_watch(&bc, obj);
        sched_undefer();
        (void)child_join(&bc, JOIN_MS);

        bch = channel_create("not-a-pager", obj);
        if (bch == NULL) {
            panic("paging: no channel");
        }
        if (handle_insert(&self->p_handles, bch, SYS_OBJ_CHANNEL,
          SYS_RIGHT_RECV, &bh) != SYS_OK) {
            panic("paging: no handle for it");
        }

        /*
         * And one in the program's own table, because a channel
         * belongs to whoever may receive on it and the point of this
         * one is that the pager it names is an ordinary program with
         * no pager of its own. The kernel keeps its handle in order
         * to mint the connection below; the last insert is what says
         * whose channel it is.
         */
        {
            sys_handle_t owned;

            if (handle_insert(&((struct proc *)obj)->p_handles, bch,
              SYS_OBJ_CHANNEL, SYS_RIGHT_RECV, &owned) != SYS_OK) {
                panic("paging: the program cannot hold its own channel");
            }
        }
        memset(&bad, 0, sizeof(bad));
        bad.cookie = 0;
        bad.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
        expect("a connection to a pager that can fault",
          run(SYS_OP_CHAN_CONNECT, bh, &bad, sizeof(bad)), SYS_OK);

        ns[0].id = SYS_NS_PAGER;
        ns[0].handle = bad.out_handle;
        expect("and a program given it, refused",
          proc_spawn_op(self, "spin", PRIO_BOOT - 1, ns, 1, NULL, 0,
            &bproc), SYS_ERR_DEADLOCK);

        handle_close(&self->p_handles, bad.out_handle);
        handle_close(&self->p_handles, bh);
    }

    (void)h;
}

/*
 * What a process was given, with names on it.
 *
 * Section 20.4 has a namespace arrive at launch as an array of named
 * directory handles. Until now the array carried numbers, which is what a
 * role is; a name is what a mount point is, and a parent that cannot say
 * where to mount a handle cannot compose a namespace at all.
 *
 * Three things are checked here and the last is the one with teeth. That
 * a child sees the names its parent chose, that a child given nothing
 * sees nothing, and that a name the caller does not own is refused rather
 * than read, which is section 23.2 applied to the array this milestone
 * added rather than to the arrays the last one did.
 */
static void
namespace_checks(sys_handle_t root, sys_handle_t console)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    sys_ns_spec_t ns[3];
    struct grant *g;
    uintptr_t pa;
    struct child c;
    sys_handle_t h;
    void *obj;
    int32_t code;

    /*
     * Two names for one directory, and a grant with no name at all.
     *
     * The two names are the composition this milestone is about. The
     * grant is there because a namespace is not all one kind of thing: a
     * driver holds one by role, and a composer that hands on connection
     * rights cannot hand it on, which is section 7.2 refusing to clamp a
     * duplication rather than a defect. The program says which happened.
     *
     * Neither the grant nor the console is named, and that is the rule
     * rather than an omission: section 20.4's array is of named
     * *directory* handles, and a thing that answers no path below it has
     * no business being a mount point.
     */
    pa = page_alloc();
    if (pa == 0) {
        panic("namespace: no page to describe");
    }
    g = grant_create("a-namespace-page", pa, PAGE_SIZE, false);
    if (g == NULL) {
        panic("namespace: no grant to hand over");
    }

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_ROOT;
    ns[0].handle = share(root);
    strncpy_ns(ns[0].name, "/");
    ns[1].id = (sys_ns_id_t)(SYS_NS_PRIVATE + 0);
    ns[1].handle = share(root);
    strncpy_ns(ns[1].name, "/files");
    if (handle_insert(ht, g, SYS_OBJ_GRANT,
      SYS_RIGHT_MAP | SYS_RIGHT_READ | SYS_RIGHT_DUPLICATE |
      SYS_RIGHT_TRANSFER, &ns[2].handle) != SYS_OK) {
        panic("namespace: no handle for the grant");
    }
    ns[2].id = SYS_NS_DEVICE;

    sched_defer();
    child_watch(&c, proc_spawn_image("namespace", 20, ns, 3, NULL, 0));
    sched_undefer();

    code = child_join(&c, JOIN_MS);
    if (code != 2) {
        panic("namespace: it found %d named entries, not 2", code);
    }
    cnprintf("    %-38s %d\n", "a child sees the names it was given",
      code);

    /*
     * And a child given nothing. Section 20.3: there is no ambient root,
     * so a process handed no entries can name nothing at all, and the
     * check is that it says so rather than finding something anyway.
     */
    sched_defer();
    child_watch(&c, proc_spawn_image("namespace", 20, NULL, 0, NULL, 0));
    sched_undefer();

    code = child_join(&c, JOIN_MS);
    if (code != 0) {
        panic("namespace: a child given nothing found %d entries", code);
    }
    cnprintf("    %-38s %s\n", "and a child given none finds",
      "nothing at all");

    /*
     * A name that does not end inside its slot. There is no pointer to
     * follow here any more, which is the point: the name is inside the
     * array the operation already checked, so what is left to get wrong
     * is a name with no terminator, and a mount point cut in half
     * resolves to something other than what the parent meant.
     */
    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_ROOT;
    ns[0].handle = share(root);
    memset(ns[0].name, 'x', sizeof(ns[0].name));
    expect("a mount name with no end to it",
      proc_spawn_op(self, "status", PRIO_BOOT - 1, ns, 1, NULL, 0, &h),
      SYS_ERR_INVALID_ARG);
    handle_close(ht, ns[0].handle);

    /*
     * And the resolver, against a namespace this thread chose so that
     * the answers are properties of the table rather than of the
     * library. A root at `/`, something else at `/dev`, and a working
     * directory at `/`, which is what `resolve` is written against.
     *
     * It is a unit test in a program because that is where the library
     * runs. Nothing here sends a message: section 20.2's claim is that
     * resolution costs no syscall, and a check that needed a server
     * would not be checking that.
     */
    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_ROOT;
    ns[0].handle = share(root);
    strncpy_ns(ns[0].name, "/");
    ns[1].id = SYS_NS_CONSOLE;
    ns[1].handle = share(console);
    strncpy_ns(ns[1].name, "/dev");
    ns[2].id = SYS_NS_CWD;
    ns[2].handle = share(root);
    strncpy_ns(ns[2].name, "/");

    sched_defer();
    child_watch(&c, proc_spawn_image("resolve", 20, ns, 3, NULL, 0));
    sched_undefer();

    code = child_join(&c, JOIN_MS);
    if (code != 0) {
        panic("namespace: the resolver failed case %d", code);
    }
    cnprintf("    %-38s %s\n", "paths resolve where they should",
      "every case");

    (void)obj;
}


/*
 * A thread that exists in order to stop, so that its slot can be
 * watched going back.
 */
static void
briefly(void *arg)
{
    (void)arg;
    thread_exit(0);
}

/*
 * A server that receives and never replies, so that something can be
 * caught between the rendezvous and the reply.
 */
static struct channel *ch_wedge;
static sys_handle_t h_wedge_recv;
static struct thread *wedge_server;

static void
never_replies(void *arg)
{
    struct sys_msg m;
    sys_thread_id_t peer;
    uint64_t cookie;

    (void)arg;

    for (;;) {
        if (channel_recv(ch_wedge, &m, &peer, &cookie) != SYS_OK) {
            return;
        }
        /*
         * And that is all. The client is now reply-blocked with its
         * buffer here, which is the state section 10.5 calls the
         * interesting case.
         */
    }
}



/*
 * Killing a server, and putting one back, per sections 10.5 and 24.3.
 *
 * The kernel holds no restart policy and section 18.3 says it must not,
 * so what is checked here is in two pieces. The mechanism is a kill and
 * a completion timeline. The policy is a program, and one that gives up
 * after a bound rather than relaunching for ever, because a child that
 * dies immediately every time is a configuration mistake and hiding one
 * is worse than stopping.
 */
static void
kill_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct sys_op_connect_args cargs;
    struct sys_op_kill_args kargs;
    sys_ns_spec_t ns[2];
    const char *argv[2];
    struct channel *ch;
    struct child sc, cc;
    sys_handle_t recv, h;
    int32_t code;
    void *obj;

    cnputs("killing: a server ended, and another put in its place\n");

    ch = channel_create("echoes", self);
    if (ch == NULL) {
        panic("kill: no channel");
    }
    expect("a channel for the server",
      handle_insert(ht, ch, SYS_OBJ_CHANNEL,
        SYS_RIGHT_RECV | SYS_RIGHT_DUPLICATE | SYS_RIGHT_TRANSFER,
        &recv), SYS_OK);

    /*
     * Its receive end goes to the server and comes back when the server
     * dies, because a handle moves. So the kernel keeps a duplicate to
     * hand to the replacement, which is what root does when it restarts
     * a server: the channel outlives the process serving it and the
     * clients never learn there was a second one.
     */
    {
        struct timeline *never = timeline_create("deaf");
        sys_handle_t give, hn;

        if (never == NULL) {
            panic("kill: no timeline");
        }
        if (handle_insert(ht, never, SYS_OBJ_TIMELINE,
          SYS_RIGHT_WAIT | SYS_RIGHT_TRANSFER, &hn) != SYS_OK) {
            panic("kill: no handle for it");
        }
        if (handle_dup(ht, recv, SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER,
          &give) != SYS_OK) {
            panic("kill: cannot hand the channel on");
        }
        memset(ns, 0, sizeof(ns));
        ns[0].id = SYS_NS_SERVICE;
        ns[0].handle = give;
        ns[1].id = SYS_NS_PRIVATE + 0;
        ns[1].handle = hn;

        argv[0] = "echoes";
        argv[1] = "holds";

        sched_defer();
        if (proc_spawn_op(self, "echoes", PRIO_BOOT, ns, 2, argv, 2,
          &h) != SYS_OK) {
            panic("kill: could not start the server");
        }
        if (handle_lookup(ht, h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
            panic("kill: the spawn returned no process");
        }
        child_watch(&sc, obj);
        sched_undefer();
    }

    /* A client, to have something blocked on it when it goes. */
    memset(&cargs, 0, sizeof(cargs));
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a connection to it", run(SYS_OP_CHAN_CONNECT, recv, &cargs,
      sizeof(cargs)), SYS_OK);

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_PRIVATE + 1;
    ns[0].handle = cargs.out_handle;
    argv[0] = "orphan";
    argv[1] = "dead";

    sched_defer();
    child_watch(&cc, proc_spawn_image("orphan", 20, ns, 1, argv, 2));
    sched_undefer();

    /*
     * Until the client is actually being served, which is the state
     * this is about: its buffer is in the server and gone from its own
     * memory, so ending the server has to give the pages back as well
     * as release the thread.
     *
     * Waited for rather than assumed. The server runs at this thread's
     * own priority, so readying it does not run it, and the first
     * version of this killed a server that had never received: the
     * client was queued with its buffer still its own and the case it
     * was written for never happened.
     */
    {
        struct thread *ct = proc_threads(cc.c_proc);
        uint32_t spin;

        for (spin = 0; spin < 64 && ct->t_wait != THREAD_WAIT_REPLY;
          spin++) {
            sched_yield();
        }
        if (ct->t_wait != THREAD_WAIT_REPLY) {
            panic("kill: the client never reached the server");
        }
    }

    /*
     * The client is being served by the server. Ending the server
     * releases it with an error rather than leaving it there, which is
     * section 12.5 reached by a kill rather than by a cancel.
     */
    memset(&kargs, 0, sizeof(kargs));
    kargs.code = 9;
    expect("ending the server", run(SYS_OP_PROC_KILL, h, &kargs,
      sizeof(kargs)), SYS_OK);

    code = child_join(&cc, JOIN_MS);
    if (code != 0) {
        panic("kill: the client reported %d", code);
    }
    code = child_join(&sc, JOIN_MS);
    if (code != 9) {
        panic("kill: the server exited %d, not the code it was given",
          code);
    }
    if (handle_close(ht, h) != SYS_OK) {
        panic("kill: the spawn handle was already gone");
    }
    cnprintf("    %-38s %s\n", "its client was released", "OWNER_DEAD");

    /*
     * And another in its place, on the same channel, answering a client
     * that cannot tell there was ever a first one.
     */
    {
        sys_handle_t give;

        if (handle_dup(ht, recv, SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER,
          &give) != SYS_OK) {
            panic("kill: cannot hand the channel to the replacement");
        }
        memset(ns, 0, sizeof(ns));
        ns[0].id = SYS_NS_SERVICE;
        ns[0].handle = give;

        sched_defer();
        if (proc_spawn_op(self, "echoes", PRIO_BOOT, ns, 1, NULL, 0,
          &h) != SYS_OK) {
            panic("kill: could not start the replacement");
        }
        if (handle_lookup(ht, h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
            panic("kill: the replacement returned no process");
        }
        child_watch(&sc, obj);
        sched_undefer();
    }

    memset(&cargs, 0, sizeof(cargs));
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a connection to the replacement",
      run(SYS_OP_CHAN_CONNECT, recv, &cargs, sizeof(cargs)), SYS_OK);

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_PRIVATE + 1;
    ns[0].handle = cargs.out_handle;
    argv[1] = "ok";

    sched_defer();
    child_watch(&cc, proc_spawn_image("orphan", 20, ns, 1, argv, 2));
    sched_undefer();

    code = child_join(&cc, JOIN_MS);
    if (code != 0) {
        panic("kill: the second client reported %d", code);
    }
    cnprintf("    %-38s %s\n", "and the next client was answered", "OK");

    /* Tidy: the replacement is not needed after this. */
    memset(&kargs, 0, sizeof(kargs));
    kargs.code = 0;
    expect("ending the replacement", run(SYS_OP_PROC_KILL, h, &kargs,
      sizeof(kargs)), SYS_OK);
    (void)child_join(&sc, JOIN_MS);
    (void)handle_close(ht, h);
    (void)handle_close(ht, recv);

    /*
     * And a supervisor that runs out of patience. The child it is given
     * exits at once every time, so the only thing that stops the
     * supervisor is its own bound, and the status says how many starts
     * it made rather than leaving that to be inferred from silence.
     */
    {
        const char *sargv[3];

        sargv[0] = "supervise";
        sargv[1] = "status";
        sargv[2] = "3";

        sched_defer();
        child_watch(&sc,
          proc_spawn_image("supervise", 20, NULL, 0, sargv, 3));
        sched_undefer();
    }

    code = child_join(&sc, JOIN_MS);
    if (code != 3) {
        panic("kill: the supervisor made %d starts, not three", code);
    }
    cnprintf("    %-38s %d\n", "a child that always dies, given up on",
      code);
}
/*
 * The two sides of a rendezvous fail differently, per section 12.5,
 * because they are waiting on different things.
 *
 * A server is at a receive point, so it can be told: it collects a
 * pulse and goes on serving. A client is not at one and has nothing to
 * collect with, so it is failed instead. Neither direction depends on a
 * deadline, which is what makes this usable during a cohort restart
 * where a supervisor is destroying processes with clients blocked on
 * them.
 */
static struct channel *dying_ch;
static sys_handle_t dying_recv;
static sys_thread_id_t dying_client;
static uint32_t dying_pulse_value;
static bool dying_saw_pulse;
static sys_err_t dying_reply_said;

/*
 * A server that serves one request, is left holding it when its client
 * goes, and finds out by receiving.
 */
static void
attentive(void *arg)
{
    struct sys_msg got, reply;
    sys_thread_id_t peer;
    uint64_t cookie;

    (void)arg;

    if (channel_recv(dying_ch, &got, &peer, &cookie) != SYS_OK) {
        panic("death: the server could not receive");
    }
    dying_client = peer;

    /*
     * And now it waits, holding an unanswered request. Its client is
     * about to be cancelled, and the next thing this receive returns
     * should be the notice rather than another request.
     */
    if (channel_recv(dying_ch, &got, &peer, &cookie) != SYS_OK) {
        panic("death: the server could not receive again");
    }
    if ((got.flags & SYS_MSG_IS_PULSE) != 0) {
        const sys_pulse_t *p = (const sys_pulse_t *)got.inline_data;

        if (p->code == SYS_PULSE_DEAD_OWNER) {
            dying_saw_pulse = true;
            dying_pulse_value = p->value;
        }
    }

    /*
     * A server that replies anyway is told the client has moved on,
     * per section 10.5 step 3. A server that ignored this would be one
     * that never learned, which is what the pulse is for.
     */
    memset(&reply, 0, sizeof(reply));
    reply.handle = SYS_HANDLE_INVALID;
    dying_reply_said = channel_reply(dying_client, &reply);

    thread_exit(0);
}

static void
death_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct sys_op_connect_args cargs;
    struct child c;
    struct thread *t;
    int32_t code;

    cnputs("death: the party that can be told, and the party that cannot\n");

    /*
     * A client cancelled while a server holds its request. The server
     * is a thread of this process, so nothing here is dying; what dies
     * is the client, and the server has to find out.
     */
    dying_ch = channel_create("attentive", self);
    if (dying_ch == NULL) {
        panic("death: no channel");
    }
    expect("a receive handle for the attentive server",
      handle_insert(ht, dying_ch, SYS_OBJ_CHANNEL, SYS_RIGHT_RECV,
        &dying_recv), SYS_OK);
    sched_ready(thread_create("attentive", 5, attentive, NULL));

    {
        sys_ns_spec_t ns[4];
        const char *argv[2];

        memset(&cargs, 0, sizeof(cargs));
        cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
        expect("a connection to it",
          run(SYS_OP_CHAN_CONNECT, dying_recv, &cargs, sizeof(cargs)),
          SYS_OK);

        memset(ns, 0, sizeof(ns));
        ns[0].id = SYS_NS_PRIVATE + 2;
        ns[0].handle = cargs.out_handle;

        argv[0] = "wedged";
        argv[1] = "reply";

        sched_defer();
        child_watch(&c, proc_spawn_image("wedged", 20, ns, 1, argv, 2));
        sched_undefer();

        t = proc_threads(c.c_proc);
        while (t->t_state != THREAD_BLOCKED) {
            sched_yield();
        }
        if (thread_cancel(t) != SYS_OK) {
            panic("death: the client would not be released");
        }

        code = child_join(&c, JOIN_MS);
        if (code != 0) {
            panic("death: the cancelled client reported %d", code);
        }
    }

    /* The server had to run to collect it and to try its reply. */
    {
        uint32_t spin;

        for (spin = 0; spin < 64 && !dying_saw_pulse; spin++) {
            sched_yield();
        }
    }

    if (!dying_saw_pulse) {
        panic("death: the server was never told its client had gone");
    }
    if (dying_pulse_value != SYS_ID_TAG32(dying_client)) {
        panic("death: the notice named 0x%lx, the client was 0x%lx",
          (unsigned long)dying_pulse_value,
          (unsigned long)SYS_ID_TAG32(dying_client));
    }
    expect("its reply afterwards", dying_reply_said, SYS_ERR_STALE);
    cnprintf("    %-38s 0x%lx\n", "the server was told, and by name",
      (unsigned long)dying_pulse_value);

    /*
     * And the other direction: a client queued on a channel whose
     * owning process goes. It is not at a receive point, so there is
     * nothing it could collect and it is failed instead.
     *
     * The owner is a program, because ownership is a process and every
     * channel this system has otherwise belongs to the kernel, which
     * does not exit. It is blocked on a timeline nothing will signal
     * and leaves when it is cancelled.
     */
    {
        sys_ns_spec_t ns[2];
        const char *argv[2];
        struct child oc, cc;
        struct channel *owned;
        sys_handle_t owned_recv;
        struct thread *ot;

        memset(&cargs, 0, sizeof(cargs));
        argv[0] = "wedged";
        argv[1] = "fence";

        {
            struct timeline *tl = timeline_create("never-again");
            sys_handle_t hf;

            if (tl == NULL) {
                panic("death: no timeline");
            }
            expect("a timeline for the owner to wait on",
              handle_insert(ht, tl, SYS_OBJ_TIMELINE,
                SYS_RIGHT_WAIT | SYS_RIGHT_TRANSFER, &hf), SYS_OK);
            memset(ns, 0, sizeof(ns));
            ns[0].id = SYS_NS_PRIVATE + 0;
            ns[0].handle = hf;
        }

        sched_defer();
        child_watch(&oc, proc_spawn_image("wedged", 20, ns, 1, argv, 2));
        sched_undefer();

        /* Its channel, now that there is a process to own it. */
        owned = channel_create("abandoned", oc.c_proc);
        if (owned == NULL) {
            panic("death: no channel for the owner");
        }
        expect("a channel belonging to that program",
          handle_insert(ht, owned, SYS_OBJ_CHANNEL, SYS_RIGHT_RECV,
            &owned_recv), SYS_OK);

        /*
         * And a handle in its own table, which is what makes it the
         * owner: a channel belongs to whoever may receive on it. The
         * kernel keeps one too, to mint the connection below.
         */
        {
            sys_handle_t its;

            if (handle_insert(&oc.c_proc->p_handles, owned,
              SYS_OBJ_CHANNEL, SYS_RIGHT_RECV, &its) != SYS_OK) {
                panic("death: it cannot hold its own channel");
            }
        }

        memset(&cargs, 0, sizeof(cargs));
        cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
        expect("a connection to it, for somebody else",
          run(SYS_OP_CHAN_CONNECT, owned_recv, &cargs, sizeof(cargs)),
          SYS_OK);

        memset(ns, 0, sizeof(ns));
        ns[0].id = SYS_NS_PRIVATE + 1;
        ns[0].handle = cargs.out_handle;

        sched_defer();
        child_watch(&cc, proc_spawn_image("orphan", 20, ns, 1, NULL, 0));
        sched_undefer();

        /* The client is queued on it, and now the owner leaves. */
        ot = proc_threads(oc.c_proc);
        while (ot->t_state != THREAD_BLOCKED) {
            sched_yield();
        }
        if (thread_cancel(ot) != SYS_OK) {
            panic("death: the owner would not be released");
        }

        (void)child_join(&oc, JOIN_MS);
        code = child_join(&cc, JOIN_MS);
        if (code != 0) {
            panic("death: the abandoned client reported %d", code);
        }
        cnprintf("    %-38s %s\n", "its client was failed, not told",
          "OWNER_DEAD");

        (void)handle_close(ht, owned_recv);
    }
}
/*
 * Pulses, and the reservation that is the whole reason they are
 * specified rather than assumed, per section 13.1.
 *
 * The queue is bounded because the pools are, so a policy is needed
 * when it fills, and the policy has to distinguish two kinds. A user
 * pulse that is lost costs its receiver a notification it can ask about
 * again. A death notice that is lost leaks a dead client's resources
 * for good, and the process that would like that to happen is exactly
 * the one sending the flood.
 */
/*
 * A thread that blocks in receive before anything is queued, so that a
 * pulse has to wake it rather than being found waiting.
 */
static struct channel *pulse_waiter_ch;
static sys_err_t pulse_waiter_saw;
static uint32_t pulse_waiter_value;

static void
pulse_waiter(void *arg)
{
    struct sys_msg got, reply;
    sys_thread_id_t peer;
    uint64_t cookie;

    (void)arg;

    /*
     * A message first, and answered, so that this thread has served
     * somebody before it waits for the pulse. A receiver that had never
     * served anybody has nothing stale to be handed, and the first
     * version of this check used one: it passed with the wake unmarked,
     * because the field that says which kind of wake it was happened to
     * be zero already.
     */
    if (channel_recv(pulse_waiter_ch, &got, &peer, &cookie) != SYS_OK) {
        panic("pulse: the waiter could not receive");
    }
    if ((got.flags & SYS_MSG_IS_PULSE) != 0) {
        panic("pulse: that should have been a message");
    }
    memset(&reply, 0, sizeof(reply));
    reply.handle = SYS_HANDLE_INVALID;
    if (channel_reply(peer, &reply) != SYS_OK) {
        panic("pulse: the waiter could not reply");
    }

    pulse_waiter_saw = channel_recv(pulse_waiter_ch, &got, &peer, &cookie);
    if (pulse_waiter_saw == SYS_OK) {
        if ((got.flags & SYS_MSG_IS_PULSE) == 0) {
            panic("pulse: a waiting receiver was handed a message");
        }
        pulse_waiter_value = ((const sys_pulse_t *)got.inline_data)->value;
    }

    thread_exit(0);
}

static void
pulse_checks(void)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct channel *ch;
    struct sys_msg got;
    sys_thread_id_t peer;
    sys_handle_t h;
    uint64_t cookie;
    uint32_t i, users, systems, told;

    cnputs("pulses: a notice that cannot be crowded out\n");

    ch = channel_create("pulses", self);
    if (ch == NULL) {
        panic("pulse: no channel");
    }
    expect("a receive handle for it",
      handle_insert(ht, ch, SYS_OBJ_CHANNEL, SYS_RIGHT_RECV, &h), SYS_OK);

    /*
     * More user pulses than the queue will hold, so that dropping is
     * what happens rather than something the check has to arrange.
     */
    for (i = 0; i < SYS_PULSE_SLOTS * 2; i++) {
        channel_pulse(ch, SYS_PULSE_USER_BASE, i);
    }

    /* And a death notice, into a queue user pulses have filled. */
    channel_pulse(ch, SYS_PULSE_DEAD_OWNER, 0x1234);

    users = 0;
    systems = 0;
    told = 0;
    for (;;) {
        const sys_pulse_t *p;

        if (channel_recv(ch, &got, &peer, &cookie) != SYS_OK) {
            panic("pulse: receiving one failed");
        }
        if ((got.flags & SYS_MSG_PULSE_LOST) != 0) {
            told++;
        }
        if ((got.flags & SYS_MSG_IS_PULSE) == 0) {
            panic("pulse: that was not a pulse");
        }

        p = (const sys_pulse_t *)got.inline_data;
        if (p->code == SYS_PULSE_DEAD_OWNER) {
            systems++;
            if (p->value != 0x1234) {
                panic("pulse: the death notice says 0x%lx",
                  (unsigned long)p->value);
            }
        } else {
            users++;
        }

        if (ch->c_pulse_count == 0) {
            break;
        }
    }

    /*
     * The notice arrived, and the user pulses did not all fit. Both
     * halves matter: a queue that held everything would say nothing
     * about the reservation, and a notice that was dropped would be
     * the leak the reservation exists to prevent.
     */
    if (systems != 1) {
        panic("pulse: %u death notices, expected one",
          (unsigned int)systems);
    }

    /*
     * And that the receiver was told, which the comment below has
     * claimed since this check was written and nothing asserted. The
     * negative half was here, that a loss is not reported twice, and
     * it passes just as well when a loss is never reported at all:
     * making the flag always read false broke nothing until this.
     */
    if (told != 1) {
        panic("pulse: the loss was reported %u times, expected once",
          (unsigned int)told);
    }
    if (users >= SYS_PULSE_SLOTS * 2) {
        panic("pulse: %u user pulses survived a queue of %u",
          (unsigned int)users, (unsigned int)SYS_PULSE_SLOTS);
    }
    cnprintf("    %-38s %u of %u kept, the notice too\n",
      "a flood, and a death notice", (unsigned int)users,
      (unsigned int)(SYS_PULSE_SLOTS * 2));

    /*
     * And the receiver was told it missed something. Reported once,
     * because the question is whether anything was lost and not how
     * much: the next receive must not claim a loss already reported.
     */
    channel_pulse(ch, SYS_PULSE_USER_BASE, 7);
    expect("a receive after the loss was reported",
      channel_recv(ch, &got, &peer, &cookie), SYS_OK);
    if ((got.flags & SYS_MSG_PULSE_LOST) != 0) {
        panic("pulse: the loss was reported twice");
    }

    /*
     * And a pulse to a receiver that is already blocked, which is the
     * other half of the queue and the one the first version of this
     * check did not have. Everything above enqueues first and receives
     * afterwards, so the receiver is never waiting when a pulse
     * arrives, and a wake that handed it a stale message would have
     * gone unnoticed.
     */
    {
        struct child pc;

        pulse_waiter_ch = ch;
        pulse_waiter_saw = SYS_ERR_TIMEOUT;

        sched_defer();
        child_watch(&pc, proc_spawn("pulse-waiter", PRIO_BOOT + 1,
          pulse_waiter, NULL, 0));
        sched_undefer();

        /*
         * It outranks this thread, so it has run and is blocked in
         * receive by the time this line is reached. One message first,
         * so that it has served somebody, and then the pulse into the
         * wait it goes back to.
         */
        {
            struct sys_op_connect_args ccargs;
            struct sys_msg req, rep;

            memset(&ccargs, 0, sizeof(ccargs));
            ccargs.rights = SYS_RIGHT_SEND;
            expect("a way to send it one message first",
              run(SYS_OP_CHAN_CONNECT, h, &ccargs, sizeof(ccargs)),
              SYS_OK);

            memset(&req, 0, sizeof(req));
            req.handle = SYS_HANDLE_INVALID;
            expect("and sending it",
              channel_send(ch, 0, &req, &rep, SYS_DEADLINE_NEVER), SYS_OK);
        }

        channel_pulse(ch, SYS_PULSE_USER_BASE, 0xbeef);
        (void)child_join(&pc, JOIN_MS);

        expect("a pulse to a receiver already waiting",
          pulse_waiter_saw, SYS_OK);
        cnprintf("    %-38s 0x%lx\n", "and what it was handed",
          (unsigned long)pulse_waiter_value);
        if (pulse_waiter_value != 0xbeef) {
            panic("pulse: a waiting receiver was handed 0x%lx",
              (unsigned long)pulse_waiter_value);
        }
    }

    /*
     * A program may not send a system pulse. The reserved slots are
     * kept from exactly the program that would like to fill them, and
     * a refusal here is what says so.
     */
    {
        struct sys_op_pulse_args pargs;
        struct sys_op_connect_args cargs;

        memset(&cargs, 0, sizeof(cargs));
        cargs.rights = SYS_RIGHT_SEND;
        expect("a send handle on the pulse channel",
          run(SYS_OP_CHAN_CONNECT, h, &cargs, sizeof(cargs)), SYS_OK);

        memset(&pargs, 0, sizeof(pargs));
        pargs.pulse.code = SYS_PULSE_DEAD_OWNER;
        expect("a program asking for a system pulse",
          run(SYS_OP_PULSE_SEND, cargs.out_handle, &pargs, sizeof(pargs)),
          SYS_ERR_INVALID_ARG);

        memset(&pargs, 0, sizeof(pargs));
        pargs.pulse.code = SYS_PULSE_USER_BASE;
        pargs.pulse.value = 99;
        expect("and a user one", run(SYS_OP_PULSE_SEND, cargs.out_handle,
          &pargs, sizeof(pargs)), SYS_OK);
    }
}

/*
 * Every way this system can block, and a thread broken out of each.
 *
 * Section 10.6 closes the list of ways to block, and section 10.5 says
 * a supervisor never has to know which of them a wedged thread is in.
 * The four cases below are that list, and one call answers all of them
 * because each wait left behind the function that takes a thread off
 * again. That hook has existed since M1 stage 7, put there by
 * deadlines; this is the second caller it was written for.
 */
static void
cancel_checks(void)
{
    static const char *const cases[] = { "recv", "fence", "send", "reply" };
    struct proc *self = proc_current();
    struct sys_op_connect_args cargs;
    struct handle_table *ht = &self->p_handles;
    struct timeline *tl;
    struct channel *idle;
    sys_ns_spec_t ns[4];
    sys_handle_t h_idle, h_fence, h_nobody, h_towedge, h_own;
    uint32_t conns_before = connection_pool()->used;
    uint32_t i;
    bool donated = false;
    void *obj;

    cnputs("cancellation: out of every wait, by the same call\n");

    /* Something to wait on that nothing will ever advance. */
    tl = timeline_create("never");
    if (tl == NULL) {
        panic("cancel: no timeline");
    }
    expect("a timeline nothing signals",
      handle_insert(ht, tl, SYS_OBJ_TIMELINE,
        SYS_RIGHT_WAIT | SYS_RIGHT_DUPLICATE | SYS_RIGHT_TRANSFER,
        &h_fence),
      SYS_OK);

    /* A channel nobody receives on, so a sender queues for ever. */
    idle = channel_create("nobody", self);
    if (idle == NULL) {
        panic("cancel: no idle channel");
    }
    expect("a channel with no receiver",
      handle_insert(ht, idle, SYS_OBJ_CHANNEL, SYS_RIGHT_RECV, &h_idle),
      SYS_OK);

    /* And a server that receives and then says nothing. */
    ch_wedge = channel_create("wedge", self);
    if (ch_wedge == NULL) {
        panic("cancel: no wedge channel");
    }
    expect("a channel for a server that never replies",
      handle_insert(ht, ch_wedge, SYS_OBJ_CHANNEL, SYS_RIGHT_RECV,
        &h_wedge_recv), SYS_OK);
    {
        /*
         * Below the programs that will wedge it, so that serving one
         * raises it. Every server this system already had is at
         * fifteen and every program is spawned below that, so no
         * donation ever happened on a path a cancel could reach, and
         * the priority a cancelled client leaves behind was invisible.
         * The check below asserts the donation happened before it
         * asserts it was undone, because a test of undoing something
         * that never happened passes for the wrong reason.
         */
        wedge_server = thread_create("wedge", 5, never_replies, NULL);
        sched_ready(wedge_server);
    }

    for (i = 0; i < 4; i++) {
        const char *argv[2];
        struct child c;
        struct thread *t;
        int32_t code;

        /* A fresh connection each time: a handle moves rather than copies. */
        memset(&cargs, 0, sizeof(cargs));
        cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
        expect("a connection to nobody",
          run(SYS_OP_CHAN_CONNECT, h_idle, &cargs, sizeof(cargs)), SYS_OK);
        h_nobody = cargs.out_handle;

        memset(&cargs, 0, sizeof(cargs));
        cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
        expect("a connection to the server that never replies",
          run(SYS_OP_CHAN_CONNECT, h_wedge_recv, &cargs, sizeof(cargs)),
          SYS_OK);
        h_towedge = cargs.out_handle;

        /* Its own channel to receive on, which nothing ever sends to. */
        {
            struct channel *own = channel_create("wedged-own", self);

            if (own == NULL) {
                panic("cancel: no channel for it to receive on");
            }
            expect("a receive handle for it",
              handle_insert(ht, own, SYS_OBJ_CHANNEL,
                SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &h_own), SYS_OK);
        }

        memset(ns, 0, sizeof(ns));
        ns[0].id = SYS_NS_SERVICE;
        ns[0].handle = h_own;
        ns[1].id = SYS_NS_PRIVATE + 0;
        {
            sys_handle_t copy;

            /*
             * Not share(), which duplicates with a connection's
             * rights. Section 7.2 only attenuates, so asking for SEND
             * on a handle that carries WAIT is refused rather than
             * quietly given whatever is left.
             */
            if (handle_dup(ht, h_fence,
              SYS_RIGHT_WAIT | SYS_RIGHT_TRANSFER, &copy) != SYS_OK) {
                panic("cancel: cannot hand on the timeline");
            }
            ns[1].handle = copy;
        }
        ns[2].id = SYS_NS_PRIVATE + 1;
        ns[2].handle = h_nobody;
        ns[3].id = SYS_NS_PRIVATE + 2;
        ns[3].handle = h_towedge;

        argv[0] = "wedged";
        argv[1] = cases[i];

        /*
         * At this thread's own priority, so that blocking here is what
         * lets it run and reach its wait. Below this one it would not
         * run at all while there is anything to do here.
         */
        /*
         * Above the server it is about to wedge, which the operation
         * form cannot do: a child may not outrank its parent and this
         * thread is the lowest thing running. The loader path has no
         * such rule because it is the loader.
         */
        sched_defer();
        obj = proc_spawn_image("wedged", 20, ns, 4, argv, 2);
        child_watch(&c, obj);
        sched_undefer();

        t = proc_threads(obj);
        if (t == NULL) {
            panic("cancel: it has no thread");
        }

        /*
         * Until it is actually blocked. A cancel of a thread that has
         * not got there yet is refused rather than lost, which is what
         * makes waiting for it a loop and not a race: the answer says
         * which of the two happened.
         */
        {
            sys_thread_id_t victim = thread_id(t);
            struct sys_msg late;

            while (t->t_state != THREAD_BLOCKED) {
                sched_yield();
            }

            /*
             * That the donation happened, before asserting it was
             * undone. A test of undoing something that never happened
             * passes for the wrong reason, and this one did: every
             * server in this system is at fifteen and every program
             * below it, so nothing on a cancellable path had ever
             * raised anybody.
             */
            donated = wedge_server->t_prio > wedge_server->t_base_prio;
            if (i == 3 && !donated) {
                panic("cancel: the client never raised the server, so "
                  "putting it back proves nothing");
            }

            if (thread_cancel(t) != SYS_OK) {
                panic("cancel: it was blocked and would not be released");
            }

            /*
             * The priority it lent is back. A client cancelled while
             * a server is holding its donation leaves that server at
             * a priority it has no reason for and nothing else will
             * ever take away, because the reply is where a donation
             * was always undone and there is not going to be one.
             */
            if (i == 3 && wedge_server->t_prio !=
              wedge_server->t_base_prio) {
                panic("cancel: the server is still at %u, base %u",
                  (unsigned int)wedge_server->t_prio,
                  (unsigned int)wedge_server->t_base_prio);
            }

            /*
             * Section 10.5 step 3: a server that replies anyway is
             * told the client has moved on. Here rather than after
             * the join, because the thread is runnable and has not
             * run: it is alive, so this is the reply being refused
             * and not the thread being gone, which would be the same
             * answer to a different question.
             */
            if (i == 3) {
                memset(&late, 0, sizeof(late));
                late.handle = SYS_HANDLE_INVALID;
                expect("a reply to a client that was cancelled",
                  channel_reply(victim, &late), SYS_ERR_STALE);
            }
        }

        code = child_join(&c, JOIN_MS);
        if (code != 0) {
            panic("cancel: blocked in %s, it reported %d", cases[i], code);
        }
        cnprintf("    %-38s %s\n", "released, and it said so", cases[i]);
    }

    expect("cancelling something that is not blocked",
      thread_cancel(thread_current()), SYS_ERR_WOULDBLOCK);

    /*
     * And the connections those four runs were given have gone back.
     * Each was moved into the program at spawn and closed with its
     * handle table when it was reaped, which is the only thing that
     * frees one: nothing did before stage four, and the pool was a
     * count of connections ever made rather than open at once.
     */
    if (connection_pool()->used != conns_before) {
        panic("cancel: %u connections in use, started with %u",
          (unsigned int)connection_pool()->used,
          (unsigned int)conns_before);
    }
    cnprintf("    %-38s %u\n", "connections in use, back to",
      (unsigned int)conns_before);
}

/*
 * Slots come back, and a name for one does not follow it.
 *
 * Nothing was ever reaped before this stage, so nothing had ever reused
 * a pool slot, and the generation counters of section 7.1 had only been
 * tried against slots freed by hand. This is the first time the system
 * recycles one itself, which is the hazard those counters are for and
 * the thing the milestone said nothing had yet tried.
 */
static void
reap_checks(void)
{
    struct handle_table *ht = &proc_current()->p_handles;
    struct pool *procs = proc_pool();
    struct pool *threads = thread_pool();
    struct sys_op_status_args sargs;
    sys_thread_id_t first, second;
    struct thread *t;
    struct sys_msg reply;
    uint32_t procs_before;
    sys_id_t stale_proc;
    struct child c;
    sys_handle_t h;
    void *obj;

    cnputs("reaping: a slot comes back, and a name for it does not\n");

    /*
     * A kernel thread, which holds no handle, so it is reapable the
     * moment it exits. Above this thread so that readying it runs it,
     * and then a yield to collect it: a thread is parked when the
     * switch leaves it and freed at the switch after, because the slot
     * being freed contains the stack the freeing would run on.
     */
    t = thread_create("briefly", PRIO_BOOT + 1, briefly, NULL);
    first = thread_id(t);
    sched_ready(t);
    sched_yield();

    if (pool_lookup(threads, first) != NULL) {
        panic("reap: the thread that exited still answers to its id");
    }

    t = thread_create("briefly", PRIO_BOOT + 1, briefly, NULL);
    second = thread_id(t);
    if (sys_id_index(second) != sys_id_index(first)) {
        panic("reap: the next thread took index %u, not %u",
          (unsigned int)sys_id_index(second),
          (unsigned int)sys_id_index(first));
    }
    cnprintf("    %-38s %u, generation %u then %u\n",
      "a thread slot, taken again", (unsigned int)sys_id_index(first),
      (unsigned int)sys_id_gen(first), (unsigned int)sys_id_gen(second));

    /*
     * And the hazard itself. The old name now indexes a live thread,
     * and speaking to it by that name has to be refused rather than
     * reaching whoever inherited the slot. Before this stage nothing
     * could reach this line, because no slot was ever handed back.
     */
    memset(&reply, 0, sizeof(reply));
    reply.handle = SYS_HANDLE_INVALID;
    expect("a reply addressed to the thread that was there",
      channel_reply(first, &reply), SYS_ERR_STALE);

    sched_ready(t);
    sched_yield();

    /*
     * A process, which is not reaped when it exits. Section 10.4 keeps
     * its status readable through a handle, so what makes it a thing
     * nobody can ask about is the last handle going, which is what the
     * join does.
     */
    procs_before = procs->used;

    sched_defer();
    /*
     * At this thread's own priority and not below it. A child below
     * this one is preempted the moment its exit signals the timeline
     * this thread is waiting on, and never runs again while this
     * thread has anything to do, so it never reaches the block that
     * parks it. At equal priority the wake does not preempt and the
     * child finishes leaving.
     */
    if (proc_spawn_op(proc_current(), "status", PRIO_BOOT, NULL, 0,
      NULL, 0, &h) != SYS_OK) {
        panic("reap: could not start a program");
    }
    if (handle_lookup(ht, h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
        panic("reap: the spawn returned no process");
    }
    child_watch(&c, obj);
    sched_undefer();

    stale_proc = ((struct proc *)obj)->p_id;

    (void)child_join(&c, JOIN_MS);

    /*
     * The spawn handed back a handle of its own and the watch made a
     * second one. The join closes what it opened and not what it was
     * given, so this closes the other, and only then is there nothing
     * left that could ask.
     */
    if (handle_close(ht, h) != SYS_OK) {
        panic("reap: the spawn handle was already gone");
    }

    /*
     * A few scheduling decisions, because the process is not the last
     * thing to go and reaping a thread cannot be prompt. A thread is
     * parked when a switch leaves it and collected at the next
     * decision, and the exiting thread here is preempted before it
     * parks: signalling its completion wakes this thread, which
     * outranks it, so it is still running when this one resumes and
     * has yet to reach the block that parks it.
     *
     * So the slot comes back within a bounded number of decisions
     * rather than at a particular one, and this waits for that rather
     * than asserting a count. Tuning it to an exact number of yields
     * would be a check about the scheduler dressed as a check about
     * reaping.
     */
    {
        uint32_t spin;

        for (spin = 0; spin < 8 && procs->used != procs_before; spin++) {
            sched_yield();
        }
    }

    if (procs->used != procs_before) {
        panic("reap: %u processes in use, started with %u",
          (unsigned int)procs->used, (unsigned int)procs_before);
    }
    if (pool_lookup(procs, stale_proc) != NULL) {
        panic("reap: the reaped process still answers to its id");
    }
    cnprintf("    %-38s %u\n", "processes in use, back to",
      (unsigned int)procs->used);

    memset(&sargs, 0, sizeof(sargs));
    expect("its handle, used after the join closed it",
      run(SYS_OP_PROC_STATUS, c.c_handle, &sargs, sizeof(sargs)),
      SYS_ERR_STALE);

    /*
     * And a completion handle held past everything else, which is
     * section 10.4: what reports an exit outlives the object that
     * exited. A completion timeline lives inside its process, so the
     * handle is a pointer into a pool slot, and without something
     * holding the object this would be reading whatever spawned next.
     *
     * Every other handle goes first, and the wait afterwards has to
     * still be satisfied by the exit that already happened rather than
     * blocking on a timeline that has been reused and reset.
     */
    sched_defer();
    if (proc_spawn_op(proc_current(), "status", PRIO_BOOT, NULL, 0,
      NULL, 0, &h) != SYS_OK) {
        panic("reap: could not start the third program");
    }
    if (handle_lookup(ht, h, SYS_OBJ_PROCESS, 0, &obj) != SYS_OK) {
        panic("reap: the third spawn returned no process");
    }
    child_watch(&c, obj);
    sched_undefer();

    {
        struct sys_op_fence_args fargs;
        sys_handle_t keep = c.c_completion;
        uint32_t spin;

        fargs.value = SYS_COMPLETION_EXITED;
        fargs.deadline = time_now() + time_from_ms(1000);
        expect("waiting for it to finish",
          run(SYS_OP_FENCE_WAIT, keep, &fargs, sizeof(fargs)), SYS_OK);

        if (handle_close(ht, c.c_handle) != SYS_OK ||
          handle_close(ht, h) != SYS_OK) {
            panic("reap: could not let go of the third program");
        }
        for (spin = 0; spin < 8; spin++) {
            sched_yield();
        }

        fargs.value = SYS_COMPLETION_EXITED;
        fargs.deadline = time_now() + time_from_ms(1000);
        expect("its completion, with nothing else holding it",
          run(SYS_OP_FENCE_WAIT, keep, &fargs, sizeof(fargs)), SYS_OK);

        (void)handle_close(ht, keep);
    }
}
/*
 * Two processes in one protection domain, per section 18.
 *
 * The check is the smallest thing a cohort can be shown to do that a
 * pair of ordinary processes cannot: one member is handed the address
 * of a word in the other's memory, and it reads and writes it. Nothing
 * in the message is a buffer descriptor, so the kernel maps nothing,
 * validates nothing, and never learns that an address crossed at all.
 *
 * The same code runs at both tiers and both must pass, which sounds
 * like a check that proves nothing and is the opposite. At Tier 0 it
 * passes because there is one address space, which is what Tier 0 is.
 * At Tier 2 it passes only because the build put these two images in
 * one cohort, and every other pair of processes at that tier would
 * fault on the same message. Making the two tiers agree here is the
 * whole content of the mechanism.
 *
 * The peer that faults is not this check. It is run by hand at Tier 2,
 * because the two tiers disagree about it and one transcript covers all
 * four configurations.
 */

/* Kept, because the recovery check has to watch it end. */
static struct child peer_a_child;

/*
 * A member faults, the cohort goes with it, and a supervisor puts it
 * back. Sections 18.2 and 18.3.
 *
 * M6 built the domain and stopped at the fault, printing what was
 * missing. This is the missing part, and the interesting half of it is
 * not the restart: it is that the peer which did nothing wrong is ended
 * too. A survivor left running would be the per-process recovery
 * section 18.2 forbids, and the reason it forbids it is that a member
 * that faulted may have written anything into any peer before it did.
 */
static void
recovery_checks(struct channel *ch)
{
    struct proc *self = proc_current();
    struct handle_table *ht = &self->p_handles;
    struct cohort *peers = cohort_of_image("peer_a");
    struct sys_op_connect_args cargs;
    sys_ns_spec_t ns[2];
    sys_handle_t h_recv;
    struct child c, bc;
    int32_t code;

    cnputs("cohort recovery: one faults and all of them go\n");

    if (peers == NULL) {
        panic("recovery: the peers are in no cohort");
    }

    /*
     * peer_c exists to die. Watched, because everything that ends here
     * has to be: an exit nobody asked about halts the machine, and the
     * whole point of this check is that three processes end.
     */
    sched_defer();
    child_watch(&c, proc_spawn_image("peer_c", 20, NULL, 0, NULL, 0));
    sched_undefer();

    code = child_join(&c, JOIN_MS);
    if (code != SYS_EXIT_FAULT) {
        panic("recovery: peer_c exited %d, not of a fault", code);
    }

    /*
     * And the peer that did nothing. It was blocked in receive and had
     * no reason of its own to stop.
     */
    code = child_join(&peer_a_child, JOIN_MS);
    if (code != SYS_EXIT_COHORT) {
        panic("recovery: peer_a exited %d, not with its cohort", code);
    }
    cnprintf("    %-38s %s\n", "the peer that did not fault, ended",
      "with its cohort");

    /*
     * No place is held by anything still running, which is what a
     * restart needs. The rule is about live processes and not about
     * pointers: a member that has exited does not hold its place, and
     * the pointer to it is cleared when it is reaped, which happens
     * when the last thing that could ask about it lets go. Asserting
     * the pointer were already NULL would be asserting when somebody
     * else closed a handle.
     */
    {
        uint32_t j;

        for (j = 0; j < peers->c_nmembers; j++) {
            struct proc *m = peers->c_running[j];

            if (m != NULL && !m->p_exited) {
                panic("recovery: %s is still running", m->p_name);
            }
        }
    }
    cnputs("    no member of it is running any more\n");

    /*
     * And a supervisor puts it back. The kernel is root here, per
     * section 24.5, and section 18.3 is clear that what it does with
     * the fact is not the kernel's business: this is one policy, which
     * is to start the members again once.
     */
    /*
     * Fresh ends, because a handle moves: the ones the first pair were
     * given went with them. The channel is what outlives the processes
     * serving it, which is what lets a restart be invisible to anybody
     * holding a connection, and holding it is what makes this thread a
     * supervisor rather than a bystander.
     */
    expect("a receive handle for the replacement",
      handle_insert(ht, ch, SYS_OBJ_CHANNEL,
        SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &h_recv), SYS_OK);

    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = 0;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("and a connection to it",
      run(SYS_OP_CHAN_CONNECT, h_recv, &cargs, sizeof(cargs)), SYS_OK);

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_SERVICE;
    ns[0].handle = h_recv;

    sched_defer();
    child_watch(&peer_a_child,
      proc_spawn_image("peer_a", 15, ns, 1, NULL, 0));
    sched_undefer();

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_ROOT;
    ns[0].handle = cargs.out_handle;

    /* And an outsider again, for the half of its check that needs one. */
    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = FS_NODE_ROOT;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("something outside the cohort, again",
      run(SYS_OP_CHAN_CONNECT, h_fs_recv, &cargs, sizeof(cargs)), SYS_OK);
    ns[1].id = PEER_NS_OUTSIDER;
    ns[1].handle = cargs.out_handle;

    sched_defer();
    child_watch(&bc, proc_spawn_image("peer_b", 20, ns, 2, NULL, 0));
    sched_undefer();

    code = child_join(&bc, JOIN_MS);
    if (code != 0) {
        panic("recovery: after the restart peer_b stopped at step %d",
          code);
    }
    cnputs("    and the pair works again, through new processes\n");
}
static void
cohort_checks(void)
{
    struct proc *self = proc_current();
    struct sys_op_connect_args cargs;
    sys_ns_spec_t ns[2];
    struct channel *ch;
    struct child c;
    struct proc *p;
    sys_handle_t h_recv, to, h_second;
    void *obj;
    int32_t code;
    sys_err_t err;

    cnputs("cohorts: two processes, one address space\n");

    ch = channel_create("peers", self);
    if (ch == NULL) {
        panic("cohort check: no channel");
    }

    err = handle_insert(&self->p_handles, ch, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &h_recv);
    expect("a receive handle for the pair", err, SYS_OK);

    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = 0;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a connection from one member to the other",
      run(SYS_OP_CHAN_CONNECT, h_recv, &cargs, sizeof(cargs)), SYS_OK);
    to = cargs.out_handle;

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_SERVICE;
    ns[0].handle = h_recv;
    /*
     * Watched, which it was not before. A server that never exits
     * needs nobody to be waiting; a member of a cohort does, because
     * section 18.2 has it end when a peer faults and an exit nobody
     * asked about halts the machine.
     */
    sched_defer();
    p = proc_spawn_image("peer_a", 15, ns, 1, NULL, 0);
    child_watch(&peer_a_child, p);
    sched_undefer();

    if (p->p_cohort == NULL) {
        panic("cohort check: peer_a is in no cohort");
    }

    /*
     * Spawned and watched under one deferral, for the reason
     * run_client() gives: the child outranks this thread and would
     * finish before anything held its completion timeline.
     */
    /*
     * And a connection to something outside the cohort, so that the
     * other half of the rule can be tried. The kernel's own file
     * server will do and is the honest choice: it is a kernel thread,
     * so it is in no cohort at all, which is what an outsider is.
     */
    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = FS_NODE_ROOT;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a connection to something outside the cohort",
      run(SYS_OP_CHAN_CONNECT, h_fs_recv, &cargs, sizeof(cargs)), SYS_OK);

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_ROOT;
    ns[0].handle = to;
    ns[1].id = PEER_NS_OUTSIDER;
    ns[1].handle = cargs.out_handle;

    sched_defer();
    p = proc_spawn_image("peer_b", 20, ns, 2, NULL, 0);
    child_watch(&c, p);
    sched_undefer();

    if (p->p_cohort == NULL) {
        panic("cohort check: peer_b is in no cohort");
    }

    code = child_join(&c, JOIN_MS);
    if (code != 0) {
        panic("cohort check: peer_b stopped at step %d", code);
    }

    cnputs("    a peer read and wrote memory it was only given the "
      "address of\n");

    /*
     * And a second copy of a member that is still running is refused.
     * peer_a is blocked in receive with its slot mapped in the shared
     * space; loading another at the address it was linked for would
     * write over it. This was not refused when the stage was first
     * built, and the loader duly moved the running peer_a's slot base
     * from one frame to another underneath it.
     *
     * At Tier 0 the loader's own one-copy rule answers first and this
     * never reaches the cohort. Both refuse, both say WOULDBLOCK, and
     * the check does not care which rule spoke, because a condition
     * with two answers is worse than a rule stated twice.
     *
     * peer_b is the other case and must still be allowed: it has
     * exited, so its place is free and a second one may have it.
     */
    expect("a second copy of a member that is still running",
      proc_spawn_op(self, "peer_a", PRIO_BOOT - 1, NULL, 0, NULL, 0,
        NULL),
      SYS_ERR_WOULDBLOCK);

    /*
     * And the other half of the same rule. peer_b has exited, so its
     * place is free and a second one may have it. Without this the
     * refusal above would pass just as well if a member could never be
     * spawned twice at all, which is a different rule and the wrong
     * one: M7 restarts a cohort by spawning its members again.
     *
     * It is given nothing, so it stops at its first lookup and exits.
     * What is being checked is that it was allowed to start.
     */
    sched_defer();
    err = proc_spawn_op(self, "peer_b", PRIO_BOOT - 1, NULL, 0, NULL, 0,
      &h_second);
    if (err == SYS_OK &&
      handle_lookup(&self->p_handles, h_second, SYS_OBJ_PROCESS, 0,
        &obj) == SYS_OK) {
        child_watch(&c, obj);
    }
    sched_undefer();
    expect("a second copy of one that has exited", err, SYS_OK);
    (void)child_join(&c, JOIN_MS);

    /*
     * And the handle the spawn returned, which the join does not
     * close because it did not open it. Until it goes the process is
     * not reaped, and until the process is reaped its place in the
     * cohort is held by a program that has finished.
     */
    if (err == SYS_OK && handle_close(&self->p_handles, h_second) !=
      SYS_OK) {
        panic("cohort check: the second spawn handle was already gone");
    }
    /*
     * And what M6 left for this milestone: a member faults, the whole
     * cohort goes, and a supervisor starts it again. Here rather than
     * in a check of its own because it needs the pair that is already
     * running and the handles they were given.
     */
    recovery_checks(ch);

}

/* The console and the directory the shell is handed, kept from the
 * checks that created them. Root will hold both when there is a root;
 * until then the kernel is what assembles the system. */

/*
 * Section 13.3 wants figures. Before any of them mean anything, the
 * clock they come from has to be one clock, shared by both cores, and
 * monotonic; and there has to be a unit that does not move when the
 * host does.
 */
#define CLK_ONCE    1024u
#define CLK_TWICE   (CLK_ONCE * 2u)

static struct meas meas_once = { .ms_name = "once" };
static struct meas meas_twice = { .ms_name = "twice" };

/*
 * One region, entered from both cores at once. The two above cannot
 * tell a per-core counter from a single shared one, because one core
 * doing all the work gives the same total either way: forcing every
 * increment into one slot leaves them reading 1024 and 2048 and passing.
 * This one is entered by both cores together, so a shared slot loses
 * increments and the total comes up short.
 */
#define CLK_SHARE   4096u

static struct meas meas_shared = { .ms_name = "shared" };

#if SYS_TIER == 2
static volatile uint64_t clk_stamp;
static volatile uint32_t clk_go;
static volatile uint32_t clk_done;

static void
clk_thread(void *arg)
{
    (void)arg;

    while (clk_go == 0) {
        md_cpu_relax();
    }

    clk_stamp = meas_now();

    /*
     * And this core's share of the region both of them enter, run
     * while the first core is entering it too.
     */
    {
        uint32_t i;

        for (i = 0; i < CLK_SHARE; i++) {
            uint64_t t0 = meas_enter();

            meas_leave(&meas_shared, t0);
        }
    }

    md_cpu_fence();
    clk_done = 1;
    thread_exit(0);
}
#endif


/*
 * Section 12.3's bound, and the deadlock it names.
 *
 * A cycle of one: a server that takes a request and then sends into the
 * channel it serves. It is now waiting for itself, and nothing will
 * ever arrive. One server and one channel is the whole of it, which is
 * why the cycle is built this way rather than out of two servers
 * pointing at each other: the walk cannot tell the difference and the
 * cheap one needs two fewer threads.
 */
static struct channel *ch_cyc;
static sys_handle_t h_cyc_recv;
static sys_handle_t h_cyc_send;
static struct thread *cyc_server;
static volatile uint32_t cyc_ready;

static void
cyc_server_thread(void *arg)
{
    struct sys_op_recv_args rx;
    struct sys_op_send_args tx;

    (void)arg;

    memset(&rx, 0, sizeof(rx));
    if (run(SYS_OP_IPC_RECV, h_cyc_recv, &rx, sizeof(rx)) != SYS_OK) {
        panic("cycle server: receive failed");
    }

    /*
     * Serving, and now sending into the channel it is serving. Nobody
     * is at a receive point on it, because this thread is the only one
     * that ever receives there and it is here.
     */
    cyc_ready = 1;

    memset(&tx, 0, sizeof(tx));
    tx.request.inline_len = 3;
    memcpy(tx.request.inline_data, "no", 3);
    (void)run(SYS_OP_IPC_SEND, h_cyc_send, &tx, sizeof(tx));

    thread_exit(0);
}

static void
cyc_client_thread(void *arg)
{
    struct sys_op_send_args tx;

    memset(&tx, 0, sizeof(tx));
    tx.request.inline_len = 3;
    memcpy(tx.request.inline_data, "hi", 3);
    (void)run(SYS_OP_IPC_SEND, (sys_handle_t)(uintptr_t)arg, &tx,
      sizeof(tx));

    thread_exit(0);
}


/*
 * The case the chain walk was built for, which nothing else in this
 * tree produces.
 *
 * A client queues on a server that is already inside a request and
 * blocked further down. Everything below that server is running at the
 * priority of the client it is serving, and the new client outranks all
 * of it. Without the walk the new client waits behind a chain nobody
 * raised, which is the inversion section 12.3 exists to prevent.
 *
 * The stage that built the walk reported that it raises nobody. That is
 * true of the workload this tree had, and it meant the raising half of
 * the mechanism was never once executed. This is that workload.
 */
#define INV_BASE    5
#define INV_LO      6
#define INV_HI      20

static struct channel *ch_inv_a;
static struct channel *ch_inv_b;
static struct channel *ch_inv_idle;
static sys_handle_t h_inv_a_recv, h_inv_a_send;
static sys_handle_t h_inv_b_recv, h_inv_b_send;
static sys_handle_t h_inv_idle_recv;
static struct thread *inv_a;
static struct thread *inv_b;

static void
inv_b_thread(void *arg)
{
    struct sys_op_recv_args rx;

    (void)arg;

    memset(&rx, 0, sizeof(rx));
    if (run(SYS_OP_IPC_RECV, h_inv_b_recv, &rx, sizeof(rx)) != SYS_OK) {
        panic("inversion: B never received");
    }

    /*
     * Serving, and now blocked on something else without replying. This
     * is what makes the chain below A stay where it is.
     */
    memset(&rx, 0, sizeof(rx));
    (void)run(SYS_OP_IPC_RECV, h_inv_idle_recv, &rx, sizeof(rx));

    thread_exit(0);
}

static void
inv_a_thread(void *arg)
{
    struct sys_op_recv_args rx;
    struct sys_op_send_args tx;

    (void)arg;

    memset(&rx, 0, sizeof(rx));
    if (run(SYS_OP_IPC_RECV, h_inv_a_recv, &rx, sizeof(rx)) != SYS_OK) {
        panic("inversion: A never received");
    }

    memset(&tx, 0, sizeof(tx));
    tx.request.inline_len = 2;
    memcpy(tx.request.inline_data, "d", 2);
    (void)run(SYS_OP_IPC_SEND, h_inv_b_send, &tx, sizeof(tx));

    thread_exit(0);
}

static void
inv_client_thread(void *arg)
{
    struct sys_op_send_args tx;

    memset(&tx, 0, sizeof(tx));
    tx.request.inline_len = 2;
    memcpy(tx.request.inline_data, "c", 2);
    (void)run(SYS_OP_IPC_SEND, (sys_handle_t)(uintptr_t)arg, &tx,
      sizeof(tx));

    thread_exit(0);
}

static bool
inv_settled(void)
{
    return inv_a->t_state == THREAD_BLOCKED &&
      inv_a->t_wait == THREAD_WAIT_REPLY &&
      inv_b->t_state == THREAD_BLOCKED &&
      inv_b->t_wait == THREAD_WAIT_RECV;
}

static void
inversion_checks(void)
{
    struct proc *p = proc_current();
    struct thread *hi;
    uint64_t before, deepest;
    uint32_t spin;

    cnputs("chains: a client behind a chain that outranks it\n");

    ch_inv_a = channel_create("inv-a", p);
    ch_inv_b = channel_create("inv-b", p);
    ch_inv_idle = channel_create("inv-idle", p);
    if (ch_inv_a == NULL || ch_inv_b == NULL || ch_inv_idle == NULL) {
        panic("inversion: no channels");
    }

    expect("channels for a two deep chain",
      handle_insert(&p->p_handles, ch_inv_a, SYS_OBJ_CHANNEL,
        SYS_RIGHT_RECV, &h_inv_a_recv), SYS_OK);
    (void)handle_insert(&p->p_handles, ch_inv_a, SYS_OBJ_CHANNEL,
      SYS_RIGHT_SEND, &h_inv_a_send);
    (void)handle_insert(&p->p_handles, ch_inv_b, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV, &h_inv_b_recv);
    (void)handle_insert(&p->p_handles, ch_inv_b, SYS_OBJ_CHANNEL,
      SYS_RIGHT_SEND, &h_inv_b_send);
    (void)handle_insert(&p->p_handles, ch_inv_idle, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV, &h_inv_idle_recv);

    inv_b = thread_create("inv-b", INV_BASE, inv_b_thread, NULL);
    sched_ready(inv_b);
    inv_a = thread_create("inv-a", INV_BASE, inv_a_thread, NULL);
    sched_ready(inv_a);
    sched_ready(thread_create("inv-lo", INV_LO, inv_client_thread,
      (void *)(uintptr_t)h_inv_a_send));

    for (spin = 0; spin < 2000000u && !inv_settled(); spin++) {
        sched_yield();
    }
    if (!inv_settled()) {
        panic("inversion: the chain never settled");
    }

    cnprintf("    a chain serving %u, two deep            OK\n",
      (unsigned int)INV_LO);
    if (inv_a->t_prio != INV_LO || inv_b->t_prio != INV_LO) {
        panic("inversion: the chain runs at %u and %u, not %u",
          (unsigned int)inv_a->t_prio, (unsigned int)inv_b->t_prio,
          (unsigned int)INV_LO);
    }

    /*
     * And now somebody who outranks all of it, queuing behind a server
     * that cannot take the request.
     */
    before = channel_chain_raised(&deepest);
    hi = thread_create("inv-hi", INV_HI, inv_client_thread,
      (void *)(uintptr_t)h_inv_a_send);
    sched_ready(hi);

    for (spin = 0; spin < 2000000u &&
      !(hi->t_state == THREAD_BLOCKED && hi->t_wait == THREAD_WAIT_SEND);
      spin++) {
        sched_yield();
    }
    if (hi->t_wait != THREAD_WAIT_SEND) {
        panic("inversion: the high client never queued");
    }

    cnprintf("    a client at %u queued behind it        %s\n",
      (unsigned int)INV_HI,
      inv_a->t_prio == INV_HI && inv_b->t_prio == INV_HI ? "OK" : "WRONG");
    if (inv_a->t_prio != INV_HI || inv_b->t_prio != INV_HI) {
        panic("inversion: the chain stayed at %u and %u",
          (unsigned int)inv_a->t_prio, (unsigned int)inv_b->t_prio);
    }

    cnprintf("    the walk raised %lu of them            OK\n",
      (unsigned long)(channel_chain_raised(&deepest) - before));
}

static void
chain_bound_checks(void)
{
    struct proc *p = proc_current();
    struct sys_op_send_args tx;
    uint32_t spin;
    sys_err_t err;

    cnputs("chains: the bound of section 12.3\n");

    ch_cyc = channel_create("cycle", p);
    if (ch_cyc == NULL) {
        panic("cycle: no channel");
    }
    err = handle_insert(&p->p_handles, ch_cyc, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV, &h_cyc_recv);
    expect("a receive handle for the cycle", err, SYS_OK);
    err = handle_insert(&p->p_handles, ch_cyc, SYS_OBJ_CHANNEL,
      SYS_RIGHT_SEND, &h_cyc_send);
    expect("a send handle for it", err, SYS_OK);

    cyc_server = thread_create("cyc-server", PRIO_BOOT, cyc_server_thread,
      NULL);
    sched_ready(cyc_server);
    sched_ready(thread_create("cyc-client", PRIO_BOOT, cyc_client_thread,
      (void *)(uintptr_t)h_cyc_send));

    /*
     * Waited for rather than assumed. The walk finds a cycle only once
     * the server is actually blocked sending into its own channel, and
     * sending before that would queue behind it and never return.
     */
    for (spin = 0; spin < 2000000u &&
      !(cyc_ready != 0 && cyc_server->t_wait == THREAD_WAIT_SEND); spin++) {
        sched_yield();
    }
    if (cyc_ready == 0 || cyc_server->t_wait != THREAD_WAIT_SEND) {
        panic("cycle: the server never got into its own channel");
    }

    memset(&tx, 0, sizeof(tx));
    tx.request.inline_len = 3;
    memcpy(tx.request.inline_data, "?", 2);
    expect("sending into a cycle",
      run(SYS_OP_IPC_SEND, h_cyc_send, &tx, sizeof(tx)),
      SYS_ERR_DEADLOCK);

    cnputs("    and a chain shorter than the bound is served OK\n");
}


/*
 * Section 17: the count exceeds one.
 *
 * Signals on a timeline are what the entries do, because a timeline is
 * a counter that only goes up: after a batch of three the value says
 * exactly how many of them ran, and after a batch that was refused it
 * says whether any of them did. An operation with no visible effect
 * would leave the second question unanswerable.
 */
#define BATCH_N     3

static struct timeline *tl_batch;
static sys_handle_t h_batch;

static void
batch_fill_on(sys_op_t *ops, sys_op_fence_args_t *args, sys_handle_t h,
  uint32_t n, uint64_t from)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        memset(&args[i], 0, sizeof(args[i]));
        args[i].value = from + i + 1;

        memset(&ops[i], 0, sizeof(ops[i]));
        ops[i].opcode = SYS_OP_FENCE_SIGNAL;
        ops[i].target = h;
        ops[i].arg_ptr = (uintptr_t)&args[i];
        ops[i].arg_len = sizeof(args[i]);
    }
}

static void
batch_fill(sys_op_t *ops, sys_op_fence_args_t *args, uint32_t n,
  uint64_t from)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        memset(&args[i], 0, sizeof(args[i]));
        args[i].value = from + i + 1;

        memset(&ops[i], 0, sizeof(ops[i]));
        ops[i].opcode = SYS_OP_FENCE_SIGNAL;
        ops[i].target = h_batch;
        ops[i].arg_ptr = (uintptr_t)&args[i];
        ops[i].arg_len = sizeof(args[i]);
    }
}


/*
 * Section 17.3, and the half of it this tree can reach.
 *
 * An issuing op starts work and hands back a timeline, so that a batch
 * can start three things and then wait on all three with one thread
 * and no worker pool. The three opcodes for it have answered
 * SYS_ERR_NOT_SUPPORTED since they were given numbers, because
 * starting work means a device that transfers, and nothing in this
 * tree transfers.
 *
 * What is here is the other half. Collecting timelines and waiting on
 * them in one submission is built, and hardware that finishes on its
 * own schedule is built: a timer advances a timeline at a deadline
 * without the thread doing anything. Arming three of them and waiting
 * on all three in one batch is section 17.3's shape with the issuing
 * op cut out of it, and it says which part is missing by showing the
 * part that is not.
 */
#define FLIGHT_N    3

static void
inflight_checks(void)
{
    static sys_op_fence_args_t args[FLIGHT_N];
    static sys_op_t ops[FLIGHT_N];
    struct proc *p = proc_current();
    struct timeline *tl[FLIGHT_N];
    struct timer *tm[FLIGHT_N];
    sys_handle_t h[FLIGHT_N];
    sys_time_t began, ended;
    uint32_t i;
    long r;

    cnputs("in flight: what section 17.3 asks for, and what is here\n");

    /*
     * Refused, and distinctly. An opcode nobody has defined is
     * INVALID_ARG; these three are defined and not built, which is a
     * different thing for a caller to be told.
     *
     * All three, where the dispatch check tries one. That check is
     * about the entry point telling the two apart; this is about the
     * premise of this stage, and a premise resting on two opcodes
     * nobody has ever called is not one to leave untested.
     */
    {
        sys_op_t none;
        uint16_t code[3] = { SYS_OP_DMA_ISSUE, SYS_OP_READ_ISSUE,
          SYS_OP_WRITE_ISSUE };

        for (i = 0; i < 3; i++) {
            memset(&none, 0, sizeof(none));
            none.opcode = code[i];
            if (run_batch(&none, 1) != SYS_ERR_NOT_SUPPORTED) {
                panic("in flight: opcode %u did not refuse as unbuilt",
                  (unsigned int)code[i]);
            }
        }
    }
    cnputs("    the three issuing opcodes refuse, as unbuilt OK\n");

    /*
     * Three timers, armed one after another and none of them waited
     * for. From here they run without this thread, which is the whole
     * of what an issuing op would have arranged for a device.
     */
    for (i = 0; i < FLIGHT_N; i++) {
        tl[i] = timeline_create("inflight");
        if (tl[i] == NULL) {
            panic("in flight: no timeline");
        }
        if (handle_insert(&p->p_handles, tl[i], SYS_OBJ_TIMELINE,
          SYS_RIGHT_SIGNAL | SYS_RIGHT_WAIT, &h[i]) != SYS_OK) {
            panic("in flight: no handle slot");
        }
    }

    began = time_now();
    for (i = 0; i < FLIGHT_N; i++) {
        tm[i] = timer_create("inflight",
          tl[i], began + time_from_ms(20 * (i + 1)), 0);
        if (tm[i] == NULL) {
            panic("in flight: no timer");
        }
    }

    /*
     * And one submission that waits on all three. The last to arrive
     * is the one that decides when this returns, because the other two
     * were already running while it did.
     */
    memset(ops, 0, sizeof(ops));
    memset(args, 0, sizeof(args));
    for (i = 0; i < FLIGHT_N; i++) {
        ops[i].opcode = SYS_OP_FENCE_WAIT;
        ops[i].target = h[i];
        ops[i].arg_ptr = (uintptr_t)&args[i];
        ops[i].arg_len = sizeof(args[i]);
        args[i].value = 1;
        args[i].deadline = SYS_DEADLINE_NEVER;
    }

    r = run_batch(ops, FLIGHT_N);
    ended = time_now();

    if (r != SYS_OK) {
        panic("in flight: the batch returned %ld", r);
    }
    for (i = 0; i < FLIGHT_N; i++) {
        if (timeline_value(tl[i]) != 1) {
            panic("in flight: timeline %u is at %lu", (unsigned int)i,
              (unsigned long)timeline_value(tl[i]));
        }
        timer_destroy(tm[i]);
        if (handle_close(&p->p_handles, h[i]) != SYS_OK) {
            panic("in flight: the handle would not close");
        }
    }

    /*
     * Asserted as a count and reported as a duration, per the rule
     * this plan keeps arriving at. What is countable is that one
     * submission waited on three things and all three arrived.
     *
     * And what the two durations below do not show, which matters more
     * than what they do. A timer does no work. Three deadlines set
     * from one origin are reached by the latest of them, and three set
     * one after another are reached by their sum, and that is
     * arithmetic about a clock rather than evidence that anything ran
     * at the same time as anything else. Nothing here is transferring
     * while the thread waits, because there is nothing here that
     * transfers.
     *
     * What they do show is the engine's shape: that several
     * completions can be outstanding at once and waited for in one
     * submission by one thread with no worker pool. That is the half
     * of section 17.3 this tree has. The half it does not have is a
     * device, and no arrangement of what is here would demonstrate
     * concurrent work, because there is no work to make concurrent.
     */
    cnprintf("    %u waits in one submission, all arrived    OK\n",
      (unsigned int)FLIGHT_N);
    cnprintf("    all three at once, in one submission: about %lu ms\n",
      (unsigned long)((ended - began) / time_from_ms(1)));

    /*
     * And the same three, armed and waited for one at a time, which is
     * what a caller without an issuing op has to do. Reported beside
     * the other because the comparison is the point and neither number
     * is a property of this kernel: what the two of them together show
     * is whether the three were running at once, which is.
     */
    {
        sys_time_t s_began = time_now();

        for (i = 0; i < FLIGHT_N; i++) {
            struct timeline *one = timeline_create("serial");
            struct timer *t1;
            sys_handle_t oh;
            sys_op_fence_args_t a1;
            sys_op_t o1;

            if (one == NULL) {
                panic("in flight: no timeline");
            }
            if (handle_insert(&p->p_handles, one, SYS_OBJ_TIMELINE,
              SYS_RIGHT_SIGNAL | SYS_RIGHT_WAIT, &oh) != SYS_OK) {
                panic("in flight: no handle slot");
            }

            t1 = timer_create("serial", one,
              time_now() + time_from_ms(20 * (i + 1)), 0);
            if (t1 == NULL) {
                panic("in flight: no timer");
            }

            memset(&o1, 0, sizeof(o1));
            memset(&a1, 0, sizeof(a1));
            o1.opcode = SYS_OP_FENCE_WAIT;
            o1.target = oh;
            o1.arg_ptr = (uintptr_t)&a1;
            o1.arg_len = sizeof(a1);
            a1.value = 1;
            a1.deadline = SYS_DEADLINE_NEVER;

            if (run_batch(&o1, 1) != SYS_OK) {
                panic("in flight: a serial wait failed");
            }

            timer_destroy(t1);
            if (handle_close(&p->p_handles, oh) != SYS_OK) {
                panic("in flight: the handle would not close");
            }
        }

        cnprintf("    one at a time, the same three:  about %lu ms\n",
          (unsigned long)((time_now() - s_began) / time_from_ms(1)));
    }
}

/*
 * Section 17.5's last line: what an aborted batch signals.
 *
 * A chain that fails partway leaves the entries after it unrun, and a
 * thread waiting on a timeline one of them was going to signal has
 * nothing left to wait for. Nothing else will ever tell it so: the
 * batch is over, the entry is cancelled, and the value it would have
 * signalled will never arrive.
 */
static struct timeline *ab_tl;
static volatile int ab_woke;

static void
abort_waiter(void *arg)
{
    (void)arg;

    /*
     * Waiting for a value the batch was going to signal, with no
     * deadline. A deadline would make this pass on a timeout and
     * prove nothing: what is being checked is that the waiter is told,
     * not that it eventually gives up.
     */
    ab_woke = (int)timeline_wait(ab_tl, 1, SYS_DEADLINE_NEVER);
    thread_exit(0);
}

static void
abort_checks(void)
{
    static sys_op_fence_args_t args[2];
    static sys_op_t ops[2];
    struct proc *p = proc_current();
    sys_handle_t h;
    uint32_t spin;
    sys_err_t err;
    long r;

    cnputs("aborting: what a batch that stops signals, section 17.5\n");

    ab_tl = timeline_create("abort");
    if (ab_tl == NULL) {
        panic("abort: no timeline");
    }
    err = handle_insert(&p->p_handles, ab_tl, SYS_OBJ_TIMELINE,
      SYS_RIGHT_SIGNAL | SYS_RIGHT_WAIT, &h);
    expect("a handle for the timeline nobody will advance", err, SYS_OK);

    ab_woke = 1;
    sched_ready(thread_create("abort-waiter", PRIO_BOOT + 1,
      abort_waiter, NULL));

    /* Let it reach the wait before anything is submitted. */
    for (spin = 0; spin < 1000 && ab_woke == 1; spin++) {
        sched_yield();
    }

    /*
     * A chain whose first entry fails, followed by the entry that
     * would have signalled. The first fails because its target is not
     * a timeline it may signal; what matters is only that it fails.
     */
    memset(ops, 0, sizeof(ops));
    memset(args, 0, sizeof(args));

    ops[0].opcode = SYS_OP_FENCE_SIGNAL;
    ops[0].target = SYS_HANDLE_INVALID;
    ops[0].flags = SYS_OP_LINK;
    ops[0].arg_ptr = (uintptr_t)&args[0];
    ops[0].arg_len = sizeof(args[0]);
    args[0].value = 1;

    ops[1].opcode = SYS_OP_FENCE_SIGNAL;
    ops[1].target = h;
    ops[1].arg_ptr = (uintptr_t)&args[1];
    ops[1].arg_len = sizeof(args[1]);
    args[1].value = 1;

    r = run_batch(ops, 2);
    if (r == SYS_OK) {
        panic("abort: the chain did not fail");
    }
    if (ops[1].status != SYS_ERR_CANCELED) {
        panic("abort: the second entry said %ld, not canceled",
          (long)ops[1].status);
    }

    for (spin = 0; spin < 100000 && ab_woke == 1; spin++) {
        sched_yield();
    }

    if (ab_woke == 1) {
        panic("abort: the waiter is still waiting for a signal that "
          "will never come");
    }
    if (ab_woke != SYS_ERR_OWNER_DEAD) {
        panic("abort: the waiter was released with %d, not OWNER_DEAD",
          ab_woke);
    }
    cnputs("    a cancelled signal releases its waiter    OK\n");

    expect("giving the handle back", handle_close(&p->p_handles, h),
      SYS_OK);
}

/*
 * Section 17.4: an entry whose target is what an earlier entry produced.
 *
 * The pair is the one the design uses to explain itself. A walk answers
 * with a connection in the message handle, and a read is sent on that
 * connection: two operations where the second cannot be written down
 * until the first has answered. Before this the second call had to be
 * made by the caller, because there was nowhere for the first answer to
 * live. Now it lives in a register and both go in one submission.
 */
static void
substitution_checks(void)
{
    static struct sys_op_send_args walk, read;
    static sys_op_t ops[2];
    struct sys_op_connect_args cargs;
    fs_request_t *req;
    fs_reply_t *rep;
    sys_handle_t dir;
    long r;

    cnputs("substitution: an entry takes its target from a register\n");

    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = FS_NODE_ROOT;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a connection to the root directory",
      run(SYS_OP_CHAN_CONNECT, h_fs_recv, &cargs, sizeof(cargs)), SYS_OK);
    dir = cargs.out_handle;

    /*
     * Entry one walks and says where to put what comes back. Entry two
     * reads and says where to get its target from. Neither names the
     * other; they agree on a register number and that is the whole of
     * the dependency.
     */
    memset(&walk, 0, sizeof(walk));
    req = (fs_request_t *)walk.request.inline_data;
    req->fr_op = FS_OP_WALK;
    req->fr_name[0] = 'm';
    walk.request.inline_len = sizeof(*req);

    memset(&read, 0, sizeof(read));
    req = (fs_request_t *)read.request.inline_data;
    req->fr_op = FS_OP_READ;
    req->fr_offset = 0;
    req->fr_length = 8;
    read.request.inline_len = sizeof(*req);

    memset(ops, 0, sizeof(ops));
    ops[0].opcode = SYS_OP_IPC_SEND;
    ops[0].target = dir;
    ops[0].flags = SYS_OP_RESULT_TO_REG;
    ops[0].reg_out = 0;
    ops[0].arg_ptr = (uintptr_t)&walk;
    ops[0].arg_len = sizeof(walk);

    ops[1].opcode = SYS_OP_IPC_SEND;
    ops[1].flags = SYS_OP_TARGET_FROM_REG;
    ops[1].reg_in = 0;
    ops[1].arg_ptr = (uintptr_t)&read;
    ops[1].arg_len = sizeof(read);

    r = run_batch(ops, 2);
    if (r != SYS_OK) {
        panic("substitution: the pair returned %ld", r);
    }
    rep = (fs_reply_t *)read.reply.inline_data;
    if (rep->fr_status != SYS_OK || rep->fr_length != 8) {
        panic("substitution: the read said %ld and %lu bytes",
          (long)rep->fr_status, (unsigned long)rep->fr_length);
    }
    cnputs("    a walk and a read in one submission      OK\n");

    /*
     * And the descriptor came back as it went in. The kernel writes
     * one field of an entry, which the interface says in as many
     * words, so a substituted target lives on a copy and never in the
     * caller's array. A prepared program of section 17.7 is the case
     * that makes this more than tidiness: it is validated once and
     * executed many times from one stored copy, and a kernel that
     * edits it has made the second execution a different program.
     */
    if (ops[1].target != SYS_HANDLE_INVALID || ops[1].reg_in != 0) {
        panic("substitution: the kernel wrote into the entry");
    }
    cnputs("    the entry is unchanged but for status    OK\n");

    /*
     * A walk for a name that is not there, which is now two things at
     * once and used to be one.
     *
     * The send succeeds as transport and the server refuses the
     * request, and section 12.4's status field is what lets the kernel
     * tell those apart. The entry fails with what the server said,
     * which it could not do before this stage: the refusal lived in
     * the payload and the kernel does not read payloads.
     *
     * And no handle moved, so the register is still empty, which is
     * section 17.4's rule holding underneath. The entry that depends
     * on it is refused on the tag rather than looking up handle zero.
     * These two protections are independent and both are wanted: the
     * second is what held while the first did not exist.
     */
    req = (fs_request_t *)walk.request.inline_data;
    req->fr_name[0] = 'z';
    memset(&read.reply, 0, sizeof(read.reply));
    r = run_batch(ops, 2);
    expect("a walk for a name that is not there",
      (sys_err_t)r, SYS_ERR_NO_ENT);
    if (ops[0].status != SYS_ERR_NO_ENT) {
        panic("substitution: the walk said %ld, not what the server did",
          (long)ops[0].status);
    }
    if (ops[1].status != SYS_ERR_INVALID_ARG) {
        panic("substitution: the read said %ld, not refused on the tag",
          (long)ops[1].status);
    }
    if (read.reply.inline_len != 0) {
        panic("substitution: the read went on an empty register");
    }
    cnputs("    the kernel sees a server's refusal       OK\n");

    /*
     * And a refusal that is not about a name. The server answers an
     * operation it does not implement with INVALID_ARG, per section
     * 25.3, and the kernel sees that the same way it sees a missing
     * file: what it reads is the field, not the reason.
     */
    req->fr_op = 0xffff;
    r = run_batch(ops, 1);
    expect("an operation the server does not implement",
      (sys_err_t)r, SYS_ERR_INVALID_ARG);
    req->fr_op = FS_OP_WALK;
    req->fr_name[0] = 'm';

    /*
     * And the same walk at the head of a chain, which is section
     * 17.5's abort-on-failure and needed the field above to exist.
     *
     * The walk fails, so everything linked behind it is cancelled
     * rather than run and refused one at a time. The difference is
     * visible in the statuses: CANCELED says an entry was never
     * attempted, and the register being empty would have said only
     * that this particular entry could not find its target.
     */
    req->fr_name[0] = 'z';
    ops[0].flags |= SYS_OP_LINK;
    ops[1].flags |= SYS_OP_LINK;
    memset(&read.reply, 0, sizeof(read.reply));

    {
        sys_op_t three[3];

        memcpy(three, ops, sizeof(ops));
        three[2] = ops[1];
        three[2].flags = SYS_OP_TARGET_FROM_REG;

        r = run_batch(three, 3);
        expect("a chain whose walk found nothing",
          (sys_err_t)r, SYS_ERR_NO_ENT);

        if (three[1].status != SYS_ERR_CANCELED ||
          three[2].status != SYS_ERR_CANCELED) {
            panic("substitution: the chain said %ld and %ld, not canceled",
              (long)three[1].status, (long)three[2].status);
        }
        if (read.reply.inline_len != 0) {
            panic("substitution: a cancelled entry sent its message");
        }
    }
    cnputs("    and a refusal cancels the rest of it     OK\n");

    ops[0].flags &= ~(uint16_t)SYS_OP_LINK;
    ops[1].flags &= ~(uint16_t)SYS_OP_LINK;
    req->fr_name[0] = 'm';

    /*
     * And the same second entry on its own. A fresh execution has a
     * fresh file, so the register it names holds nothing, and it is
     * refused before the message goes rather than sending on a handle
     * that was never there.
     *
     * This is also the end of what stage one could only check
     * directly: that an execution starts with nothing in it, seen
     * through the batch path rather than through the initialiser.
     */
    memset(&read.reply, 0, sizeof(read.reply));
    r = run_batch(&ops[1], 1);
    expect("the same entry with nothing in the register",
      (sys_err_t)r, SYS_ERR_INVALID_ARG);
    if (read.reply.inline_len != 0) {
        panic("substitution: a refused entry sent its message anyway");
    }
    cnputs("    nothing carried over from the last one   OK\n");

    /*
     * A register outside the file, refused before the operation runs
     * for the same reason: an entry that sends and then finds it has
     * nowhere to put the answer has already sent.
     */
    ops[0].reg_out = SYS_BATCH_REGS;
    expect("a result register outside the file",
      (sys_err_t)run_batch(&ops[0], 1), SYS_ERR_INVALID_ARG);
    ops[0].reg_out = 0;

    /*
     * And an operation that produces no handle to put anywhere.
     * Section 17.4 names the producers; the rest are refused by name
     * rather than handed whatever they left in their arguments.
     */
    {
        sys_op_t nop;

        memset(&nop, 0, sizeof(nop));
        nop.opcode = SYS_OP_NOP;
        nop.flags = SYS_OP_RESULT_TO_REG;
        expect("a result register on an operation with no result",
          (sys_err_t)run_batch(&nop, 1), SYS_ERR_NOT_SUPPORTED);
    }

    expect("giving the directory back",
      handle_close(&proc_current()->p_handles, dir), SYS_OK);
}

/*
 * What a process costs in frames, and how many of them come back.
 *
 * Section 1 has said since M9 that memory is never reclaimed, and no
 * milestone has said how much. The frame allocator reports how many are
 * in use and attributes none of them, so "a process holds frames" has
 * been a sentence rather than a number.
 *
 * Read either side of a spawn and either side of the reap, which are
 * different moments: a process that has exited still exists while
 * anything can ask about it, and M11 made the reference count what
 * decides. What this stage establishes is the first number, and that
 * the second is zero.
 *
 * The two tiers give different first numbers, and the difference is the
 * milestone's shape. At Tier 0 an image is copied to the address it was
 * linked for and runs there, in memory the archive already occupies, so
 * a process costs no frames and there is nothing about it to reclaim. At
 * Tier 2 the loader allocates and maps the whole slot, which is a
 * megabyte whatever the program's size, and the tables to reach it.
 * Every frame this milestone can give back is a Tier 2 frame.
 *
 * The program is brief, which needs no namespace and does nothing at
 * all: what is being measured is what a process costs, so a process
 * that had to be handed connections first would be measuring the
 * handing as well, and one that computed for eighty milliseconds would
 * be measuring the computing. It was spin until M14 stage five found
 * nine of those spins between the last check and the shell, which is
 * most of a second in front of a session that feeds a line every two.
 */
static uint32_t
frame_cost_once(const char *image, uint32_t *returned)
{
    struct child c;
    uint32_t before, spawned, reaped;
    sys_time_t deadline;
    sys_id_t id;
    int32_t code;

    before = page_used();

    /*
     * Both reads happen before the child is let go, which is what the
     * defer is for. Spinning up a program that allocated nothing made
     * this look right either way, and the measurement is meant to be
     * of the loading and not of whatever the program then does.
     */
    sched_defer();
    child_watch(&c, proc_spawn_image(image, 20, NULL, 0, NULL, 0));
    id = c.c_proc->p_id;
    spawned = page_used();

#if SYS_TIER == 2
    /*
     * The same number reached the other way, per M14 stage six. What
     * the allocator says a spawn took and what the process is holding
     * are two measurements of one thing, taken through the frame count
     * and through a walk of the process's own address space, and
     * neither is derived from the other. A loader that mapped a page
     * twice, or allocated one it did not map, would move one and not
     * the other.
     */
    {
        uint32_t walked = proc_frames(c.c_proc);

        if (walked != spawned - before) {
            panic("frames: %s took %u frames and is holding %u",
              image, (unsigned int)(spawned - before),
              (unsigned int)walked);
        }
    }
#endif

    sched_undefer();

    code = child_join(&c, JOIN_MS);
    if (code != 0) {
        panic("frames: %s exited %d", image, code);
    }

    /*
     * The join is not the reap, and reading the count straight after
     * one reads it too early. Waiting on the completion timeline says
     * the process has exited and closing the handles says nothing can
     * ask about it any more, but its last thread is parked at that
     * point, not collected: thread_reap_parked runs at the top of the
     * next switch, and proc_reap runs from there. Reading the count
     * after the join reads it before any of that has happened.
     *
     * The slot going back is the reap, exactly, so the identifier going
     * stale is the event to wait for. It is taken before the join
     * because afterwards this pointer names a slot that may already
     * have been handed to somebody else.
     */
    deadline = time_now() + time_from_ms(JOIN_MS);
    while (pool_lookup(proc_pool(), id) != NULL) {
        if (time_now() > deadline) {
            panic("frames: it exited and was never reaped");
        }
        sched_yield();
    }

    reaped = page_used();

    /*
     * Frames coming back is the count falling. A count that rose across
     * the reap is not a negative number of frames returned, which is
     * what subtracting the other way round would print: it is something
     * having allocated while this was watching, and the measurement
     * saying so is better than it wrapping to four billion.
     */
    if (reaped > spawned) {
        panic("frames: %u more in use after the reap than before it",
          (unsigned int)(reaped - spawned));
    }

    *returned = spawned - reaped;

    if (spawned < before) {
        panic("frames: the count fell across a spawn");
    }

    return spawned - before;
}

static void
frame_cost_checks(void)
{
    uint32_t first, second, back, back2;

    cnputs("frames: what a process costs and what comes back\n");

    /*
     * Twice, because page_used() is the whole system's count and not
     * this process's: anything else that allocated inside the window
     * would be attributed to the spawn, and on a Tier 2 machine the
     * other core is running while this measures. Two spawns that cost
     * different amounts is that interference showing itself. Two that
     * agree are not proof it cannot happen, but they are the difference
     * between a number that was measured and a number that was read
     * once.
     *
     * It is also the count/duration rule of M9 applied to itself. A
     * count is asserted because it is the same in every run; measuring
     * it twice in one run is the cheapest test of that claim there is.
     */
    first = frame_cost_once("brief", &back);
    second = frame_cost_once("brief", &back2);

    cnprintf("    a process costs %u frames, %u came back\n",
      (unsigned int)first, (unsigned int)back);

#if SYS_TIER == 2
    {
        const struct boot_member *bm = boot_lookup("brief");

        /*
         * Broken out, because the two halves answer to different
         * things. What the image needs is the archive's number and
         * changes when the program does; what the address space costs
         * is the port's and changes when the table shape does.
         */
        cnprintf("    %u of them are what its image needs, %u are the "
          "space it runs in\n",
          (unsigned int)(bm->bm_mem / PAGE_SIZE),
          (unsigned int)(first - bm->bm_mem / PAGE_SIZE));
    }
#endif

    if (second != first) {
        panic("frames: the same program cost %u then %u",
          (unsigned int)first, (unsigned int)second);
    }
    if (back2 != back) {
        panic("frames: %u came back, then %u",
          (unsigned int)back, (unsigned int)back2);
    }

    /*
     * The cost is a count. It is what the image needs rounded to pages,
     * plus a page for the startup record and whatever the tier's tables
     * take, and none of that depends on the host, so it is the same in
     * every run and may be asserted. The exact figure differs by
     * architecture because the table shapes do, which is why the floor
     * is read from the archive and not written down here.
     *
     * The floor was a slot until M14 stage two, which is what a process
     * cost when the loader mapped one whatever the image was. Comparing
     * against the image is the same check asking the same question of a
     * loader that now knows the answer.
     */
#if SYS_TIER == 0
    if (first != 0) {
        panic("frames: a flat-memory process cost %u frames",
          (unsigned int)first);
    }
#else
    {
        const struct boot_member *m = boot_lookup("brief");
        uint32_t needs = m != NULL ? m->bm_mem / PAGE_SIZE : 0;

        if (needs == 0) {
            panic("frames: the archive says nothing about brief");
        }
        if (first < needs) {
            panic("frames: a process cost %u frames, fewer than the %u "
              "its image needs", (unsigned int)first, (unsigned int)needs);
        }
    }
#endif

    /*
     * And what came back, asserted and not merely printed. The run of
     * processes below measures the residue and then checks the residue,
     * which is a shape that cannot fail on the thing it is for: with
     * the freeing taken out it measures a residue of 264, finds 264 of
     * them eight times over, and says nothing. This is the assertion
     * that does fail. A slot's worth is the claim stage two makes, and
     * the frames beyond it are the address space, which is stage
     * three's and not returned yet.
     */
#if SYS_TIER == 0
    if (back != 0) {
        panic("frames: %u came back from a process that cost nothing",
          (unsigned int)back);
    }
#else
    if (back != first) {
        panic("frames: %u of the %u a process cost came back",
          (unsigned int)back, (unsigned int)first);
    }
#endif
}

/*
 * And what many of them cost, which is the difference between
 * reclaiming and freeing once.
 *
 * A reap that gave back the frames of the process in front of it and
 * not the one behind would pass the check above and still be a system
 * bounded by how many processes it has ever started. What says
 * otherwise is the count after a run of them: it climbs by what a
 * process holds that this stage does not return yet, once per process,
 * and by nothing else.
 *
 * The residue is measured rather than written down, because it is the
 * address space and its tables, which stage three returns and which
 * differ between the two ports. What is asserted here is that the
 * residue is all there is: at Tier 0 it is zero because a process
 * costs nothing, and at Tier 2 it is the same small number every time
 * round rather than the 264 a process costs.
 */
#define FRAME_ROUNDS    8u

/*
 * How many times stage five spawns and reaps. Chosen so that the frames
 * the run asks for exceed the frames the machine has, which is the
 * claim, and so that the check still fits in a boot.
 */
#define FRAME_CYCLES    2400u

/*
 * How many frames the ledger at the end of the boot cannot attribute to
 * anything. Asserted so that a change to it is something a reader is
 * told about; see frame_ledger().
 */
#define FRAME_UNCLAIMED 0u

static void
frame_return_checks(void)
{
    uint32_t before, after, cost, back, residue, grew, i;

    cnputs("frames: what a run of processes leaves behind\n");

    cost = frame_cost_once("brief", &back);
    residue = cost - back;

    before = page_used();
    for (i = 0; i < FRAME_ROUNDS; i++) {
        (void)frame_cost_once("brief", &back);
    }
    after = page_used();

    if (after < before) {
        panic("frames: a run of processes gave back more than it took");
    }

    grew = after - before;

    cnprintf("    %u processes, one after another, held %u frames, %u each\n",
      (unsigned int)FRAME_ROUNDS, (unsigned int)grew,
      (unsigned int)residue);

    /*
     * Zero, and not the residue this run measured. Checking a run
     * against its own measurement is the shape that cannot fail: with
     * the freeing taken out it measures a residue of 264 and finds 264
     * of them eight times over. Since stage three there is no residue
     * to allow for, so the number to assert is the one the system does
     * not get to choose.
     */
    if (grew != 0) {
        panic("frames: %u processes came and went and held %u frames",
          (unsigned int)FRAME_ROUNDS, (unsigned int)grew);
    }
}

/*
 * Whether the system can now run indefinitely, per M13 stage 5.
 *
 * The question the milestone opened with was how long this system can
 * run, and the answer was that it is bounded by how many processes it
 * has ever started: a process took frames and nothing gave them back,
 * so the machine had a lifetime measured in spawns. Stages two and
 * three made the frames come back. This is the stage that spends them.
 *
 * A program is spawned and reaped more times than the machine has
 * frames, and the count is read at either end. What that demonstrates
 * is not that the arithmetic in the earlier stages was right, which the
 * earlier checks already assert, but that it stays right: a reclamation
 * that returned all but one frame each time round would pass every
 * check before this one and still be a machine with a lifetime.
 *
 * The program is brief, which does nothing. What a cycle costs is then
 * what a spawn and a reap cost, and the figure below is about this
 * mechanism rather than about how long some program takes to run.
 *
 * The duration is reported and not asserted, per M9's rule: how long
 * this takes is a property of the host. The count of iterations, the
 * frames each one moves, and the count at either end are properties of
 * the kernel, and those are asserted.
 */
static void
frame_endurance_checks(void)
{
    uint32_t before, after, i;
    uint32_t cost, back;
    uint64_t t0, t1;

    cnputs("frames: a program run more times than the machine has frames\n");

    cost = frame_cost_once("brief", &back);

    exit_quiet = true;

    before = page_used();
    t0 = time_now();

    for (i = 0; i < FRAME_CYCLES; i++) {
        (void)frame_cost_once("brief", &back);
    }

    t1 = time_now();

    /*
     * Waited for rather than snapped, for the reason stage four's
     * review settled on: the count is the whole machine's, and an
     * assertion that it is exactly what it was is an assertion that
     * nothing else moved a frame in the instant this looked. The last
     * cycle's reap is the one thing certain to be in flight, since
     * the join is not the reap.
     */
    {
        sys_time_t deadline = time_now() + time_from_ms(JOIN_MS);

        while ((after = page_used()) != before) {
            if (time_now() > deadline) {
                break;
            }
            sched_yield();
        }
    }

    exit_quiet = false;

    cnprintf("    %u cycles of %u frames, %lu in all against %u the "
      "machine has\n", (unsigned int)FRAME_CYCLES, (unsigned int)cost,
      (unsigned long)((uint64_t)FRAME_CYCLES * cost),
      (unsigned int)page_count());
    /*
     * Above the compared line and never asserted, because how long a
     * run of spawns takes is a property of the host: M9's rule, and
     * M12 had to relearn it.
     */
    cnprintf("    took %lu ms, a number of the host and not of the "
      "kernel\n", (unsigned long)((t1 - t0) * 1000ULL / time_hz()));

    if (after != before) {
        panic("frames: %u cycles left the count at %u, not %u",
          (unsigned int)FRAME_CYCLES, (unsigned int)after,
          (unsigned int)before);
    }

    /*
     * And that the run was worth making. A cycle that cost nothing
     * would leave the count where it started for the wrong reason, and
     * at Tier 0 that is exactly what happens: a process costs no frames
     * there, so what this stage proves is a Tier 2 claim and the tier
     * with flat memory has nothing to prove.
     */
#if SYS_TIER == 2
    if ((uint64_t)FRAME_CYCLES * cost <= (uint64_t)page_count()) {
        panic("frames: %u cycles of %u never exceeded the machine's %u",
          (unsigned int)FRAME_CYCLES, (unsigned int)cost,
          (unsigned int)page_count());
    }
#endif
}

/*
 * Section 17.4: the registers one execution has, and what they refuse.
 *
 * Checked directly rather than through a submission, because at this
 * stage nothing writes a register from an entry: the file exists and
 * the entries that use it come next. What can be established here is
 * every property the rest of the milestone will rest on, and the
 * refusals are the properties. A file that returns a value for a
 * register nobody wrote, or hands back an integer as a handle, is the
 * whole of what section 17.4 says must not happen.
 */
static void
register_checks(void)
{
    struct batch_regs r;
    sys_handle_t h;
    uint64_t v;
    unsigned int i;

    cnputs("registers: what one entry can hand the next, section 17.4\n");

    regs_init(&r);

    /*
     * Empty is a state, and not the number zero. Every register starts
     * in it and reads of either kind are refused, which is what makes
     * a batch that names a register an earlier entry never wrote fail
     * instead of proceeding on a zero.
     */
    for (i = 0; i < SYS_BATCH_REGS; i++) {
        uint8_t tag;

        if (regs_peek(&r, i, &tag) != SYS_OK || tag != REG_EMPTY) {
            panic("registers: register %u was not empty at the start", i);
        }
        if (regs_get_handle(&r, i, &h) != SYS_ERR_INVALID_ARG ||
          regs_get_scalar(&r, i, &v) != SYS_ERR_INVALID_ARG) {
            panic("registers: empty register %u was readable", i);
        }
    }
    cnprintf("    %u registers, all of them empty          OK\n",
      (unsigned int)SYS_BATCH_REGS);

    /* A scalar goes in and comes back, and is not a handle. */
    expect("writing a scalar", regs_put_scalar(&r, 1, 0x5eed), SYS_OK);
    v = 0;
    expect("reading it back", regs_get_scalar(&r, 1, &v), SYS_OK);
    if (v != 0x5eed) {
        panic("registers: a scalar came back as %lu", (unsigned long)v);
    }
    expect("reading that scalar as a handle",
      regs_get_handle(&r, 1, &h), SYS_ERR_INVALID_ARG);
    cnputs("    a scalar is not a handle                 OK\n");

    /* And a handle goes in and comes back, and is not a scalar. */
    expect("writing a handle", regs_put_handle(&r, 2, (sys_handle_t)0x99),
      SYS_OK);
    h = SYS_HANDLE_INVALID;
    expect("reading it back", regs_get_handle(&r, 2, &h), SYS_OK);
    if (h != (sys_handle_t)0x99) {
        panic("registers: a handle came back as %lu", (unsigned long)h);
    }
    expect("reading that handle as a scalar",
      regs_get_scalar(&r, 2, &v), SYS_ERR_INVALID_ARG);
    cnputs("    a handle is not a scalar                 OK\n");

    /*
     * A register that is written again is what it was written as, not
     * what it was. Otherwise a batch could put a handle somewhere and
     * a later entry could turn it into a number that still reads as a
     * handle.
     */
    expect("writing over a handle with a scalar",
      regs_put_scalar(&r, 2, 7), SYS_OK);
    expect("and reading it as a handle",
      regs_get_handle(&r, 2, &h), SYS_ERR_INVALID_ARG);
    cnputs("    written again is written again           OK\n");

    /*
     * An index outside the file is refused and not folded back into
     * it, because a program that names register nine has a bug and a
     * kernel that quietly gives it register one hides it.
     */
    expect("writing outside the file",
      regs_put_scalar(&r, SYS_BATCH_REGS, 1), SYS_ERR_INVALID_ARG);
    expect("reading outside the file",
      regs_get_scalar(&r, SYS_BATCH_REGS, &v), SYS_ERR_INVALID_ARG);
    {
        uint8_t tag = REG_HANDLE;

        expect("asking what is outside the file",
          regs_peek(&r, SYS_BATCH_REGS, &tag), SYS_ERR_INVALID_ARG);
    }
    cnputs("    an index outside the file is refused     OK\n");

    /*
     * And an execution starts with nothing from the one before it.
     * This is the property that makes the abort rule of section 17.5
     * enumerable: a register is either written by an entry of this
     * batch or it is empty, with no third case where it holds
     * something older.
     */
    regs_init(&r);
    for (i = 0; i < SYS_BATCH_REGS; i++) {
        uint8_t tag;

        if (regs_peek(&r, i, &tag) != SYS_OK || tag != REG_EMPTY) {
            panic("registers: register %u survived an execution", i);
        }
    }
    cnputs("    nothing survives into the next execution OK\n");
}

/*
 * Section 17.5: chaining, and what a chain does when an entry fails.
 *
 * The entries signal a timeline for the same reason as the stage
 * before: a counter that only goes up says exactly which of them ran.
 */
static void
link_checks(void)
{
    static sys_op_fence_args_t args[5];
    static sys_op_t ops[5];
    struct proc *p = proc_current();
    struct timeline *tl;
    sys_handle_t h;
    uint64_t base;
    uint32_t i;
    sys_err_t err;

    cnputs("chaining: abort-on-error grouping, section 17.5\n");

    tl = timeline_create("link");
    if (tl == NULL) {
        panic("link: no timeline");
    }
    err = handle_insert(&p->p_handles, tl, SYS_OBJ_TIMELINE,
      SYS_RIGHT_SIGNAL | SYS_RIGHT_WAIT, &h);
    expect("a signal handle for the chain", err, SYS_OK);

    /*
     * Five entries: a chain of three whose middle one fails, then one
     * unlinked entry, then another. The middle one fails by signalling
     * a value the timeline has already passed, which section 9.1 makes
     * an error and which needs no broken handle to arrange.
     */
    for (i = 0; i < 5; i++) {
        memset(&args[i], 0, sizeof(args[i]));
        memset(&ops[i], 0, sizeof(ops[i]));
        ops[i].opcode = SYS_OP_FENCE_SIGNAL;
        ops[i].target = h;
        ops[i].arg_ptr = (uintptr_t)&args[i];
        ops[i].arg_len = sizeof(args[i]);
    }

    base = timeline_value(tl);
    args[0].value = base + 1;       /* runs */
    args[1].value = base;           /* backwards: fails */
    args[2].value = base + 2;       /* cancelled, so never runs */
    args[3].value = base + 2;       /* an independent entry, runs */
    args[4].value = base + 3;       /* and another */

    ops[0].flags = SYS_OP_LINK;
    ops[1].flags = SYS_OP_LINK;
    /* ops[2] ends the chain; ops[3] and ops[4] are on their own. */

    expect("a batch with a chain of three in it",
      (sys_err_t)run_batch(ops, 5), SYS_ERR_INVALID_ARG);

    if (ops[0].status != SYS_OK) {
        panic("chaining: the first entry did not run");
    }
    if (ops[1].status != SYS_ERR_INVALID_ARG) {
        panic("chaining: the failing entry said %d", (int)ops[1].status);
    }
    if (ops[2].status != SYS_ERR_CANCELED) {
        panic("chaining: the rest of the chain said %d",
          (int)ops[2].status);
    }
    if (ops[3].status != SYS_OK || ops[4].status != SYS_OK) {
        panic("chaining: an independent entry was cancelled with it");
    }
    cnputs("    the rest of the chain was cancelled    OK\n");
    cnputs("    and an entry outside it still ran      OK\n");

    /*
     * And the counter says which of them happened: the first, then the
     * two after the chain, and not the cancelled one.
     */
    if (timeline_value(tl) != base + 3) {
        panic("chaining: the counter reached %lu, not %lu",
          (unsigned long)timeline_value(tl), (unsigned long)(base + 3));
    }
    cnprintf("    three of five ran, counter at %lu      OK\n",
      (unsigned long)timeline_value(tl));

    /*
     * Section 17.5 says chains are not graphs and that a join is
     * expressed with a fence instead. Two producers advancing a
     * timeline and a consumer waiting on the final value is that join,
     * and it is a thing LINK cannot say: there is no flag arrangement
     * that makes one entry wait for two.
     */
    base = timeline_value(tl);
    args[0].value = base + 1;
    args[1].value = base + 2;
    memset(&args[2], 0, sizeof(args[2]));
    args[2].value = base + 2;

    /*
     * A deadline rather than never, and it is not a timing assertion:
     * the value is already reached when this entry runs, so the wait
     * returns at once and the deadline is only reached if the entries
     * ran out of order. Waiting for ever would make that failure a
     * hang, and this milestone has said twice that a check whose
     * failure mode is a hang is a check nobody has read.
     */
    args[2].deadline = time_now() + time_from_ms(30000);
    ops[0].flags = 0;
    ops[1].flags = 0;
    ops[2].flags = 0;
    ops[2].opcode = SYS_OP_FENCE_WAIT;

    expect("a join written as two producers and a wait",
      (sys_err_t)run_batch(ops, 3), SYS_OK);
    cnputs("    which LINK has no way to express       OK\n");

    expect("giving the handle back", handle_close(&p->p_handles, h),
      SYS_OK);
}


/*
 * Section 13.3 on batches: the bound does not appear in interrupt
 * latency and the cost of one entry does.
 *
 * That sentence has been in the design since before there was a batch,
 * and until the count could exceed one there was no way to find out
 * whether it was true. It rests entirely on the batch being
 * interruptible at every entry boundary.
 */

/*
 * The milestone's question: does expressing a set of operations as one
 * submission beat submitting them one at a time?
 *
 * Answered in counts. A duration comparing two ways of doing the same
 * work would be a figure about this host, and M9 spent six stages
 * establishing that. The same eight signals go through the same entry
 * point both ways, so the work either side of the comparison is the
 * same work and the check can say why: the timeline advances by eight
 * whichever form is used.
 */
#define PAY_N   8


/*
 * Section 17.7: a program validated once and executed many times.
 *
 * A different claim from batching. Batching amortises the cost of a
 * call over several operations; this amortises the cost of validating
 * over several executions of the same operations. The two save
 * different things and stage four measured only the first.
 */
#define PREP_N      4
#define PREP_TIMES  4

static void
prepared_checks(void)
{
    static sys_op_fence_args_t args[PREP_N];
    static sys_op_t ops[PREP_N];
    struct proc *p = proc_current();
    sys_op_prog_args_t pargs;
    struct timeline *tl;
    sys_handle_t h, prog;
    uint64_t base, k0, c0, batched, prepared;
    uint32_t i;
    sys_err_t err;

    cnputs("programs: validated once, executed many times\n");

    tl = timeline_create("prepared");
    if (tl == NULL) {
        panic("prepared: no timeline");
    }
    err = handle_insert(&p->p_handles, tl, SYS_OBJ_TIMELINE,
      SYS_RIGHT_SIGNAL, &h);
    expect("a signal handle for the program", err, SYS_OK);

    /*
     * The same four signals run four times as an ordinary batch, so
     * that there is something to compare against. The entries have to
     * be refilled between runs because a timeline only goes up and the
     * same values twice would be an error the second time.
     */
    (void)submit_crossings(&k0);
    for (i = 0; i < PREP_TIMES; i++) {
        base = timeline_value(tl);
        batch_fill_on(ops, args, h, PREP_N, base);
        if (run_batch(ops, PREP_N) != SYS_OK) {
            panic("prepared: a plain batch failed");
        }
    }
    (void)submit_crossings(&batched);
    batched -= k0;

    /*
     * And now as a program. The entries are fixed at load, so the
     * values they signal are fixed too, which is why this one counts
     * up from where the batches left off and then re-signals the same
     * values on later executions. Those later signals fail, and that
     * is expected and is not what is being measured: what is being
     * measured is how many checks the executions cost.
     */
    base = timeline_value(tl);
    batch_fill_on(ops, args, h, PREP_N, base);

    memset(&pargs, 0, sizeof(pargs));
    pargs.entries = (uintptr_t)ops;
    pargs.count = PREP_N;

    /*
     * The loading is inside the window, because it is part of what
     * this form costs. Leaving it out would compare the executions of
     * one form against the whole of the other, which is how a
     * mechanism comes to look free.
     */
    (void)submit_crossings(&c0);

    expect("loading a program", run(SYS_OP_PROG_LOAD, 0, &pargs,
      sizeof(pargs)), SYS_OK);
    prog = pargs.out_handle;
    for (i = 0; i < PREP_TIMES; i++) {
        (void)run(SYS_OP_PROG_EXEC, prog, NULL, 0);
    }
    (void)submit_crossings(&prepared);
    prepared -= c0;

    cnprintf("    %u entries, %u times as a batch:   %lu checks\n",
      (unsigned int)PREP_N, (unsigned int)PREP_TIMES,
      (unsigned long)batched);
    cnprintf("    loaded once and run %u times:      %lu checks\n",
      (unsigned int)PREP_TIMES, (unsigned long)prepared);

    /*
     * A batch pays for the array and for looking at every entry, every
     * time. A program pays for the entries when it runs them and for
     * nothing else, because the array is the kernel's and the opcodes
     * were checked at load.
     */
    if (batched != PREP_TIMES * (1 + PREP_N * 2)) {
        panic("prepared: %lu checks for the batches, not %u",
          (unsigned long)batched,
          (unsigned int)(PREP_TIMES * (1 + PREP_N * 2)));
    }
    /*
     * Loading costs one check for the operation that asks for it, one
     * for the array, and one per entry, paid once. Each execution
     * costs one for the operation and one per entry as they run. The
     * operation asking is itself an entry and pays like one, which is
     * section 17.7's point about there being one implementation.
     */
    if (prepared != (2 + PREP_N) + PREP_TIMES * (1 + PREP_N)) {
        panic("prepared: %lu checks for load and executions, not %u",
          (unsigned long)prepared,
          (unsigned int)((2 + PREP_N) + PREP_TIMES * (1 + PREP_N)));
    }
    cnprintf("    validating once saved %lu of them\n",
      (unsigned long)(batched - prepared));

    /*
     * And a handle that has gone stale since the program was loaded.
     * The entries name it and it is looked up at every execution, so
     * this is refused when it runs rather than running with whatever
     * the slot now holds.
     */
    expect("closing the handle the program names",
      handle_close(&p->p_handles, h), SYS_OK);
    expect("executing it afterwards",
      run(SYS_OP_PROG_EXEC, prog, NULL, 0), SYS_ERR_STALE);
    cnputs("    a stale handle is refused at execution  OK\n");

    expect("giving the program back",
      handle_close(&p->p_handles, prog), SYS_OK);

    /*
     * And more programs than the pool holds, loaded and given back one
     * at a time. A program is its handle and nothing else names one,
     * so closing it has to return the slot; it did not, and the fifth
     * load was refused for want of room nothing was using.
     */
    for (i = 0; i < SYS_MAX_PREPARED * 2; i++) {
        memset(&pargs, 0, sizeof(pargs));
        pargs.entries = (uintptr_t)ops;
        pargs.count = PREP_N;
        if (run(SYS_OP_PROG_LOAD, 0, &pargs, sizeof(pargs)) != SYS_OK) {
            panic("prepared: load %u of %u ran out of slots",
              (unsigned int)i, (unsigned int)(SYS_MAX_PREPARED * 2));
        }
        if (handle_close(&p->p_handles, pargs.out_handle) != SYS_OK) {
            panic("prepared: could not give program %u back",
              (unsigned int)i);
        }
    }
    cnprintf("    %u loaded and given back, %u slots      OK\n",
      (unsigned int)(SYS_MAX_PREPARED * 2),
      (unsigned int)SYS_MAX_PREPARED);
}

static void
pays_checks(void)
{
    static sys_op_fence_args_t args[PAY_N];
    static sys_op_t ops[PAY_N];
    struct proc *p = proc_current();
    struct timeline *tl;
    sys_handle_t h;
    uint64_t one_calls, one_checks, one_locks;
    uint64_t few_calls, few_checks, few_locks;
    uint64_t base, c0, k0, l0;
    uint32_t i;
    sys_err_t err;

    cnputs("batches: whether one submission beats several\n");

    tl = timeline_create("pays");
    if (tl == NULL) {
        panic("pays: no timeline");
    }
    err = handle_insert(&p->p_handles, tl, SYS_OBJ_TIMELINE,
      SYS_RIGHT_SIGNAL, &h);
    expect("a signal handle for the comparison", err, SYS_OK);

    /* Eight signals, one call each. */
    base = timeline_value(tl);
    batch_fill_on(ops, args, h, PAY_N, base);
    c0 = submit_crossings(&k0);
    l0 = spin_take_count();
    for (i = 0; i < PAY_N; i++) {
        if (run_batch(&ops[i], 1) != SYS_OK) {
            panic("pays: a single submission failed");
        }
    }
    one_calls = submit_crossings(&one_checks) - c0;
    one_checks -= k0;
    one_locks = spin_take_count() - l0;

    if (timeline_value(tl) != base + PAY_N) {
        panic("pays: the one-at-a-time form did %lu of %u",
          (unsigned long)(timeline_value(tl) - base), (unsigned int)PAY_N);
    }

    /* The same eight signals, one call. */
    base = timeline_value(tl);
    batch_fill_on(ops, args, h, PAY_N, base);
    c0 = submit_crossings(&k0);
    l0 = spin_take_count();
    if (run_batch(ops, PAY_N) != SYS_OK) {
        panic("pays: the batch failed");
    }
    few_calls = submit_crossings(&few_checks) - c0;
    few_checks -= k0;
    few_locks = spin_take_count() - l0;

    if (timeline_value(tl) != base + PAY_N) {
        panic("pays: the batch form did %lu of %u",
          (unsigned long)(timeline_value(tl) - base), (unsigned int)PAY_N);
    }

    cnprintf("    %u signals, one call each: %lu crossings, "
      "%lu checks\n", (unsigned int)PAY_N, (unsigned long)one_calls,
      (unsigned long)one_checks);
    cnprintf("    the same %u in one call:   %lu crossings, "
      "%lu checks\n", (unsigned int)PAY_N, (unsigned long)few_calls,
      (unsigned long)few_checks);

    /*
     * The obvious column and the one M6 said to look at as well. A
     * batch saves crossings, which is what it is for. What it does not
     * save is the per-entry checking, because every entry is still
     * checked before it runs and again when it runs, whichever form
     * carried it.
     */
    /*
     * Both columns asserted and not only printed. These are counts, so
     * they are the same on every host and in all four configurations,
     * and the milestone's product is the numbers: a headline figure
     * that can drift without anything failing is a figure nobody is
     * keeping. Changing how entries are validated changes the second
     * column, and it should have to be looked at and written down
     * again rather than quietly becoming a different claim.
     *
     * Three checks a call: the descriptor array, the entry before it
     * runs, and the entry when it runs. A batch pays the first once.
     */
    if (one_calls != PAY_N || few_calls != 1) {
        panic("pays: %lu crossings against %lu",
          (unsigned long)few_calls, (unsigned long)one_calls);
    }
    if (one_checks != PAY_N * 3 || few_checks != 1 + PAY_N * 2) {
        panic("pays: %lu checks against %lu, not %u and %u",
          (unsigned long)few_checks, (unsigned long)one_checks,
          (unsigned int)(1 + PAY_N * 2), (unsigned int)(PAY_N * 3));
    }
    cnprintf("    it saves %lu crossings and %lu checks\n",
      (unsigned long)(one_calls - few_calls),
      (unsigned long)(one_checks - few_checks));

#if SYS_TIER == 2
    cnprintf("    and %lu of the lock, %lu against %lu\n",
      (unsigned long)(one_locks - few_locks), (unsigned long)one_locks,
      (unsigned long)few_locks);
#else
    (void)one_locks;
    (void)few_locks;
#endif

    expect("giving the handle back", handle_close(&p->p_handles, h),
      SYS_OK);
}

static void
batch_latency_checks(void)
{
    static sys_op_fence_args_t args[SYS_BATCH_MAX_ENTRIES];
    static sys_op_t ops[SYS_BATCH_MAX_ENTRIES];
    struct proc *p = proc_current();
    struct timeline *tl;
    sys_handle_t h;
    uintptr_t site_one, site_many;
    uint64_t one, many, points;
    uint64_t n_one, n_many;
    uint64_t base;
    sys_err_t err;

    cnputs("batches: what a batch does to interrupt latency\n");

    tl = timeline_create("latency");
    if (tl == NULL) {
        panic("latency: no timeline");
    }
    err = handle_insert(&p->p_handles, tl, SYS_OBJ_TIMELINE,
      SYS_RIGHT_SIGNAL, &h);
    expect("a signal handle for the measurement", err, SYS_OK);

    /*
     * The points a batch passes are its entries, exactly. That is a
     * count, so it is the same on every host and in every
     * configuration, and it is what the rest of this rests on: a
     * boundary that is not marked cannot be a boundary anything stops
     * at.
     */
    base = timeline_value(tl);
    batch_fill_on(ops, args, h, SYS_BATCH_MAX_ENTRIES, base);
    points = preempt_points();
    expect("a batch of the largest size",
      (sys_err_t)run_batch(ops, SYS_BATCH_MAX_ENTRIES), SYS_OK);
    points = preempt_points() - points;

    cnprintf("    %u entries passed %lu points            %s\n",
      (unsigned int)SYS_BATCH_MAX_ENTRIES, (unsigned long)points,
      points == SYS_BATCH_MAX_ENTRIES ? "OK" : "WRONG");
    if (points != SYS_BATCH_MAX_ENTRIES) {
        panic("latency: %lu points for %u entries",
          (unsigned long)points, (unsigned int)SYS_BATCH_MAX_ENTRIES);
    }

    /*
     * And now the question. One entry against the largest batch, each
     * measured in its own window, with the site reported so that a
     * region that grew somewhere else is not counted as this one
     * growing.
     */
    /*
     * Nothing else on this core inside the window.
     *
     * The window is the core and not the batch: anything else this
     * core does between the clearing and the reading is in it. That
     * was written down as making the figure an upper bound rather
     * than the batch's cost, and left at that. It was worth more.
     * With the dispatch held off, twelve runs of the sixteen-entry
     * figure on one port sit between 275 and 301 ticks; without it,
     * two of the twelve read 1030 and 1143, which is another thread's
     * work inside the window and nothing to do with the batch. That
     * is what used to push the old assertion against its threshold.
     *
     * It matters more now than it did, because the count below is
     * asserted exactly: one thread dispatched in here would add its
     * regions to these and fail a check about the batch for a reason
     * that has nothing to do with the batch.
     *
     * Deferring holds off the dispatch and not the interrupt, so what
     * is being measured is untouched. A handler is entered with
     * interrupts already masked and opens no region of its own.
     */
    sched_defer();

    base = timeline_value(tl);
    batch_fill_on(ops, args, h, 1, base);
    intr_region_reset();
    (void)run_batch(ops, 1);
    one = intr_region_worst(&site_one);
    n_one = intr_region_count();

    base = timeline_value(tl);
    batch_fill_on(ops, args, h, SYS_BATCH_MAX_ENTRIES, base);
    intr_region_reset();
    (void)run_batch(ops, SYS_BATCH_MAX_ENTRIES);
    many = intr_region_worst(&site_many);
    n_many = intr_region_count();

    sched_undefer();

    cnprintf("    one entry: %lu region, held them off %lu ticks, "
      "from 0x%lx\n", (unsigned long)n_one, (unsigned long)one,
      (unsigned long)site_one);
    cnprintf("    %u entries: %lu regions, held them off %lu ticks, "
      "from 0x%lx\n", (unsigned int)SYS_BATCH_MAX_ENTRIES,
      (unsigned long)n_many, (unsigned long)many,
      (unsigned long)site_many);

    /*
     * No measurement is not a result. A count of zero means nothing
     * was recorded, which happens if this runs before the instrument
     * is armed, and reporting that as the claim holding is how a check
     * comes to pass on no data at all. It did, the first time this was
     * run.
     */
    if (n_one == 0 || n_many == 0) {
        panic("latency: nothing was measured, %lu and %lu regions",
          (unsigned long)n_one, (unsigned long)n_many);
    }

    /*
     * What is asserted is the count and not the durations.
     *
     * The question is whether anything lets interrupts in between one
     * entry and the next. Counting the regions answers it exactly: a
     * batch that is interruptible at every boundary has a region per
     * entry, and one that disables interrupts once for the whole call
     * has one region however many entries are in it. Both figures are
     * the same on every host and in every run.
     *
     * It used to be asserted as a ratio of the two durations, that
     * sixteen entries held interrupts off no more than four times as
     * long as one. That is a duration compared against a threshold,
     * which section 13.3's own rule says never to do, and it was not
     * safe: with eight machines running at once the one-entry figure
     * ranges from 48 to 86 ticks and the sixteen-entry figure from 45
     * to 204, and a run at 231 against 54 failed the build. The
     * comment that stood here claimed the margin was an order of
     * magnitude wider than the effect. It was about a fifth of it.
     *
     * The durations are still printed, because what the regions cost
     * is worth seeing, and they are not asserted on.
     */
    if (n_many == n_one * SYS_BATCH_MAX_ENTRIES) {
        cnputs("    a region for each entry, so the bound stays out OK\n");
    } else if (n_many == n_one) {
        cnprintf("    one region for all %u, so the bound is in it\n",
          (unsigned int)SYS_BATCH_MAX_ENTRIES);

#if SYS_TIER == 0
        /*
         * Asserted at this tier and reported at the other. Section
         * 13.3's claim holds here, so a run where it stops holding is
         * a regression and should say so; it does not hold at Tier 2,
         * where the kernel lock is held across the whole call, and
         * asserting the broken behaviour would mean failing the day
         * somebody fixed it.
         */
        panic("latency: one region covered all %u entries",
          (unsigned int)SYS_BATCH_MAX_ENTRIES);
#endif
    } else {
        /*
         * Neither shape. Not something to pass over: the two counts
         * are meant to be exactly equal or exactly a factor of the
         * entry count apart, and anything else means this is no longer
         * measuring what it says.
         */
        panic("latency: %lu regions for one entry and %lu for %u",
          (unsigned long)n_one, (unsigned long)n_many,
          (unsigned int)SYS_BATCH_MAX_ENTRIES);
    }

    expect("giving the handle back", handle_close(&p->p_handles, h),
      SYS_OK);
}

static void
batch_checks(void)
{
    struct proc *p = proc_current();
    static sys_op_fence_args_t args[SYS_BATCH_MAX_ENTRIES + 1];
    static sys_op_t ops[SYS_BATCH_MAX_ENTRIES + 1];
    uint32_t i;
    sys_err_t err;

    cnputs("batches: the count of section 17 exceeds one\n");

    tl_batch = timeline_create("batch");
    if (tl_batch == NULL) {
        panic("batch: no timeline");
    }
    err = handle_insert(&p->p_handles, tl_batch, SYS_OBJ_TIMELINE,
      SYS_RIGHT_SIGNAL | SYS_RIGHT_WAIT, &h_batch);
    expect("a signal handle for the batch", err, SYS_OK);

    /*
     * Three entries, three signals, and a counter that says how many
     * of them happened.
     */
    batch_fill(ops, args, BATCH_N, 0);
    expect("a batch of three", (sys_err_t)run_batch(ops, BATCH_N),
      SYS_OK);
    cnprintf("    the counter reached %lu                  %s\n",
      (unsigned long)timeline_value(tl_batch),
      timeline_value(tl_batch) == BATCH_N ? "OK" : "WRONG");
    if (timeline_value(tl_batch) != BATCH_N) {
        panic("batch: three entries advanced it to %lu",
          (unsigned long)timeline_value(tl_batch));
    }

    for (i = 0; i < BATCH_N; i++) {
        if (ops[i].status != SYS_OK) {
            panic("batch: entry %u said %d", (unsigned int)i,
              (int)ops[i].status);
        }
    }
    cnputs("    and every entry wrote its own status   OK\n");

    /*
     * A bad entry in the middle. Nothing may run, which the counter
     * proves by not moving, and the entry that was refused has to say
     * why.
     */
    batch_fill(ops, args, BATCH_N, BATCH_N);
    ops[1].opcode = SYS_OP_COUNT;
    expect("a batch with a bad entry in the middle",
      (sys_err_t)run_batch(ops, BATCH_N), SYS_ERR_INVALID_ARG);
    if (timeline_value(tl_batch) != BATCH_N) {
        panic("batch: a refused batch advanced it to %lu",
          (unsigned long)timeline_value(tl_batch));
    }
    if (ops[1].status != SYS_ERR_INVALID_ARG || ops[0].status != SYS_OK) {
        panic("batch: the refusal was not reported on its own entry");
    }
    cnputs("    nothing ran, and the bad entry said why OK\n");

    expect("a batch longer than the bound",
      (sys_err_t)run_batch(ops, SYS_BATCH_MAX_ENTRIES + 1),
      SYS_ERR_INVALID_ARG);
    expect("a batch of none at all",
      (sys_err_t)run_batch(ops, 0), SYS_ERR_INVALID_ARG);

    /*
     * A null argument pointer with a length is a malformed descriptor
     * and not an unreachable one, so the operation answers it rather
     * than the validator. Pinned here because the validator of stage
     * one tested the length instead of the pointer and turned this
     * answer into FAULT, which is a decision that was already made and
     * written down where it was made.
     */
    batch_fill(ops, args, 1, timeline_value(tl_batch));
    ops[0].arg_ptr = 0;
    expect("an entry with a length and no arguments",
      (sys_err_t)run_batch(ops, 1), SYS_ERR_INVALID_ARG);

    /*
     * And the full bound, which is the case section 13.3 has something
     * to say about and the next stage measures.
     */
    batch_fill(ops, args, SYS_BATCH_MAX_ENTRIES, BATCH_N);
    expect("a batch of the largest size allowed",
      (sys_err_t)run_batch(ops, SYS_BATCH_MAX_ENTRIES), SYS_OK);
    cnprintf("    %u entries in one call, counter at %lu  OK\n",
      (unsigned int)SYS_BATCH_MAX_ENTRIES,
      (unsigned long)timeline_value(tl_batch));

    /*
     * Given back, because a later and unrelated check asserts this
     * table is empty. M8 stage three left one behind and failed that
     * check rather than its own.
     */
    expect("giving the handle back",
      handle_close(&p->p_handles, h_batch), SYS_OK);
}


/*
 * The reference counts that decide when an object is freed, taken and
 * let go from both cores at once.
 *
 * M8 listed these as unsafe the moment there were two cores, M9 listed
 * them again, and M10 listed them a third time. Nothing ever checked
 * them, which is most of why nothing ever fixed them: the hazard was a
 * sentence in three retrospectives rather than a line of output.
 *
 * A thread is the object, because a thread carries a count and can be
 * referenced by a handle without being run. The count is put back
 * exactly where it started, which a lost increment or a lost decrement
 * both break.
 */
#define REF_ROUNDS  4096

static struct thread *ref_victim;

static void
ref_hammer(void)
{
    uint32_t i;

    for (i = 0; i < REF_ROUNDS; i++) {
        thread_ref(ref_victim);
        thread_unref(ref_victim);
    }
}

/*
 * The other core's half. Tier 0 has no other core, per section 13.3,
 * so this and the two flags it waits on exist only where there is one.
 */
#if SYS_TIER == 2

static volatile uint32_t ref_go;
static volatile uint32_t ref_done;

static void
ref_thread(void *arg)
{
    (void)arg;

    while (ref_go == 0) {
        md_cpu_relax();
    }

    ref_hammer();

    md_cpu_fence();
    ref_done = 1;
    thread_exit(0);
}

#endif /* SYS_TIER == 2 */

static void
refcount_checks(void)
{
    uint32_t before;

    cnputs("references: a count two cores let go of at once\n");

    /*
     * The checking thread itself, which is running and so cannot be
     * reaped however far its count is wound up and down. One extra
     * reference is held across the whole thing so that reaching zero
     * is never the right answer either.
     */
    ref_victim = thread_current();
    thread_ref(ref_victim);
    before = thread_refs(ref_victim);

#if SYS_TIER == 2
    {
        uint32_t spin;
        struct thread *t = thread_create("ref-other", PRIO_BOOT,
          ref_thread, NULL);

        expect("a second core to let go from", sched_migrate(t, 1),
          SYS_OK);
        sched_ready(t);

        md_cpu_fence();
        ref_go = 1;

        ref_hammer();

        for (spin = 0; spin < 2000000u && ref_done == 0; spin++) {
            sched_yield();
        }
        if (ref_done == 0) {
            panic("references: the other core never finished");
        }
        md_cpu_fence();

        cnprintf("    %u rounds from each of two cores       OK\n",
          (unsigned int)REF_ROUNDS);
    }
#else
    ref_hammer();
    cnprintf("    %u rounds on the only core there is    OK\n",
      (unsigned int)REF_ROUNDS);
#endif

    if (thread_refs(ref_victim) != before) {
        panic("references: the count came back as %u, not %u",
          (unsigned int)thread_refs(ref_victim), (unsigned int)before);
    }
    cnprintf("    and the count came back to %u            OK\n",
      (unsigned int)before);

    thread_unref(ref_victim);
}

static void
clock_checks(void)
{
    uint32_t i;

    cnputs("clocks: what a figure from here is a figure of\n");

    meas_report_clock();
    meas_check_clock();
    cnputs("    it moves, and never backwards            OK\n");

#if SYS_TIER == 2
    {
        struct thread *t;
        uint64_t before, after;
        uint32_t spin;

        /*
         * And it is the same clock on the other core. Both ports take
         * it from a counter the whole machine shares, and a per-core
         * one would pass every other check here while making every
         * figure that crosses a core meaningless.
         */
        t = thread_create("clk", PRIO_BOOT, clk_thread, NULL);
        expect("a reader on the second core", sched_migrate(t, 1), SYS_OK);
        sched_ready(t);

        before = meas_now();
        md_cpu_fence();
        clk_go = 1;

        /*
         * This core's share, entered while the other core is entering
         * the same region. The overlap is the point: a counter kept in
         * one slot for the machine loses increments here and nowhere
         * else in this check.
         */
        for (spin = 0; spin < CLK_SHARE; spin++) {
            uint64_t t0 = meas_enter();

            meas_leave(&meas_shared, t0);
        }

        for (spin = 0; spin < 2000000u && clk_done == 0; spin++) {
            sched_yield();
        }
        if (clk_done == 0) {
            panic("clocks: the second core never read it");
        }
        md_cpu_fence();
        after = meas_now();

        if (clk_stamp < before || clk_stamp > after) {
            panic("clocks: the other core read %lu, outside %lu to %lu",
              (unsigned long)clk_stamp, (unsigned long)before,
              (unsigned long)after);
        }
        cnputs("    and the other core reads the same one  OK\n");

        /*
         * And both cores' shares are there, each on its own core.
         * The total alone would not say that: what says it is that
         * neither core lost any of its own.
         */
        if (meas_count_on(&meas_shared, 0) != CLK_SHARE ||
          meas_count_on(&meas_shared, 1) != CLK_SHARE) {
            panic("clocks: shares of %lu and %lu, not %u each",
              (unsigned long)meas_count_on(&meas_shared, 0),
              (unsigned long)meas_count_on(&meas_shared, 1),
              (unsigned int)CLK_SHARE);
        }
        cnprintf("    one region from both cores at once     %lu and %lu\n",
          (unsigned long)meas_count_on(&meas_shared, 0),
          (unsigned long)meas_count_on(&meas_shared, 1));
    }
#else
    for (i = 0; i < CLK_SHARE; i++) {
        uint64_t t0 = meas_enter();

        meas_leave(&meas_shared, t0);
    }
#endif

    if (meas_count(&meas_shared) != CLK_SHARE * cpu_count()) {
        panic("clocks: %lu entries in the shared region, not %lu",
          (unsigned long)meas_count(&meas_shared),
          (unsigned long)(CLK_SHARE * cpu_count()));
    }

    /*
     * The unit that does not move. A region entered twice as often is
     * counted twice as often, on every host and in every configuration,
     * which is why the count is what the check compares and the
     * duration beside it is not.
     */
    for (i = 0; i < CLK_ONCE; i++) {
        uint64_t t0 = meas_enter();

        meas_leave(&meas_once, t0);
    }
    for (i = 0; i < CLK_TWICE; i++) {
        uint64_t t0 = meas_enter();

        meas_leave(&meas_twice, t0);
    }

    if (meas_count(&meas_once) != CLK_ONCE ||
      meas_count(&meas_twice) != CLK_TWICE) {
        panic("clocks: counted %lu and %lu, not %u and %u",
          (unsigned long)meas_count(&meas_once),
          (unsigned long)meas_count(&meas_twice),
          (unsigned int)CLK_ONCE, (unsigned int)CLK_TWICE);
    }
    if (meas_count(&meas_twice) != 2 * meas_count(&meas_once)) {
        panic("clocks: twice the work did not count twice");
    }

    cnprintf("    twice the work, twice the count        %lu and %lu\n",
      (unsigned long)meas_count(&meas_once),
      (unsigned long)meas_count(&meas_twice));

    /*
     * And the durations beside them, which are not compared and are not
     * asserted against anything. They are here to be looked at, and to
     * make the point that the pair of numbers above is the same
     * everywhere while this pair is not.
     */
    cnprintf("    longest visit to each, in ticks        %lu and %lu\n",
      (unsigned long)meas_longest(&meas_once),
      (unsigned long)meas_longest(&meas_twice));
}

#if SYS_TIER == 2
/*
 * Section 11.4: a thread runs on the core it was put on, and nothing
 * puts it anywhere else.
 */
static volatile uint32_t migrant_core = 0xffffffffu;
static volatile uint32_t migrant_links;
static volatile uint32_t migrant_done;
static volatile uint32_t stayer_cores;
static volatile uint32_t stayer_rounds;
static volatile uint32_t stayer_stop;

static void
migrant_thread(void *arg)
{
    (void)arg;

    migrant_core = cpu_self()->cp_index;

    /*
     * Read from inside the thread because it is the only place the
     * thread is certainly still there to be asked. A slot goes back to
     * the pool when the last thing that could ask about it lets go.
     */
    migrant_links = thread_linked_cores(thread_current());
    migrant_done = 1;
}

static void
stayer_thread(void *arg)
{
    (void)arg;

    while (stayer_stop == 0) {
        stayer_cores |= (uint32_t)1 << cpu_self()->cp_index;
        stayer_rounds++;
        sched_yield();
    }
}


static void
core_checks(void)
{
    sys_op_migrate_args_t margs;
    struct thread *t;
    sys_handle_t h;
    uint32_t spin;

    cnputs("cores: where a thread runs, section 11.4\n");

    /*
     * Migrated before it has ever been runnable, so that the core it
     * started on never has it on a queue at all. Migrating it after
     * would work and would prove less: the check is that a thread put
     * on the other core was never on this one.
     */
    t = thread_create("migrant", PRIO_BOOT + 1, migrant_thread, NULL);

    /*
     * Through the operation a program would use, and not only through
     * the kernel call under it. Where a thread runs is authority over
     * that thread, so it is reached with a handle carrying WRITE and
     * refused without one.
     */
    expect("a handle to it, with the right to move it",
      handle_insert(&proc_current()->p_handles, t, SYS_OBJ_THREAD,
        SYS_RIGHT_READ | SYS_RIGHT_WRITE, &h), SYS_OK);
    margs.core = 1;
    margs.pad = 0;
    expect("moving it to the second core",
      run(SYS_OP_THREAD_MIGRATE, h, &margs, sizeof(margs)), SYS_OK);
    margs.core = SYS_MAX_CORES;
    expect("moving it to a core that is not there",
      run(SYS_OP_THREAD_MIGRATE, h, &margs, sizeof(margs)),
      SYS_ERR_INVALID_ARG);
    expect("moving it with no argument structure",
      run(SYS_OP_THREAD_MIGRATE, h, NULL, 0), SYS_ERR_INVALID_ARG);
    expect("moving the thread that is running",
      sched_migrate(thread_current(), 1), SYS_ERR_WOULDBLOCK);

    /*
     * Given back before the thread runs, because the handle checks
     * later assert that this table is empty and a check that leaves
     * something behind fails a check that has nothing to do with it.
     */
    expect("giving the handle back", handle_close(&proc_current()->p_handles, h),
      SYS_OK);

    {
        uint64_t decisions = sched_decisions();

        sched_ready(t);

        /*
         * Readying it took no scheduling decision here, though it
         * outranks what is running. The decision that would have been
         * taken chooses this thread again, so what the guard saves is
         * invisible in what ran and has to be counted.
         */
        if (sched_decisions() != decisions) {
            panic("cores: a thread put on the other core took %lu "
              "decisions on this one",
              (unsigned long)(sched_decisions() - decisions));
        }
    }

    /*
     * Offered the core while waiting, and at the same priority the
     * migrated thread has. A wait that spun instead would prove less
     * and say less: a thread that was never migrated would sit on this
     * core's queue behind a thread that never yields, and the check
     * would hang rather than report where it ran.
     */
    for (spin = 0; spin < 2000000u && migrant_done == 0; spin++) {
        sched_yield();
    }
    if (migrant_done == 0) {
        panic("cores: the migrated thread never ran");
    }

    cnprintf("    it ran on core %u                       %s\n",
      (unsigned int)migrant_core, migrant_core == 1 ? "OK" : "WRONG");
    if (migrant_core != 1) {
        panic("cores: migrated to core 1 and ran on core %u",
          (unsigned int)migrant_core);
    }
    if (migrant_links != ((uint32_t)1 << 1)) {
        panic("cores: it was queued on 0x%lx and only core 1 was asked",
          (unsigned long)migrant_links);
    }
    cnputs("    and core 0 never had it queued         OK\n");

    /*
     * It outranks what this core was running, and this core did not
     * stop for it. A thread made runnable elsewhere is nothing to do
     * with what runs here, and preempting for one would be a core
     * giving up its turn for a thread that cannot take it.
     */
    if (sched_remote_readies == 0) {
        panic("cores: nothing outranking this core was put on the other");
    }
    cnprintf("    %lu readied there, none preempting here  OK\n",
      (unsigned long)sched_remote_readies);

    /*
     * And one that is not migrated, over a run long enough that a
     * balancer would have had every chance to move it.
     */
    t = thread_create("stayer", PRIO_BOOT, stayer_thread, NULL);
    sched_ready(t);

    while (stayer_rounds < 2000) {
        sched_yield();
    }
    stayer_stop = 1;
    sched_yield();

    cnprintf("    %lu turns, all on core %u              OK\n",
      (unsigned long)stayer_rounds,
      (unsigned int)(31u - (unsigned int)__builtin_clz(stayer_cores)));
    if (stayer_cores != 1u) {
        panic("cores: a thread nothing moved ran on 0x%lx",
          (unsigned long)stayer_cores);
    }
}
#endif /* SYS_TIER == 2 */

#if SYS_TIER == 2
/*
 * Section 11.4: donation crosses a core boundary. The server is pinned
 * to the second core and the client runs on the first, so the priority
 * the server serves at had to travel with an inter-processor interrupt
 * to get there.
 */
#define XC_SERVER_PRIO  4
#define XC_CLIENT_PRIO  9

static struct channel *ch_xc;
static sys_handle_t h_xc_recv;
static volatile uint32_t xc_served;
static volatile uint8_t xc_prio_serving;
static volatile uint8_t xc_prio_after;
static volatile uint32_t xc_core;

static void
xc_server(void *arg)
{
    struct sys_op_recv_args rx;
    struct sys_op_reply_args tx;

    (void)arg;

    for (;;) {
        memset(&rx, 0, sizeof(rx));
        if (run(SYS_OP_IPC_RECV, h_xc_recv, &rx, sizeof(rx)) != SYS_OK) {
            panic("xc server: receive failed");
        }

        /*
         * What it is running at while it serves, and which core it is
         * on. The first is the donation and the second is what makes
         * it a donation that crossed.
         */
        xc_prio_serving = thread_current()->t_prio;
        xc_core = cpu_self()->cp_index;

        memset(&tx, 0, sizeof(tx));
        tx.peer = rx.peer;
        tx.reply.inline_len = 3;
        memcpy(tx.reply.inline_data, "ok", 3);
        if (run(SYS_OP_IPC_REPLY, 0, &tx, sizeof(tx)) != SYS_OK) {
            panic("xc server: reply failed");
        }

        /* And what it drops back to once the reply has undone it. */
        xc_prio_after = thread_current()->t_prio;
        xc_served++;
    }
}

static void
xc_client(void *arg)
{
    struct sys_op_send_args tx;
    int i;

    for (i = 0; i < 3; i++) {
        memset(&tx, 0, sizeof(tx));
        tx.request.inline_len = 3;
        memcpy(tx.request.inline_data, "xc", 3);
        if (run(SYS_OP_IPC_SEND, (sys_handle_t)(uintptr_t)arg, &tx,
          sizeof(tx)) != SYS_OK) {
            panic("xc client: send failed");
        }
    }

    thread_exit(0);
}


#if SYS_TIER == 2
/*
 * Two cores taking slots from one pool and giving them back.
 *
 * The pool the system runs on is reached from both cores already, but
 * twice in a whole session, which is a lock needed by construction and
 * tested by nothing. This is the same argument the kernel lock got in
 * stage two and the same answer: a pool of its own, hammered from both
 * sides, so that the exclusion is exercised rather than assumed.
 *
 * What breaks without it is the free list rather than the counters. Two
 * callers take the head, both are given the same slot, and the list is
 * shorter than the number of things that think they hold part of it.
 */
#define PS_SLOTS        8
#define PS_ROUNDS       4096

struct ps_obj {
    /*
     * Written by the core holding the slot and read back by it. Marked
     * so that the reading back actually happens: a plain member
     * written and then compared against what was just written is a
     * comparison the compiler is entitled to delete, and it does, so
     * the first version of this check could not fail.
     */
    volatile uint64_t   ps_mark;
};

POOL_STORAGE(ps, struct ps_obj, PS_SLOTS);
static struct pool ps_pool;
static volatile uint32_t ps_go;
static volatile uint32_t ps_done;
static volatile uint64_t ps_taken[SYS_MAX_CORES];
static volatile uint32_t ps_clash;

static void
ps_round(void)
{
    struct ps_obj *o;
    sys_id_t id;
    uint32_t i;
    uint64_t mark = (uint64_t)cpu_self()->cp_index + 1;

    o = pool_alloc(&ps_pool, &id);
    if (o == NULL) {
        return;
    }

    /*
     * The slot is this core's alone from the moment it left the list,
     * so what was written here is what is here. A mark that is not the
     * one just written means the other core was handed the same slot,
     * which is the failure this is looking for and not a lost count.
     *
     * The gap between writing and reading is on purpose. Two cores
     * have to be inside at once for this to see anything, and a window
     * a few instructions wide is one they would have to hit exactly.
     */
    o->ps_mark = mark;
    for (i = 0; i < 16; i++) {
        md_cpu_relax();
    }
    if (o->ps_mark != mark) {
        ps_clash++;
    }

    ps_taken[cpu_self()->cp_index]++;

    if (!pool_free(&ps_pool, id)) {
        panic("pool check: a slot that was taken would not go back");
    }
}

static void
ps_thread(void *arg)
{
    uint32_t i;

    (void)arg;

    while (ps_go == 0) {
        md_cpu_relax();
    }
    for (i = 0; i < PS_ROUNDS; i++) {
        ps_round();
    }

    md_cpu_fence();
    ps_done = 1;
    thread_exit(0);
}

static void
pool_stress_checks(void)
{
    struct thread *t;
    uint32_t i;
    uint32_t spin;

    cnputs("pools: two cores taking slots from one\n");

    POOL_INIT(&ps_pool, ps, "stress", PS_SLOTS);

    t = thread_create("ps-other", PRIO_BOOT, ps_thread, NULL);
    expect("putting the other half on the second core",
      sched_migrate(t, 1), SYS_OK);
    sched_ready(t);

    md_cpu_fence();
    ps_go = 1;

    for (i = 0; i < PS_ROUNDS; i++) {
        ps_round();
    }

    for (spin = 0; spin < 2000000u && ps_done == 0; spin++) {
        sched_yield();
    }
    if (ps_done == 0) {
        panic("pool check: the other core never finished");
    }
    md_cpu_fence();

    cnprintf("    %lu rounds, %lu on core 0 and %lu on core 1\n",
      (unsigned long)(ps_taken[0] + ps_taken[1]),
      (unsigned long)ps_taken[0], (unsigned long)ps_taken[1]);
    if (ps_taken[1] == 0) {
        panic("pool check: the second core took nothing");
    }
    if (ps_clash != 0) {
        panic("pool check: one slot handed to two cores %lu times",
          (unsigned long)ps_clash);
    }

    /*
     * And the list is all there. Every slot taken was given back, so
     * everything must be free and the whole pool must be available
     * again, which a shortened list cannot manage.
     */
    for (i = 0; i < PS_SLOTS; i++) {
        if (pool_alloc(&ps_pool, NULL) == NULL) {
            panic("pool check: %u of %u slots came back",
              (unsigned int)i, (unsigned int)PS_SLOTS);
        }
    }
    cnprintf("    and all %u slots came back              OK\n",
      (unsigned int)PS_SLOTS);
}
#endif /* SYS_TIER == 2 */

static void
cross_core_donation_checks(void)
{
    struct proc *p = proc_current();
    struct thread *server;
    sys_handle_t send_h;
    uint32_t spin;
    sys_err_t err;

    cnputs("donation: across a core boundary, section 11.4\n");

    ch_xc = channel_create("xc", p);
    if (ch_xc == NULL) {
        panic("xc: no channel");
    }
    err = handle_insert(&p->p_handles, ch_xc, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV, &h_xc_recv);
    expect("a receive handle on the crossing channel", err, SYS_OK);
    err = handle_insert(&p->p_handles, ch_xc, SYS_OBJ_CHANNEL,
      SYS_RIGHT_SEND, &send_h);
    expect("a send handle on it", err, SYS_OK);

    /*
     * The server below the client and on the other core. Both halves
     * matter: below, or there is nothing to donate, and elsewhere, or
     * the donation never leaves this core.
     */
    server = thread_create("xc-server", XC_SERVER_PRIO, xc_server, NULL);
    expect("pinning the server to the second core",
      sched_migrate(server, 1), SYS_OK);
    sched_ready(server);

    sched_ready(thread_create("xc-client", XC_CLIENT_PRIO, xc_client,
      (void *)(uintptr_t)send_h));

    for (spin = 0; spin < 2000000u && xc_served < 3; spin++) {
        sched_yield();
    }
    if (xc_served < 3) {
        panic("donation: the server on the other core served %lu of 3",
          (unsigned long)xc_served);
    }

    cnprintf("    a server based at %u served at %u          %s\n",
      (unsigned int)XC_SERVER_PRIO, (unsigned int)xc_prio_serving,
      xc_prio_serving == XC_CLIENT_PRIO ? "OK" : "WRONG");
    if (xc_prio_serving != XC_CLIENT_PRIO) {
        panic("donation: served at %u and the client was at %u",
          (unsigned int)xc_prio_serving, (unsigned int)XC_CLIENT_PRIO);
    }

    cnprintf("    on core %u, which is not the client's    OK\n",
      (unsigned int)xc_core);
    if (xc_core != 1) {
        panic("donation: the server ran on core %u",
          (unsigned int)xc_core);
    }

    cnprintf("    and back to %u once the reply undid it   %s\n",
      (unsigned int)xc_prio_after,
      xc_prio_after == XC_SERVER_PRIO ? "OK" : "WRONG");
    if (xc_prio_after != XC_SERVER_PRIO) {
        panic("donation: left at %u after the reply",
          (unsigned int)xc_prio_after);
    }
}
#endif /* SYS_TIER == 2 */

#if SYS_TIER == 2

/*
 * The milestone's own question, asked of the storage stack.
 *
 * A read goes from a client into the filesystem and from there into the
 * block store, which is two boundaries and a donation chain across
 * them. Putting the store on the other core makes every one of those
 * crossings a real one, and what it costs is the number this stage is
 * for: pokes sent between cores, and donations raised on a thread the
 * raising core does not own. Both are zero on one core by construction.
 */
static void
pinned_storage_checks(struct proc *store, sys_handle_t root,
  uint64_t base_pokes, uint64_t base_donations)
{
    struct thread *t;
    uint64_t pokes, donations;

    cnputs("placement: the storage stack across two cores\n");

    cnprintf("    on one core it cost                    "
      "%lu pokes, %lu donations\n",
      (unsigned long)base_pokes, (unsigned long)base_donations);

    t = proc_threads(store);
    if (t == NULL) {
        panic("placement: the block store has no thread");
    }

    /*
     * Blocked at its receive point, which is why this is allowed at
     * all: a thread on no queue can be moved by anyone, and a thread
     * running on another core cannot be moved by this one.
     */
    expect("moving the block store to the second core",
      sched_migrate(t, 1), SYS_OK);

    {
        uint64_t before_pokes, before_donations;
        uint64_t after_pokes, after_donations;

        before_pokes = cpu_crossings(&before_donations);

        /*
         * The same read again, with one boundary now crossing a core
         * as well as an address space. Nothing above knows: the client
         * asks for the same file and gets the same bytes.
         */
        run_client("fs_client", root, BLK_MOTD_BYTES, SEND_BLOCK_EITHER);

        after_pokes = cpu_crossings(&after_donations);

        pokes = after_pokes - before_pokes;
        donations = after_donations - before_donations;
    }

    cnprintf("    with the store on the other core       "
      "%lu pokes, %lu donations\n",
      (unsigned long)pokes, (unsigned long)donations);

    /*
     * That the crossing happened at all. A read that never left the
     * first core would cost nothing extra and pass every other check
     * here, which is the shape of failure this milestone keeps
     * finding.
     *
     * The donations are what prove it and the pokes are not. A poke is
     * sent for any reason one core has to tell another something,
     * including a timer being rearmed, so a nonzero count of them
     * would let this pass on a machine where the read never left the
     * first core. A donation raised on a thread this core does not own
     * is the chain crossing and nothing else.
     */
    if (donations == 0) {
        panic("placement: the store moved and no donation crossed");
    }
    if (pokes == 0) {
        panic("placement: a donation crossed and nothing was told");
    }
    if (t->t_core != 1) {
        panic("placement: the store is on core %u",
          (unsigned int)t->t_core);
    }
}
#endif /* SYS_TIER == 2 */

static void
user_slice(void)
{
    struct proc *self = proc_current();
#if SYS_TIER == 2
    struct proc *blk_proc;
    sys_handle_t root4;
#endif
    struct sys_op_connect_args cargs;
    sys_ns_spec_t ns[2];
    struct channel *ch;
    struct channel *blk;
    sys_handle_t h_recv, h_blk, store, root, root2, root3, root5, root6;
    sys_handle_t root7;
    sys_err_t err;

    boot_report();
    cohort_report();

    ch = channel_create("fs-user", self);
    if (ch == NULL) {
        panic("user slice: no channel");
    }

    /*
     * SYS_RIGHT_TRANSFER because this handle is going to be handed to the
     * server, and section 7.4 makes moving a handle a right rather than
     * something a holder may always do.
     */
    err = handle_insert(&self->p_handles, ch, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &h_recv);
    expect("a receive handle for the filesystem", err, SYS_OK);

    /*
     * Two root connections, minted here while this process still holds the
     * receive right, because section 12.1 lets only a holder of that right
     * mint one and handing it to the server moves it away. One per client,
     * since a handle moves rather than copies.
     *
     * The cookie is the server's own per-connection state, so this is the
     * one place the kernel has to know a convention that belongs to a
     * program. It is what root does at boot, per section 24.5.
     */
    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = FS_NODE_ROOT;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a connection to the root directory",
      run(SYS_OP_CHAN_CONNECT, h_recv, &cargs, sizeof(cargs)), SYS_OK);
    root = cargs.out_handle;

    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = FS_NODE_ROOT;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a second one, for the second client",
      run(SYS_OP_CHAN_CONNECT, h_recv, &cargs, sizeof(cargs)), SYS_OK);
    root2 = cargs.out_handle;

    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = FS_NODE_ROOT;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a third one, for the client that chains",
      run(SYS_OP_CHAN_CONNECT, h_recv, &cargs, sizeof(cargs)), SYS_OK);
    root5 = cargs.out_handle;

    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = FS_NODE_ROOT;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a fourth one, for the read after everything is reclaimed",
      run(SYS_OP_CHAN_CONNECT, h_recv, &cargs, sizeof(cargs)), SYS_OK);
    root6 = cargs.out_handle;

    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = FS_NODE_ROOT;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a fifth one, for the program that runs from what it lent",
      run(SYS_OP_CHAN_CONNECT, h_recv, &cargs, sizeof(cargs)), SYS_OK);
    root7 = cargs.out_handle;

#if SYS_TIER == 2
    /*
     * And one for the same read done again with the store on the other
     * core. A connection is handed over to the client that uses it, so
     * reading twice means connecting twice.
     */
    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = FS_NODE_ROOT;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("one more, for the read across two cores",
      run(SYS_OP_CHAN_CONNECT, h_recv, &cargs, sizeof(cargs)), SYS_OK);
    root4 = cargs.out_handle;
#endif

    /*
     * A third, for the shell, and duplicable because the shell hands a
     * copy to every child that names a file. Minted here for the same
     * reason as the other two: only a holder of the receive right may
     * mint one, and that right is about to move to the server.
     */
    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = FS_NODE_ROOT;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_DUPLICATE |
      SYS_RIGHT_TRANSFER;
    expect("a third one, for the shell",
      run(SYS_OP_CHAN_CONNECT, h_recv, &cargs, sizeof(cargs)), SYS_OK);
    h_fs_root = cargs.out_handle;

    /*
     * And a union: two directories under one name, per section 20.4.
     * Both are this filesystem, reached through connections whose cookie
     * says which directory the server should show, so what is joined is
     * two directories rather than two servers. The order is the order
     * these are written, and it is the whole of the semantics: a name in
     * both comes from the first.
     */
    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = FS_NODE_ROOT;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_DUPLICATE |
      SYS_RIGHT_TRANSFER;
    expect("a union, first half",
      run(SYS_OP_CHAN_CONNECT, h_recv, &cargs, sizeof(cargs)), SYS_OK);
    u_first = cargs.out_handle;

    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = FS_NODE_ALT;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_DUPLICATE |
      SYS_RIGHT_TRANSFER;
    expect("and second", 
      run(SYS_OP_CHAN_CONNECT, h_recv, &cargs, sizeof(cargs)), SYS_OK);
    u_second = cargs.out_handle;

    /*
     * A fourth connection, for the program that describes a buffer it
     * does not own. It gets an ordinary one because the check is that a
     * bad descriptor is refused, not that a bad client is.
     */
    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = FS_NODE_ROOT;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a fourth one, for the trespasser",
      run(SYS_OP_CHAN_CONNECT, h_recv, &cargs, sizeof(cargs)), SYS_OK);
    root3 = cargs.out_handle;

    /*
     * The block store, and the connection the filesystem reads it
     * through. Built before the filesystem because the filesystem
     * cannot start without it: the bytes it serves are down there now,
     * and a file server with nothing under it has nothing to serve.
     *
     * Two processes and one more boundary. What a client asks for now
     * crosses into the filesystem and then into the store, which is
     * the stack section 18 is about and the thing this tree did not
     * have. Nothing above notices, which is the check.
     */
    blk = channel_create("blk-store", self);
    if (blk == NULL) {
        panic("user slice: no channel for the store");
    }
    err = handle_insert(&self->p_handles, blk, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV | SYS_RIGHT_TRANSFER, &h_blk);
    expect("a receive handle for the block store", err, SYS_OK);

    memset(&cargs, 0, sizeof(cargs));
    cargs.cookie = 0;
    cargs.rights = SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER;
    expect("a connection to it, for the filesystem",
      run(SYS_OP_CHAN_CONNECT, h_blk, &cargs, sizeof(cargs)), SYS_OK);
    store = cargs.out_handle;

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_SERVICE;
    ns[0].handle = h_blk;
    cnputs("    loading blk_server, which holds the bytes\n");
    {
        struct proc *store = proc_spawn_image("blk_server", 15, ns, 1, NULL, 0);

#if SYS_TIER == 2
        blk_proc = store;
#else
        (void)store;
#endif
    }

    memset(ns, 0, sizeof(ns));
    ns[0].id = SYS_NS_SERVICE;
    ns[0].handle = h_recv;
    ns[1].id = SYS_NS_STORE;
    ns[1].handle = store;
    cnputs("    loading fs_server, which receives on that channel\n");
    (void)proc_spawn_image("fs_server", 15, ns, 2, NULL, 0);

    /*
     * The same file, read two ways. Section 25.7 says one message should
     * cost a third of the round trips and none of the connection pool; if
     * it does not, the section is deleted and section 12.7 with it.
     */
    {
        uint32_t a = frames_quiet();

        kernel_buffer_checks(h_fs_root);

        /*
         * Nothing, which the plan for this stage did not expect. A
         * buffer lent across a rendezvous is the client's own memory
         * mapped into the server for the length of the call, so there
         * is no frame to hold. The tables that describe it where the
         * server sees it were built by the first such call and are
         * reused by every one after.
         */
        frame_holder("a buffer lent across a rendezvous", a, 0, "nothing");
    }

#if SYS_TIER == 2
    {
        uint64_t p0, d0, p1, d1;

        /*
         * The same read, measured with everything on one core, so
         * that what putting the store on the other one costs is a
         * difference rather than a number on its own.
         */
        p0 = cpu_crossings(&d0);
        run_client("fs_client", root, BLK_MOTD_BYTES, SEND_BLOCK_YES);
        p1 = cpu_crossings(&d1);

        run_client("fs_batch", root2, BLK_MOTD_BYTES, SEND_BLOCK_NO);
        run_client("fs_chain", root5, BLK_MOTD_BYTES, SEND_BLOCK_YES);

        pinned_storage_checks(blk_proc, root4, p1 - p0, d1 - d0);
    }
#else
    run_client("fs_client", root, BLK_MOTD_BYTES, SEND_BLOCK_YES);
    run_client("fs_batch", root2, BLK_MOTD_BYTES, SEND_BLOCK_NO);
    run_client("fs_chain", root5, BLK_MOTD_BYTES, SEND_BLOCK_YES);
#endif

    trespass_checks(root3);

    move_checks();
    reap_checks();
    cancel_checks();
    pulse_checks();
    death_checks();
    kill_checks();
    cohort_checks();

    /*
     * Memory given back, and then the same file read again through a
     * process spawned after it was. M13 stage 3 frees page tables, and
     * a table freed while something could still reach it is a frame
     * handed to the next caller with a live mapping pointing into it.
     * The read is what would find that: it spawns a program into
     * frames the reclamation has just returned, and takes it across the
     * trap edge and through a server in another address space.
     *
     * Here rather than later in the boot because this is where a
     * connection to the file server exists. The reclamation wants to
     * happen before the read and not after, which is the whole point.
     */
    frame_cost_checks();
    frame_return_checks();
    {
        uint32_t a = frames_quiet();

        run_client("fs_client", root6, BLK_MOTD_BYTES, SEND_BLOCK_YES);

        /*
         * And the whole of it in one number: a process spawned into the
         * frames the reclamation has just returned, reading a file
         * through a server in another address space, and then reaped.
         */
        frame_holder("a client that ran and was reaped", a, 0, "nothing");
    }

    lent_permission_checks(root7);
}

/*
 * What is still held when the boot is over, and what holds it, per M13
 * stage 6.
 *
 * The milestone's other stages each measured one mechanism. This is the
 * one that has to add up: every frame the machine has spent is either
 * something named here or a number this cannot account for, and the
 * last line is what says which.
 *
 * The parts, in the order they are subtracted:
 *
 * What the port spent before anything ran is the kernel's own image,
 * the boot archive and the early mapping. Nothing owns it and nothing
 * can give it back, and a system that reclaimed it would be a system
 * unmapping the code doing the reclaiming.
 *
 * What live processes hold is their slots and the tables that reach
 * them. These are not a leak: they belong to programs that have not
 * finished, which at the end of this boot is every server the checks
 * started and never stopped. Section 15 has them returned when their
 * owner is gone, and stages two and three are what does that.
 *
 * What cohorts hold is one address space each, counted against the
 * cohort because that is whose it is.
 *
 * What the named holders of stage four hold is a grant's page and the
 * pages a pager supplied. Both belong to an object that outlives the
 * process they were lent to, and neither is a frame a process could
 * give back.
 *
 * The remainder is the honest part. If it is zero, everything the
 * machine has spent has a name. If it is not, the number is what nobody
 * has claimed, and it is printed rather than explained away.
 */
static void
frame_ledger(void)
{
    struct pool *procs = proc_pool();
    uint32_t held = page_used();
    uint32_t live = 0, in_procs = 0, in_cohorts, named, orphaned, rest;
    uint32_t in_kernel = 0;
    uint32_t i;

    for (i = 0; i < procs->count; i++) {
        struct proc *p = pool_at(procs, i);

        if (p == NULL) {
            continue;
        }
        live++;
        in_procs += proc_frames(p);
    }

    in_cohorts = cohort_frames();
    named = grant_frames();
    orphaned = proc_frames_orphaned();
#if SYS_TIER == 2
    in_kernel = md_as_frames(vm_kernel()) - kernel_tables_at_start;
#endif

    cnprintf("frames: %u still held when the boot is over\n",
      (unsigned int)held);
    cnprintf("    %-38s %u\n", "spent before anything ran",
      (unsigned int)frames_at_start);
    cnprintf("    %-38s %u in %u of them\n", "held by processes still running",
      (unsigned int)in_procs, (unsigned int)live);
    cnprintf("    %-38s %u\n", "held by cohort address spaces",
      (unsigned int)in_cohorts);
    cnprintf("    %-38s %u\n", "held by grants over ordinary memory",
      (unsigned int)named);
    cnprintf("    %-38s %u\n", "orphaned when a cohort place moved on",
      (unsigned int)orphaned);
    cnprintf("    %-38s %u\n", "added to the kernel's own address space",
      (unsigned int)in_kernel);

    rest = held - frames_at_start - in_procs - in_cohorts - named -
      orphaned - in_kernel;

    cnprintf("    %-38s %u\n", "held by nothing that says so", rest);

    /*
     * Asserted, because every part of it is a count. What it asserts is
     * not that the remainder is zero, which it is not: it is that the
     * remainder is what it was when this was written, so that a change
     * to it is something a reader is told about rather than something
     * they would have to go looking for.
     */
    if (held < frames_at_start + in_procs + in_cohorts + named +
      orphaned + in_kernel) {
        panic("frames: the parts come to more than the whole");
    }
    if (rest != FRAME_UNCLAIMED) {
        panic("frames: %u held by nothing that says so, not %u",
          (unsigned int)rest, (unsigned int)FRAME_UNCLAIMED);
    }
}

/*
 * What a server thread owes against what it records, per M22 stage one.
 *
 * `t_client` is one identifier and a receiving thread may owe several
 * replies. Section 12.6 describes a server that hands the client id to
 * a worker and keeps receiving, section 12.2 says any thread of the
 * server may answer, and the console does both: it takes a read with no
 * line behind it, holds the request, goes back to receive, and the
 * thread that has the bytes replies.
 *
 * These four counts move with when console input arrives, the same way
 * the count of address space switches does, so they are here rather
 * than in the compared file. They are the evidence that the record
 * stops naming what is owed, and the numbers that do not move are in
 * the report below.
 *
 * The last of them is a walk of every thread at the end of the boot,
 * not a running count: a thread on a serving list owing nobody is a
 * channel that reads as having somebody standing in it, and the
 * question is whether any are left that way when everything is over.
 */
static void
owed_checks(void)
{
    struct pool *threads = thread_pool();
    uint32_t standing = 0;
    uint32_t checked = 0;
    uint32_t pointing = 0;
    uint32_t owing = 0;
    intr_state_t ks = 0;
    intr_state_t s;
    uint64_t t0;
    uint64_t t1;
    bool held;
    uint32_t i;

    /*
     * Under the kernel's exclusion, and with interrupts off inside it.
     *
     * This runs on the boot thread after the session, and the session
     * does not take everything with it: the console and `wedged` are
     * still there, and at Tier 2 the other core can be inside a
     * rendezvous writing the two fields this reads. M20 stage three's
     * review found the same thing about the walk that checks channel
     * holds, and this is that finding arriving again rather than a new
     * one.
     */
    held = klock_held();
    if (!held) {
        ks = klock_enter();
    }
    s = md_intr_disable();
    t0 = time_now();

    for (i = 0; i < pool_slots(threads); i++) {
        struct thread *t = pool_at(threads, i);
        struct thread *c;
        uint32_t listed;

        if (t == NULL) {
            continue;
        }

        if (t->t_serve_ch != NULL) {
            standing++;
            if (t->t_donee == NULL) {
                panic("owed: %s is served on %s and owed by nobody",
                  t->t_name, t->t_serve_ch->c_name);
            }
        }

        if (t->t_donee != NULL) {
            pointing++;
        }

        /*
         * Every thread's list against its count, and every member of
         * that list against its own record of who owes it.
         *
         * The walk inside the rendezvous runs where the count passes
         * one, which is once in this boot and says nothing about the
         * other thirty-one slots. This asks all of them at a moment
         * when the answer should be settled.
         */
        listed = 0;
        for (c = t->t_owed_head; c != NULL; c = c->t_owed_next) {
            if (c->t_donee != t) {
                panic("owed: %s is on the list of %s and owed by %s",
                  c->t_name, t->t_name,
                  c->t_donee == NULL ? "nobody" : c->t_donee->t_name);
            }
            listed++;
            if (listed > pool_slots(threads)) {
                panic("owed: the list of %s does not end", t->t_name);
            }
        }
        if (listed != t->t_owed) {
            panic("owed: %s owes %u and its list holds %u", t->t_name,
              (unsigned int)t->t_owed, (unsigned int)listed);
        }
        owing += t->t_owed;
        checked++;
    }

    /*
     * And the two totals, which is what an inner walk of every thread
     * used to say and this says in one pass instead of thirty-two.
     *
     * Each thread's list holds exactly as many clients as it owes, and
     * every one of them names that thread, both checked above. So the
     * sum of what every thread owes is the number of distinct clients
     * that are on some list. If that equals the number of clients
     * naming a server, then no client names a server whose list it is
     * not on, which is the third view and the one the other two cannot
     * give between them.
     */
    if (owing != pointing) {
        panic("owed: %u clients are owed and %u name a server",
          (unsigned int)owing, (unsigned int)pointing);
    }

    t1 = time_now();
    md_intr_restore(s);
    if (!held) {
        klock_exit(ks);
    }

    cnputs("what a server thread owes, against what it records\n");
    cnprintf("    %-38s %lu\n", "held a request and blocked receiving",
      (unsigned long)channel_owed_cleared);
    cnprintf("    %-38s %lu\n", "replies its record did not name",
      (unsigned long)channel_owed_reply_unnamed);
    cnprintf("    %-38s %u\n",
      "requests on a serving list at the end", (unsigned int)standing);
    cnprintf("    %-38s %u threads, all agreeing, about %lu us\n",
      "count, list and walk at the end", (unsigned int)checked,
      (unsigned long)((t1 - t0) * 1000000ULL / time_hz()));
    cnprintf("    %-38s %lu of %lu\n",
      "the set kept what a block gave away",
      (unsigned long)channel_owed_cleared_kept,
      (unsigned long)channel_owed_cleared);

    /*
     * Asserted above zero rather than at a value, because it counts
     * something the recorded session does and the session is paced by
     * the host. What the milestone needs from it is that it happens at
     * all: a boot where no thread ever holds a request and goes back to
     * receive is a boot this milestone has nothing to say about.
     */
    if (channel_owed_cleared == 0) {
        panic("owed: no thread held a request and blocked receiving");
    }

    /*
     * And the walk ran over something. A pool that reported no live
     * threads would agree with every count it did not make.
     */
    if (checked == 0) {
        panic("owed: the walk found no threads to check");
    }

    /*
     * And the other way the record goes. A receive that blocks gives
     * the field to saying which kind of wake this was, and the client
     * it named is still owed; the set is what still names it. Asserted
     * as an equality rather than above zero, because the equality holds
     * in both the paced run and the burst one while the count itself
     * does not.
     */
    if (channel_owed_cleared_kept != channel_owed_cleared) {
        panic("owed: %lu records given away at a block, %lu kept",
          (unsigned long)channel_owed_cleared,
          (unsigned long)channel_owed_cleared_kept);
    }

    /*
     * Replies whose sender did not hold the request are reported and
     * not asserted, which is written down rather than left to look
     * stronger than it is. They need a reply sent by a thread other
     * than the one that received, and the only thing in this boot that
     * does it is the console answering a read it had to wait for a
     * line to fill. In the burst session the line is always there, the
     * request thread answers where it stands, and the count is zero.
     * An assertion above zero would hold in the paced run and stop the
     * burst one, which is a check that measures the harness.
     *
     * Stage one counted two more numbers here and stage four took
     * both away. One was threads left on a serving list by another
     * thread's reply and the other was threads standing on a list
     * owing nobody, and neither can happen now: the entry is the
     * request, it goes when the request is answered, and a thread on
     * a list with nobody owing it is the panic in the walk above.
     */
}


void
kernel_main(void)
{
    struct proc *kproc;

    cnprintf("Nyctos/" ARCH_NAME " tier %u kernel booting in %s\n",
      (unsigned int)SYS_TIER, md_mode());
#if SYS_DEBUG_CONSOLE
    /*
     * Section 22.4 requires that a build carrying the debug console say
     * so. It is ambient authority: any process can print without holding
     * anything, which is the one thing the rest of the system does not
     * allow. A production image compiles it out and this line goes with it.
     */
    cnprintf("*** debug console compiled in and %s: section 22.4 ***\n",
      SYS_DEBUG_CONSOLE_INIT ? "live" : "dormant");
#endif

    /*
     * The core running this claims its own state before anything
     * reads what it is doing, which is everything below: the scheduler
     * and the thread paths now find the running thread through the
     * register this sets rather than through a global.
     */
    cpu_init();

    object_init();
    object_check();
    object_report();

    {
        uintptr_t base, hold;
        size_t len, hold_len;

        md_memory_map(&base, &len, &hold, &hold_len);
        page_init(base, len, hold, hold_len);
        boot_reserve();
    }

#if SYS_TIER == 2
    /*
     * Translation as soon as there are frames to build tables from
     * and before anything else allocates any, because everything
     * after this line runs through the tables it builds. A page table
     * comes from somewhere, which is the ordering that matters: the
     * frame allocator is up one line above this.
     */
    vm_kernel_init();
#endif

    irq_init();
    md_intr_init();
    md_timer_init();
    time_init();

    if (irq_attach_handler(md_timer_irq(), time_interrupt) != SYS_OK) {
        panic("time: could not claim the timer interrupt");
    }

#if SYS_TIER == 2
    /*
     * And the poke one core sends another, per section 11.4. It is a
     * kernel handler and not a timeline for the same reason the timer
     * is: nothing waits for it, and what it means is decide again.
     */
    if (irq_attach_handler(md_cpu_ipi_irq(), sched_ipi) != SYS_OK) {
        panic("cores: could not claim the inter-processor interrupt");
    }
    md_intr_init_core();
#endif

    sched_init();

    kproc = proc_bootstrap("kernel");
    thread_bootstrap("boot", PRIO_BOOT, kproc);

    /*
     * The other cores, once there is a kernel process for each of them
     * to bootstrap a thread of its own in. A core arriving before that
     * has queues but nothing to be: dispatching means switching away
     * from something, and section 11.6 has idle be a state of the core
     * rather than a thread, which is a state something has to be in.
     */
    cpu_start_others();
    cpu_report();
    lock_check();
    meas_check_clock_across();
    clock_checks();
    refcount_checks();
    register_checks();
    batch_checks();
    link_checks();
#if SYS_TIER == 2
    core_checks();
#endif

    cnputs("ops: the entry point\n");
    expect("nop", run(SYS_OP_NOP, 0, NULL, 0), SYS_OK);
    expect("an opcode that is defined but unbuilt",
      run(SYS_OP_DMA_ISSUE, 0, NULL, 0), SYS_ERR_NOT_SUPPORTED);
    expect("an opcode that is not defined at all",
      run(SYS_OP_COUNT, 0, NULL, 0), SYS_ERR_INVALID_ARG);

    cnputs("debug console: the run-time gate of section 22.4\n");
    debug_console_checks();

    cnputs("frames: the memory the port says it has\n");
    page_checks();

    /*
     * The system page of section 6.2, before the count below is taken,
     * so the ledger counts it among what the port had spent rather than
     * as a frame nobody claims.
     */
    syspage_build();

    /*
     * What the port had spent before any of this ran: the kernel's own
     * image, the boot archive, and whatever the early mapping built.
     * The ledger at the end of the boot subtracts it, because none of
     * it belongs to anything that could give it back.
     */
    frames_at_start = page_used();
#if SYS_TIER == 2
    kernel_tables_at_start = md_as_frames(vm_kernel());
#endif

    cnputs("handles: authority, rights and staleness\n");
    handle_checks();

    cnputs("timelines: the four waiters of section 9.1\n");
    timeline_checks();

    cnputs("interrupts: a device advancing a timeline\n");
    md_intr_enable();

    /*
     * From here, because everything before it is a machine that has
     * not turned interrupts on yet and a figure taken across that is a
     * figure about booting.
     */
    intr_region_arm();
    interrupt_checks();

    /*
     * After the instrument is armed, which happens where interrupts
     * are first enabled. Before that there is nothing to measure and
     * the check would pass on two zeros.
     */
    batch_latency_checks();
    pays_checks();
    prepared_checks();

    cnputs("the console: an interrupt attached and handed over\n");
    console_checks();

    cnputs("timers: activation, phase and deadlines\n");
    timer_checks();

    cnputs("exit: completion timelines and join, section 10.4\n");
    completion_checks();

    cnputs("ipc: rendezvous, queue order and donation\n");
    ipc_checks();
    chain_checks();
    reply_error_checks();
    high_priority_server_checks();
    chain_bound_checks();
    inversion_checks();
    owe_checks();
    two_channel_checks();
    held_death_checks();
    owing_checks();
#if SYS_TIER == 2
    cross_core_donation_checks();
    pool_stress_checks();
#endif

    channel_count_check();

    cnputs("the slice: a file read from another process\n");
    slice_checks();

    substitution_checks();
    abort_checks();
    inflight_checks();

    cnputs("user mode: a program that can be interrupted\n");
    user_interrupt_checks();

    cnputs("spawn: a program that starts a program\n");
    spawn_checks();

    channel_count_check();

    cnputs("faults: what the hardware raises, and who dies of it\n");
    fault_checks();

    cnputs("isolation: two processes from one image\n");
    two_copies_checks();

    cnputs("threads: a program with more than one\n");
    thread_checks();

    channel_count_check();

    cnputs("the console: a driver process, and a line typed at it\n");
    console_service();
    device_checks();
    permission_checks();
    syspage_checks();
    clock_read_checks();
    clock_cost_checks();
    kill_reach_checks();
    kill_unrun_checks();
    kill_boundary_checks();
    kill_states_checks();
    kill_newborn_checks();
    kill_other_core_checks();
#if SYS_TIER == 2
    kill_faulting_checks();
#endif
    kill_server_stale_checks();

    /*
     * Everything M18 does, between two readings of the connection
     * pool. Each of these checks accounts for its own, and this is the
     * one that says so about all of them together: five servers, a
     * dozen clients and two channels of departed peers later, the pool
     * holds what it held. The number in the boot report at the end of
     * the run is larger than this because the shell session opens
     * files, and none of that is this milestone's.
     */
    {
        uint32_t before = connection_pool()->used;

        ledger_leak_checks();
        notice_absence_checks();
        ledger_identity_checks();
        notice_loss_checks();
        depth_checks();

        if (connection_pool()->used != before) {
            panic("a server: %u connections in use after all of it, "
              "started with %u",
              (unsigned int)connection_pool()->used,
              (unsigned int)before);
        }
        cnprintf("    %-38s %u, where it started\n",
          "connections in use after all of it",
          (unsigned int)connection_pool()->used);
    }

    channel_hold_checks();
    restart_checks();
    channel_endurance_checks();
    channel_checks();
#if SYS_TIER == 2
    asid_checks();
    asid_reuse_checks();
    cross_core_stale_checks();
    invalidate_cost_checks();
    flush_cost_checks();
#endif
    startup_record_checks();
    deadline_checks();

    cnputs("frames: what holds one that no process holds\n");

    {
        uint32_t a = grant_frames();

        grant_checks();

        /*
         * One frame, and it is the one grant_checks allocated before it
         * made the grant. grant_create is handed a physical address and
         * does not own it: the caller found the memory and the grant
         * describes it, so a grant's teardown has nothing to give back.
         * A device grant is the case that makes that right, its memory
         * being a chip and not a frame at all.
         */
        grant_holder("a page given away as a grant", a, 1, "the grant");
    }

    {
        uint32_t a = grant_frames();

        paging_checks();

        /*
         * The pages the pager answered faults with, still held after the
         * program that faulted has been reaped. They are its grants'
         * frames, mapped into the program at an address the pager did
         * not choose, and p_pager_bytes is what keeps stage two's reap
         * off them. This is that boundary measured: what is held is what
         * was supplied, and it is nothing at Tier 0, where a flat
         * address space never faults.
         */
        grant_holder("pages a pager supplied", a, pager_pages,
          "the pager's grants");
    }

    cnputs("the slice again, with both halves in user mode\n");
    user_slice();

    cnputs("a shell, which is what all of it was for\n");
    cnputs("namespaces: what a process was given, and its names\n");
    namespace_checks(h_fs_root, h_console);

    shell_checks(h_console, h_fs_root);

    /*
     * After the shell and not before it. This runs a program a few
     * hundred times and takes a couple of seconds doing it, and the
     * paced session feeds the shell a line every two seconds from the
     * moment the machine starts. Put in front of the shell it delays
     * it past input already sent, and the transcript loses the echo of
     * the first command typed: a check that changes what it is
     * checking. Nothing runs after this but the reports.
     */
    frame_endurance_checks();

    /*
     * The same rate over the whole run and not only over the two
     * client runs that print it. Everything that ever moved a buffer
     * is in this, including the shell session, and a path that spent
     * something other than the rule says would be caught here even if
     * nothing measured it directly.
     */
    {
        struct vm_cost all;

        vm_cost_read(&all);
        boundary_rate("the whole run", &all);

        /*
         * And the scale the buffer path has to be read against.
         *
         * Printed here rather than in the resource report, and as a
         * relation rather than as a recorded total, because the exact
         * count depends on when console input arrives: the same
         * session paced gives three hundred and ninety-five and burst
         * gives three hundred and eighty-eight. Pinning a number that
         * moves with the host would be a check that failed for
         * reasons no change caused. What does not move is that there
         * are far more of these than there are buffer moves.
         *
         * How many of them threw the whole translation cache away used
         * to be all of them, and the line used to say so. Since M19
         * stage two a port with identifiers keeps what it learned, and
         * what is printed is the pair: a number that moves with the
         * host beside a number that does not, since a port either tags
         * its translations or does not.
         *
         * If a later stage ever makes this false, that is a result
         * rather than a broken check, and it should be read as one.
         */
        cnprintf("address space switches: %u, of which %u threw every "
          "translation away, against %u buffer halves\n",
          (unsigned int)all.vc_switch,
          (unsigned int)all.vc_switch_flush,
          (unsigned int)all.vc_halves);
#if SYS_TIER == 2
        if (all.vc_switch <= all.vc_halves) {
            panic("switching stopped dominating: %u switches, %u halves",
              (unsigned int)all.vc_switch, (unsigned int)all.vc_halves);
        }

        /*
         * A port with no identifiers flushes on every one of them, and
         * a port with identifiers flushes on fewer. Not on none: a
         * switch also throws everything away when another core has
         * invalidated something this one may be holding, which is the
         * guarantee every switch used to give by accident and now
         * gives on purpose. How many that is depends on how much the
         * two cores did, so what is asserted is the relation.
         */
        if (md_asid_bits() == 0) {
            if (all.vc_switch_flush != all.vc_switch) {
                panic("%u of %u switches flushed with no identifiers",
                  (unsigned int)all.vc_switch_flush,
                  (unsigned int)all.vc_switch);
            }
        } else if (all.vc_switch_flush >= all.vc_switch) {
            panic("%u of %u switches flushed, with %u bits of "
              "identifier", (unsigned int)all.vc_switch_flush,
              (unsigned int)all.vc_switch,
              (unsigned int)md_asid_bits());
        } else if (cpu_count() > 1 && vm_maint_noted() > 0 &&
          all.vc_switch_flush == 0) {
            /*
             * And not none of them either, where there is a second core
             * whose fences do not reach this one and it did some
             * maintenance. riscv64 reports ten, which is small and is
             * not zero, and zero would mean the switch had stopped
             * noticing the other core rather than that there was
             * nothing to notice. aarch64's fences are inner-shareable
             * and reach the other core themselves, so that port notes
             * no maintenance and expects no such flush.
             */
            panic("no switch threw anything away, on %u cores with %lu "
              "invalidations to hear about", (unsigned int)cpu_count(),
              (unsigned long)vm_maint_noted());
        }

        {
            struct vm_cost per;
            uint64_t sum = 0;
            uint32_t i;

            /*
             * And where they happened. Section 11.4 makes this a
             * question worth asking: a switch is something a core
             * does, so the number above is a sum, and a sum whose
             * parts nobody looks at is a number that would go on
             * looking reasonable if one core stopped contributing.
             */
            cnprintf("    of those,");
            for (i = 0; i < cpu_count(); i++) {
                vm_cost_read_core(&per, i);
                sum += per.vc_switch;
                cnprintf("%s %u on core %u",
                  i == 0 ? "" : " and", (unsigned int)per.vc_switch,
                  (unsigned int)i);
                if (per.vc_switch == 0) {
                    panic("core %u switched nothing at all",
                      (unsigned int)i);
                }
            }
            cnprintf("\n");

            if (sum != all.vc_switch) {
                panic("the parts come to %lu and the whole says %lu",
                  (unsigned long)sum, (unsigned long)all.vc_switch);
            }
        }
#endif
    }

    /*
     * And the frames, which are the other thing a run consumes and the
     * one reaping does not give back. A slot returns to its pool when
     * nothing can ask about the process any more; the megabyte it was
     * loaded into does not, so used and held are the same number and
     * that is the point of printing both.
     *
     * Here rather than in the compared report because the two ports do
     * not spend the same: they map their kernels differently and their
     * page tables are shaped differently, so this is a number per
     * configuration where that file is one per tier.
     */
    cpu_idle_report();


    /*
     * And how stale a stored uptime would be, per M16 stage one.
     * Section 6.2 has the system page carry a field the kernel writes
     * and a program reads; this says what the writing would look like
     * in a kernel that arms the hardware for the next event and for
     * nothing else.
     */
    {
        const char *who = "nothing";
        uint64_t worst = 0;
        uint64_t marks = clock_mark_count(&worst, &who);

        cnputs("the clock: how often a stored uptime could be written\n");
        cnprintf("    %-38s %lu\n", "moments the kernel was running",
          (unsigned long)marks);
        cnprintf("    %-38s %lu ticks, about %lu us\n",
          "longest gap between two of them", (unsigned long)worst,
          (unsigned long)(worst * 1000000ULL / time_hz()));
        cnprintf("    %-38s %s\n", "what was running across it", who);

        /*
         * The count is asserted and the gap is not. How many times the
         * kernel ran is a property of what the checks ask it to do; how
         * long the longest gap was is a property of the host, which is
         * M9's rule and the reason this stage reports a duration rather
         * than checking one.
         */
        if (marks == 0) {
            panic("the clock: the kernel never ran, which cannot be");
        }
    }

    /*
     * And what the deadlines did, per M15 stage six. Counts rather than
     * durations: how many sends gave up, and which of section 12.1's
     * two states each was in, are properties of what the checks asked
     * for and are the same in every run. How long any of them waited is
     * the host's and is printed where it is measured.
     */
    cnputs("deadlines: what gave up, and from where\n");
    cnprintf("    %-38s %lu queued, %lu with a server working\n",
      "sends that gave up",
      (unsigned long)channel_gave_up_queued,
      (unsigned long)channel_gave_up_working);

    if (channel_gave_up_queued != DEADLINE_QUEUED_EXPECTED) {
        panic("deadlines: %lu gave up on the queue, not %u",
          (unsigned long)channel_gave_up_queued,
          (unsigned int)DEADLINE_QUEUED_EXPECTED);
    }
    if (channel_gave_up_working != DEADLINE_WORKING_EXPECTED) {
        panic("deadlines: %lu gave up with a server working, not %u",
          (unsigned long)channel_gave_up_working,
          (unsigned int)DEADLINE_WORKING_EXPECTED);
    }



    cnprintf("frames: %u of %u used at most, %u still held\n",
      (unsigned int)page_highwater(), (unsigned int)page_count(),
      (unsigned int)page_used());

    frame_ledger();

#if SYS_TIER == 2
    {
        uint64_t donations;
        uint64_t pokes = cpu_crossings(&donations);

        /*
         * What the whole run spent on the cores being separate. It is
         * larger than the four the placement check measured for one
         * read, which is the shell reading files through a stack that
         * is still split: nothing moved the store back, because
         * nothing moves on its own.
         */
        cnprintf("cores: %lu pokes between them, %lu donations that "
          "crossed\n", (unsigned long)pokes, (unsigned long)donations);
        if (donations < 3) {
            panic("cores: %lu donations crossed in the whole run",
              (unsigned long)donations);
        }
    }
#endif

    /*
     * Above the compared line, because it is about the machine that
     * ran rather than about what the run did, and it is the last thing
     * said before the numbers that have to be identical everywhere.
     */
    pool_report_cores();
    page_report_cores();

    intr_region_report();
    irq_dispatch_report();
    ipc_report();

    /*
     * And what a server thread owes, after the regions are reported
     * for the reason the channel walk below is placed here: this walk
     * runs with the kernel held and interrupts off, and a region
     * opened after that report is one the boot's own instrument cannot
     * see. What stands in for being measured is printing how long it
     * held, which it does.
     */
    owed_checks();

    /*
     * The one pool nothing gives back, per M20 stage one, read once
     * under the kernel lock so that all of it describes one moment.
     *
     * The walk's own duration goes here, above the compared line,
     * where a duration belongs. It is also the only place it can go:
     * the interrupts-off regions of section 13.3 are reported a few
     * lines above this, so a region opened after that report is one
     * the boot's own instrument cannot see, and printing how long it
     * held the lock is what stands in for being measured.
     */
    {
        struct channel_scan cs;

        channel_scan(&cs);
        cnprintf("channels: %u tables, %u connections, %u threads and "
          "%u slots walked with the kernel held, about %lu us, and "
          "%lu us the first time\n",
          (unsigned int)cs.cs_tables, (unsigned int)cs.cs_conns,
          (unsigned int)cs.cs_threads, (unsigned int)cs.cs_of,
          (unsigned long)(cs.cs_ns / 1000),
          (unsigned long)(channel_scan_first_ns / 1000));
        cnprintf("    %-38s %u, first of them %s\n",
          "slots nothing names and nothing frees",
          (unsigned int)cs.cs_stuck,
          cs.cs_stuck_name != NULL ? cs.cs_stuck_name : "none");
        cnprintf("    %-38s %u of %u\n",
          "slots given back from inside the kernel",
          (unsigned int)channel_freed_in_kernel,
          (unsigned int)channel_freed);
        channel_report = cs;
    }

    /*
     * Which boundary each marked thread was stopped at, per M21
     * stage three. Above the line because the split moves: the one
     * construction that reaches the first-entry boundary is a race
     * against an inter-processor interrupt, run a fixed number of
     * times, and how many of those land is the host's answer. Their
     * total is not, and that is what is compared.
     */
    cnprintf("  where a kill stopped a thread: %lu before a first "
      "entry, %lu before a later one, %lu after a batch\n",
      (unsigned long)proc_stopped_before_first,
      (unsigned long)proc_stopped_before_later,
      (unsigned long)proc_stopped_after_batch);

    object_report_usage();

    /*
     * Inside the compared part of the report, because it is a count.
     * The durations these regions also produced are above the line,
     * where they are free to differ between one host and the next.
     * This line is the milestone's rule made visible: what is compared
     * is a property of this kernel, and what moves with the machine is
     * reported and not compared.
     */
    cnprintf("  measured regions: %lu entries and %lu, %lu shared\n",
      (unsigned long)meas_count(&meas_once),
      (unsigned long)meas_count(&meas_twice),
      (unsigned long)meas_count(&meas_shared));

    /*
     * And what stopping a thread that will not stop cost, per M17.
     * Inside the compared part of the report because all three are
     * counts: a kill written, an interrupt a kill had to send to reach
     * a core, and a client told that the one thread which could have
     * answered it had gone. None of them moves with the host. How long
     * any of it took is above this line, where a duration belongs.
     */
    cnprintf("  kills: %lu written, pokes %lu, clients told %lu\n",
      (unsigned long)proc_kills, (unsigned long)proc_kill_pokes,
      (unsigned long)channel_stale_clients);

    /*
     * And what a kill did not reach, per M21 stage one. Counts, so
     * they belong here: a kill written against a thread that had
     * never run and did not end it, and the entries such a thread
     * went on to start with the mark already set. Both are what the
     * checks ask for and neither moves with the host.
     *
     * Stage one asserted the opposite of all of this, and said so:
     * a kill that reached nothing and an entry started after it were
     * the subject of the milestone. Stage two turns them over. The
     * two trampolines are counted apart because only one of them is
     * reached by anything before stage five, and a check that added
     * them together would not notice the other going missing.
     */
    cnprintf("  killed before ever running: %lu a program and "
      "%lu a thread it made, %lu entries\n",
      (unsigned long)proc_unrun_programs,
      (unsigned long)proc_unrun_threads,
      (unsigned long)marked_entries);
    cnprintf("  of those, found inside the kernel: %lu\n",
      (unsigned long)proc_unrun_locked);

    /*
     * And where a marked thread was stopped at an entry boundary,
     * per M21 stage three. Before an entry is the point M17's
     * placement did not have, and it is the one that keeps a marked
     * thread from beginning an operation that would block. After a
     * batch catches a mark written while the last entry ran.
     *
     * Counts, and the two are apart because they are different
     * moments with different consequences. A boot with nothing in
     * the first is a boot where the placement was never exercised.
     */
    cnprintf("  stopped at an entry boundary: %lu\n",
      (unsigned long)(proc_stopped_before_first +
      proc_stopped_before_later + proc_stopped_after_batch));

    /*
     * And the whole of it as one sum, per M21 stage seven.
     *
     * A kill closes the set of threads it has to reach at the moment
     * it writes the mark. Every thread in that set leaves by one of
     * three doors: the trampoline it had not been dispatched to, the
     * boundary between two entries, or the return from a trap. The
     * three counts above are the doors. This line is the claim that
     * they are all of the doors there are, which is what section
     * 10.5 says in words.
     *
     * The sum is the assertion with teeth. Each of the three can be
     * right on its own with a thread having left some other way, and
     * a fourth way out is exactly what the milestone was about: for
     * M17 and M20 there was one door, and a thread that arrived at
     * none of them was a thread a kill did not reach.
     *
     * The last number is not a door. It is a thread that was already
     * on its way out when the mark went down, which is what a thread
     * that faults and takes its cohort with it does to itself: the
     * halt kills the process it is a member of, and the kill counts
     * a thread that is inside `thread_exit` already.
     *
     * Counted where it happens rather than by taking the doors away
     * from the total, which was how this read first and was no
     * check at all: a remainder adds back up to the total whatever
     * either of them is. The five numbers come from five places
     * now, and the line above is the arithmetic between them.
     */
    {
        unsigned long doors = (unsigned long)(proc_unrun_programs +
          proc_unrun_threads + proc_stopped_before_first +
          proc_stopped_before_later + proc_stopped_after_batch +
          proc_ended_at_trap);

        if (proc_exits_while_dying != proc_reached) {
            panic("kills: %lu threads reached and %lu ended",
              (unsigned long)proc_reached,
              (unsigned long)proc_exits_while_dying);
        }
        if (doors + proc_exits_own_way != proc_reached) {
            panic("kills: %lu reached, %lu by a door and %lu their "
              "own way", (unsigned long)proc_reached, doors,
              (unsigned long)proc_exits_own_way);
        }

        cnprintf("  threads a kill had to reach: %lu, and all of "
          "them ended\n", (unsigned long)proc_reached);
        cnprintf("  by one of three doors: %lu a trampoline, %lu an "
          "entry boundary, %lu a trap, %lu already leaving\n",
          (unsigned long)(proc_unrun_programs + proc_unrun_threads),
          (unsigned long)(proc_stopped_before_first +
          proc_stopped_before_later + proc_stopped_after_batch),
          (unsigned long)proc_ended_at_trap,
          (unsigned long)proc_exits_own_way);
    }

    /*
     * And the threads a process that was over asked for and did not
     * get, per M21 stage five. A count, and the one that says the
     * set of threads a kill has to reach is closed at the moment
     * the mark is written rather than open afterwards.
     *
     * The two are apart because a process is over in two ways and
     * the reading was written for one of them. A boot with nothing
     * in the second is a boot where a process that ended on its own
     * was never asked.
     *
     * Nothing in user mode reaches either today, and that is the
     * point of the reading before an entry: a marked thread does
     * not begin the operation. What the counts stand on is the
     * check, which asks from outside the process.
     */
    cnprintf("  threads refused to a process that is over: %lu, "
      "%lu of them never marked\n",
      (unsigned long)proc_creates_refused,
      (unsigned long)proc_creates_unmarked);
    if (proc_creates_refused == 0) {
        panic("kills: a dying process was never asked for a thread");
    }
    if (proc_creates_unmarked == 0) {
        panic("kills: a process that ended was never asked for one");
    }
    if (proc_unrun_programs == 0) {
        panic("kills: nothing ended at a program's own trampoline");
    }
    if (marked_entries != 0) {
        panic("kills: %lu entries were started after the mark",
          (unsigned long)marked_entries);
    }

    /*
     * And what a server outliving its clients cost, per M18. Counts
     * again, and the same rule: a peer collected, a pulse that found
     * no slot, a sender a bounded queue would not hold. What any of it
     * took is above the line.
     *
     * The connections are the milestone's own measurement rather than
     * a cost. A departed client's connection is kept until its server
     * asks for it, so the number in use at the end of a boot is the
     * number nobody collected, and it has to be the number the boot
     * started with rather than one that grew.
     */
    cnprintf("  peers: %lu collected, pulses %lu dropped and %lu "
      "evicted\n", (unsigned long)channel_peers_collected,
      (unsigned long)channel_pulses_dropped,
      (unsigned long)channel_pulses_evicted);
    cnprintf("  senders: %lu refused, %lu displaced\n",
      (unsigned long)channel_senders_refused,
      (unsigned long)channel_senders_displaced);
    cnprintf("  connections: %u in use, %u at once\n",
      (unsigned int)connection_pool()->used,
      (unsigned int)connection_pool()->highwater);

    /*
     * And what telling address spaces apart cost, per M19.
     *
     * Counts, so they belong below the line with the rest of them: a
     * name given out, a name given out that somebody had already had,
     * a space that got none because there were none left, the spaces
     * holding a name when the boot finished, and the translations
     * thrown away when a name went back. What none of these is is a
     * duration, and what a switch or an invalidation costs on a
     * particular host is above the line, where it belongs. The two
     * counts the buffer path already reports, invalidations and
     * flushes, are on its own line further up and are not repeated.
     *
     * Neither the count of switches nor the count of them that flushed
     * is here. The first moves with the host, for the reason M6's
     * review gave: it depends on when console input arrives, and the
     * same kernel gives 5472 burst against 5476 paced. The second does
     * not move in either mode, but it differs by port, ten on riscv64
     * and none on aarch64, and there is one compared file for both
     * tiers rather than one per configuration. Both are printed and
     * asserted above the line, beside each other, where the relation
     * between them is what is checked.
     */
    /*
     * And what a boot spends on channels, per M20 stage one.
     *
     * In the compared part of the report because all four are counts.
     * What a boot spends is what its checks ask for and is the same in
     * every run at a tier; how long the walk took is above the line.
     *
     * Every other pool here ends a boot below its own high-water
     * mark, because a process, a thread, a connection, a prepared
     * program and a timer all go back. Since stage three a channel
     * does too, and the fourth number is what the free could not
     * take: slots nothing names with somebody still standing in them.
     */
    cnprintf("  channels: %u made, %u given back, %u live\n",
      (unsigned int)channel_report.cs_made,
      (unsigned int)channel_report.cs_given_back,
      (unsigned int)channel_report.cs_used);
    cnprintf("  channels at once: %u of %u, and %u restarts of a "
      "server on one slot\n",
      (unsigned int)channel_report.cs_at_once,
      (unsigned int)channel_report.cs_of,
      (unsigned int)restarts_done);
    cnprintf("  pulses that went with a channel: %u, of which %u of "
      "the reserved kind\n",
      (unsigned int)channel_pulses_discarded,
      (unsigned int)channel_notices_discarded);
    /*
     * What a server thread owes against what it records, per M22 stage
     * one. Counts, and the four here are the ones that do not move with
     * when console input arrives: the others are above the line with
     * the walk that reads them.
     *
     * The first says a receiving thread owed more replies than
     * `t_client` can name, and the second and third are the two ways
     * the record stops naming one of them. A rendezvous writes over an
     * identifier still owed, and a receive that blocks clears it
     * because after the block it is what tells a request from a pulse.
     *
     * The fourth is a channel that looked idle. `channel_free_if_idle`
     * asks whether anybody is standing in a channel by reading its
     * serving list, and a thread taken off that list while it still
     * owes a client on it leaves a client blocked on a reply with
     * nothing representing it.
     *
     * Stage one asserts the wrong answer on purpose. If a thread never
     * owes more than the record holds, the milestone has no subject and
     * should be abandoned rather than built.
     */
    cnprintf("  a server thread owed more than one reply: %lu, and at "
      "most %u at once\n", (unsigned long)channel_owed_walks,
      (unsigned int)channel_owed_most);
    cnprintf("  a record that holds one: %lu written over, and a set "
      "that kept %lu of them\n",
      (unsigned long)channel_owed_overwrote,
      (unsigned long)channel_owed_kept);
    cnprintf("  a donation given back: %lu stopped above base, %lu "
      "left to another core\n",
      (unsigned long)channel_owed_kept_prio,
      (unsigned long)channel_owed_reprio_elsewhere);
    cnprintf("  a request taken on a channel its thread was already "
      "serving another on: %lu\n", (unsigned long)channel_serving_two);
    cnprintf("  a request in flight when its client died: %lu, and a "
      "slot kept for one: %lu\n", (unsigned long)channel_held_died,
      (unsigned long)channel_held_kept_slot);

    cnprintf("  a death that reached every client it owed: %lu at "
      "once at most, and %lu left to the owner's\n",
      (unsigned long)channel_owed_death_most,
      (unsigned long)channel_owed_death_left);

    if (channel_owed_most <= 1) {
        panic("owed: no thread ever owed more than one reply");
    }
    if (channel_owed_walks == 0) {
        panic("owed: the count was never checked against a walk");
    }
    if (channel_owed_overwrote == 0) {
        panic("owed: no record was ever written over while still owed");
    }
    /*
     * And every identifier the record lost was still named by the set,
     * which is stage one's assertion turned over.
     *
     * The two are counted in different ways at the same place. One
     * tests the record and finds that it names a client this thread no
     * longer will; the other searches the list of clients for that
     * thread and finds it there. Equal says nothing went missing
     * between the record being written over and the set being asked.
     */
    if (channel_owed_kept != channel_owed_overwrote) {
        panic("owed: %lu records written over and %lu of them kept",
          (unsigned long)channel_owed_overwrote,
          (unsigned long)channel_owed_kept);
    }

    /*
     * And a donation that stopped above base because the thread still
     * owed somebody who had raised it. Zero would mean the recompute
     * never had anything left to read, and the way back to base would
     * be right for every case this boot reaches.
     *
     * Beside it, the recomputes this core could not make because the
     * thread was ready on another core. Not asserted, because skipping
     * is the right answer when it happens and the compared file is
     * what says it has not: a zero there stops being zero in a diff.
     */
    if (channel_owed_kept_prio == 0) {
        panic("owed: every donation went all the way back to base");
    }

    /*
     * And a thread inside requests on more than one channel, which
     * the entry being the request is what makes expressible. Zero
     * would mean the boot never built the case the serving list was
     * changed shape for.
     */
    if (channel_serving_two == 0) {
        panic("owed: no thread was inside requests on two channels");
    }

    /*
     * And the two halves of stage five. A client that died with its
     * request being served, and a slot the free could not take
     * because a request was standing in it. Zero for either would
     * mean the boot never built the case.
     */
    if (channel_held_died == 0) {
        panic("owed: no client died with its request being served");
    }
    if (channel_held_kept_slot == 0) {
        panic("owed: no slot was ever kept for a request in flight");
    }

    /*
     * And stage six. One death reaching two clients is the whole of
     * what the stage added, and the boot builds it on purpose, so it
     * is the same number on any host.
     *
     * The clients left for section 12.5 are compared and not
     * asserted. Nothing in the boot builds that case on purpose: it
     * comes out of where a supervisor's kills fall against a server
     * thread's exit, so an assertion here would be this stage
     * claiming something another part of the boot happens to
     * produce. The compared file is the right place for it, and it
     * has read the same in every paced and burst run so far, which
     * is what makes a change in it worth looking at.
     */
    if (channel_owed_death_most < 2) {
        panic("owed: no death ever reached more than one client");
    }


    /*
     * Asserted as well as compared, because the compared file covers
     * the paced run and the burst run is only read for a panic. A
     * number this milestone turns on should fail in both.
     *
     * Stage one asserted the opposite of both of these, and said so:
     * a pool where nothing was reachable and nothing was ever given
     * back was the subject of the milestone, and stage three is what
     * makes the assertions turn over. What is asserted now is that
     * slots come back and that the pool holds fewer than it has had
     * out at once, which is the sentence every other pool here could
     * already say.
     */
    if (channel_freed == 0) {
        panic("channels: not one slot went back in the whole boot");
    }

    /*
     * Made, given back and live have to meet, which is the one
     * statement that ties the three counters together: a channel
     * that was made either went back or is still here. The counters
     * are kept in three different places, so a mistake in any of
     * them shows up here rather than as a plausible number in the
     * report.
     */
    if (channel_report.cs_made !=
      channel_report.cs_given_back + channel_report.cs_used) {
        panic("channels: %u made against %u back and %u live",
          (unsigned int)channel_report.cs_made,
          (unsigned int)channel_report.cs_given_back,
          (unsigned int)channel_report.cs_used);
    }

    /*
     * And the figure the milestone exists for. `SYS_MAX_CHANNELS`
     * bounded how many channels a machine could ever make; what it
     * bounds now is how many exist at one time. The assertion is
     * that the boot has passed the old bound, because on the day it
     * stops doing that this line stops meaning anything.
     */
    if (channel_report.cs_made <= channel_report.cs_of) {
        panic("channels: %u made, which is no more than the %u the "
          "pool has", (unsigned int)channel_report.cs_made,
          (unsigned int)channel_report.cs_of);
    }

    /*
     * And the restarts really happened. Without this the line says
     * nothing when the check that produces it stops running: the
     * count would read zero and the slot would read zero, which is
     * a slot the pool has.
     */
    if (restarts_done != RESTART_ROUNDS) {
        panic("channels: %u restarts in the report, not %u",
          (unsigned int)restarts_done, (unsigned int)RESTART_ROUNDS);
    }
    if (restarts_slot >= channel_report.cs_of) {
        panic("channels: the restarts named slot %u of %u",
          (unsigned int)restarts_slot,
          (unsigned int)channel_report.cs_of);
    }
    if (channel_report.cs_unreachable != 0) {
        panic("channels: %u slots are free in every sense and still "
          "held", (unsigned int)channel_report.cs_unreachable);
    }

    /*
     * And nothing is waiting for a departure that will not come.
     * Stage three left one of these a boot, a channel holding a pulse
     * nobody could collect, and stage four decided that a pulse is
     * not somebody. What is left in this state now is a channel a
     * thread is inside, and the thread leaving is what frees it, so
     * at the end of a boot there should be none.
     */
    if (channel_report.cs_stuck != 0) {
        panic("channels: %u slots nothing names with somebody still "
          "inside", (unsigned int)channel_report.cs_stuck);
    }
    if (channel_report.cs_used >= channel_report.cs_at_once) {
        panic("channels: %u in use against %u at once, so nothing is "
          "being reused",
          (unsigned int)channel_report.cs_used,
          (unsigned int)channel_report.cs_at_once);
    }

#if SYS_TIER == 2
    {
        struct vm_cost all;
        uint32_t given, reused, none, live;

        vm_cost_read(&all);
        vm_asid_counts(&given, &reused, &none, &live);

        cnprintf("  names: %u given out, %u again, %u spaces without, "
          "%u live\n", (unsigned int)given, (unsigned int)reused,
          (unsigned int)none, (unsigned int)live);
        cnprintf("  names given back: %lu, each throwing away what was "
          "filed under it\n", (unsigned long)all.vc_asid_inval);
    }
#endif

    cnputs("stage 9 complete\n");
    machine_halt(0);
}
