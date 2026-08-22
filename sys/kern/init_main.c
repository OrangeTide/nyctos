/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <sys/channel.h>
#include <sys/config.h>
#include <sys/connection.h>
#include <sys/fs.h>
#include <sys/grant.h>
#include <sys/handle.h>
#include <sys/intr.h>
#include <sys/object.h>
#include <sys/op.h>
#include <sys/page.h>
#include <sys/proc.h>
#include <sys/sched.h>
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

    code = child_join(&c, 1000);
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

    tl = timeline_create("spin-timer");
    if (tl == NULL) {
        panic("spin: no timeline");
    }
    t = timer_create("spin", tl, time_now() + time_from_ms(1),
      time_from_ms(1));
    if (t == NULL) {
        panic("spin: no timer");
    }

    sched_defer();
    child_watch(&c, proc_spawn_image("spin", 20, NULL, 0));
    sched_undefer();
    (void)child_join(&c, 1000);

    timer_destroy(t);

    if (md_user_irqs == before) {
        panic("nothing interrupted the program while it ran");
    }
    cnprintf("    %lu interrupts taken while a program was running\n",
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
    child_watch(&c, proc_spawn_image("spawner", 20, NULL, 0));
    sched_undefer();

    code = child_join(&c, 1000);
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
    child_watch(&c, proc_spawn_image("threads", 20, NULL, 0));
    sched_undefer();

    code = child_join(&c, 1000);
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

    ns[0].id = SYS_NS_CONSOLE;
    ns[0].handle = dup.out_handle;

    sched_defer();
    child_watch(&c, proc_spawn_image("greet", 20, ns, 1));
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
    sys_ns_spec_t ns[2];
    struct child c;
    int32_t code;

    memset(&dup, 0, sizeof(dup));
    dup.new_rights = SYS_RIGHT_SEND | SYS_RIGHT_DUPLICATE |
      SYS_RIGHT_TRANSFER;
    expect("a console for the shell",
      run(SYS_OP_HANDLE_DUP, console, &dup, sizeof(dup)), SYS_OK);
    ns[0].id = SYS_NS_CONSOLE;
    ns[0].handle = dup.out_handle;

    memset(&dup, 0, sizeof(dup));
    dup.new_rights = SYS_RIGHT_SEND | SYS_RIGHT_DUPLICATE |
      SYS_RIGHT_TRANSFER;
    expect("a directory for the shell",
      run(SYS_OP_HANDLE_DUP, root, &dup, sizeof(dup)), SYS_OK);
    ns[1].id = SYS_NS_ROOT;
    ns[1].handle = dup.out_handle;

    sched_defer();
    child_watch(&c, proc_spawn_image("sh", 20, ns, 2));
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

    (void)proc_spawn_image("console", 15, ns, 4);

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

    ns[0].id = SYS_NS_ROOT;
    ns[0].handle = root;
    cnputs("    spawning a client process with that one handle\n");
    sched_defer();
    child_watch(&c, proc_spawn("fs-client", 20, fs_client, ns, 1));
    sched_undefer();

    (void)child_join(&c, 1000);

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
static void
run_client(const char *image, sys_handle_t root, uint32_t expect_bytes,
  bool expect_send_block)
{
    struct pool *conns = connection_pool();
    sys_ns_spec_t ns[1];
    struct proc *p;
    struct child c;
    int32_t code;
    uint64_t before = channel_rendezvous;
    uint32_t pool_before;

    pool_highwater_reset(conns);
    pool_before = conns->highwater;

    trace_reset();
    trace_enable(true);

    ns[0].id = SYS_NS_ROOT;
    ns[0].handle = root;

    /*
     * Spawning and watching are one operation. The child outranks this
     * thread, so without the deferral it runs and exits inside
     * proc_spawn_image, before anything holds its completion timeline,
     * and halts the system for an exit nobody had claimed.
     */
    sched_defer();
    p = proc_spawn_image(image, 20, ns, 1);
    child_watch(&c, p);
    sched_undefer();

    code = child_join(&c, 1000);

    trace_enable(false);

    cnprintf("    %-10s %lu rendezvous, %lu connections\n", image,
      (unsigned long)(channel_rendezvous - before),
      (unsigned long)(conns->highwater - pool_before));
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
     */
    if (trace_saw(THREAD_WAIT_SEND) != expect_send_block) {
        panic("%s: send-blocking was %sexpected and %shappened", image,
          expect_send_block ? "" : "un", expect_send_block ? "never " : "");
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

    ns[0].id = SYS_NS_ROOT;
    ns[0].handle = root;

    sched_defer();
    child_watch(&c, proc_spawn_image("trespass", 20, ns, 1));
    sched_undefer();

    code = child_join(&c, 1000);

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

    if (child_join(&c1, 1000) != 7) {
        panic("two copies: the first did not run to its own exit");
    }
    handle_close(ht, h1);
#if SYS_TIER == 2
    if (child_join(&c2, 1000) != 7) {
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
    size_t n;
    int i;

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

    n = 0;
    for (i = 60; i >= 0; i -= 4) {
        unsigned int d = (unsigned int)((kernel >> i) & 0xf);

        if (n == 0 && d == 0 && i != 0) {
            continue;
        }
        addr[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    addr[n] = '\0';

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

    (void)child_join(&c, 1000);

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

    ns[0].id = SYS_NS_SERVICE;
    ns[0].handle = cargs.out_handle;
    ns[1].id = SYS_NS_PRIVATE;
    ns[1].handle = h_gate;

    sched_defer();
    child_watch(&c, proc_spawn_image("grabber", 20, ns, 2));
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

    code = child_join(&c, 1000);

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
        size_t n;
        int i;

        n = 0;
        for (i = 60; i >= 0; i -= 4) {
            unsigned int d = (unsigned int)((at >> i) & 0xf);

            if (n == 0 && d == 0 && i != 0) {
                continue;
            }
            addr[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        }
        addr[n] = '\0';

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

        (void)child_join(&c, 1000);

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

        if (child_join(&c, 1000) != -4) {
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

/* The console and the directory the shell is handed, kept from the
 * checks that created them. Root will hold both when there is a root;
 * until then the kernel is what assembles the system. */

static void
user_slice(void)
{
    struct proc *self = proc_current();
    struct sys_op_connect_args cargs;
    sys_ns_spec_t ns[1];
    struct channel *ch;
    sys_handle_t h_recv, root, root2, root3;
    sys_err_t err;

    boot_report();

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

    ns[0].id = SYS_NS_SERVICE;
    ns[0].handle = h_recv;
    cnputs("    loading fs_server, which receives on that channel\n");
    (void)proc_spawn_image("fs_server", 15, ns, 1);

    /*
     * The same file, read two ways. Section 25.7 says one message should
     * cost a third of the round trips and none of the connection pool; if
     * it does not, the section is deleted and section 12.7 with it.
     */
    kernel_buffer_checks(h_fs_root);

    run_client("fs_client", root, sizeof(motd_text) - 1, true);
    run_client("fs_batch", root2, sizeof(motd_text) - 1, false);

    trespass_checks(root3);

    move_checks();
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

    sched_init();
    kproc = proc_bootstrap("kernel");
    thread_bootstrap("boot", PRIO_BOOT, kproc);

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

    cnputs("handles: authority, rights and staleness\n");
    handle_checks();

    cnputs("timelines: the four waiters of section 9.1\n");
    timeline_checks();

    cnputs("interrupts: a device advancing a timeline\n");
    md_intr_enable();
    interrupt_checks();

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

    cnputs("the slice: a file read from another process\n");
    slice_checks();

    cnputs("user mode: a program that can be interrupted\n");
    user_interrupt_checks();

    cnputs("spawn: a program that starts a program\n");
    spawn_checks();

    cnputs("faults: what the hardware raises, and who dies of it\n");
    fault_checks();

    cnputs("isolation: two processes from one image\n");
    two_copies_checks();

    cnputs("threads: a program with more than one\n");
    thread_checks();

    cnputs("the console: a driver process, and a line typed at it\n");
    console_service();
    device_checks();

    cnputs("the slice again, with both halves in user mode\n");
    user_slice();

    cnputs("a shell, which is what all of it was for\n");
    shell_checks(h_console, h_fs_root);

    object_report_usage();
    cnputs("stage 9 complete\n");
    machine_halt(0);
}
