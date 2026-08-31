/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <sys/measure.h>
#include <sys/lock.h>
#include <sys/blk.h>
#include <sys/bootarch.h>
#include <sys/channel.h>
#include <sys/cohort.h>
#include <sys/cpu.h>
#include <sys/config.h>
#include <sys/connection.h>
#include <sys/fs.h>
#include <sys/grant.h>
#include <sys/handle.h>
#include <sys/intr.h>
#include <sys/object.h>
#include <sys/page.h>
#include <sys/op.h>
#include <sys/page.h>
#include <sys/peer.h>
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
    t = timer_create("spin", tl, time_now() + time_from_ms(1),
      time_from_ms(1));
    if (t == NULL) {
        panic("spin: no timer");
    }

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
        sched_undefer();
        (void)child_join(&c, JOIN_MS);
    }

    timer_destroy(t);

    if (md_user_irqs == before) {
        panic("nothing interrupted a program in %u runs of one",
          (unsigned int)tries);
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

        cnprintf("    %-10s %lu rendezvous, %lu connections, "
          "%lu entries, %lu invalidations\n", image,
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
    uint32_t i, users, systems;

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
    for (;;) {
        const sys_pulse_t *p;

        if (channel_recv(ch, &got, &peer, &cookie) != SYS_OK) {
            panic("pulse: receiving one failed");
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
              channel_send(ch, 0, &req, &rep), SYS_OK);
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
     * The window is the core and not the batch: anything else this
     * core does between the clearing and the reading is in it, an
     * interrupt included. That makes the figure an upper bound on what
     * the batch cost rather than the batch's cost, which is the safe
     * direction for the question being asked, because contamination
     * can only make the one-entry figure larger and the growth harder
     * to see.
     */
    base = timeline_value(tl);
    batch_fill_on(ops, args, h, 1, base);
    intr_region_reset();
    (void)run_batch(ops, 1);
    one = intr_region_worst(&site_one);

    base = timeline_value(tl);
    batch_fill_on(ops, args, h, SYS_BATCH_MAX_ENTRIES, base);
    intr_region_reset();
    (void)run_batch(ops, SYS_BATCH_MAX_ENTRIES);
    many = intr_region_worst(&site_many);

    cnprintf("    one entry held them off %lu ticks, from 0x%lx\n",
      (unsigned long)one, (unsigned long)site_one);
    cnprintf("    %u entries held them off %lu ticks, from 0x%lx\n",
      (unsigned int)SYS_BATCH_MAX_ENTRIES, (unsigned long)many,
      (unsigned long)site_many);

    /*
     * What is asserted is a relation and not a threshold. If the batch
     * is interruptible at every entry boundary then the longest region
     * inside it is one entry's worth however many entries there are,
     * and the two figures are the same size. If it is not, the second
     * grows with the count and section 13.3 is wrong about this
     * implementation.
     *
     * Reported either way. Which of the two happens is the result of
     * this stage and it is not the check's business to have an opinion.
     */
    /*
     * No measurement is not a result. Both figures being zero means
     * nothing was recorded, which happens if this runs before the
     * instrument is armed, and reporting that as the claim holding is
     * how a check comes to pass on no data at all. It did, the first
     * time this was run.
     */
    if (one == 0 || many == 0) {
        panic("latency: nothing was measured, %lu and %lu ticks",
          (unsigned long)one, (unsigned long)many);
    }

    if (many > one * 4) {
        cnprintf("    the bound does appear in the figure     "
          "%lu times\n", (unsigned long)(many / one));

#if SYS_TIER == 0
        /*
         * Asserted at this tier and reported at the other. Section
         * 13.3's claim holds here, so a run where it stops holding is
         * a regression and should say so; it does not hold at Tier 2,
         * where the kernel lock is held across the whole call, and
         * asserting the broken behaviour would mean failing the day
         * somebody fixed it.
         *
         * A factor of four between two figures measured seconds apart
         * in one run is about as safe as a duration comparison gets.
         * The two are 62 and 55 ticks here, so the margin is an order
         * of magnitude wider than the effect.
         */
        panic("latency: the batch bound entered the figure, %lu against %lu",
          (unsigned long)many, (unsigned long)one);
#endif
    } else {
        cnputs("    and the bound does not appear in it     OK\n");
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
    sys_handle_t h_recv, h_blk, store, root, root2, root3;
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
    kernel_buffer_checks(h_fs_root);

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

        pinned_storage_checks(blk_proc, root4, p1 - p0, d1 - d0);
    }
#else
    run_client("fs_client", root, BLK_MOTD_BYTES, SEND_BLOCK_YES);
    run_client("fs_batch", root2, BLK_MOTD_BYTES, SEND_BLOCK_NO);
#endif

    trespass_checks(root3);

    move_checks();
    reap_checks();
    cancel_checks();
    pulse_checks();
    death_checks();
    kill_checks();
    cohort_checks();
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
    clock_checks();
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
#if SYS_TIER == 2
    cross_core_donation_checks();
    pool_stress_checks();
#endif

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
    grant_checks();
    paging_checks();

    cnputs("the slice again, with both halves in user mode\n");
    user_slice();

    cnputs("a shell, which is what all of it was for\n");
    cnputs("namespaces: what a process was given, and its names\n");
    namespace_checks(h_fs_root, h_console);

    shell_checks(h_console, h_fs_root);

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
         * are far more of these than there are buffer moves, and each
         * of them discards the whole translation cache because
         * neither port assigns address space identifiers.
         *
         * If a later stage ever makes this false, that is a result
         * rather than a broken check, and it should be read as one.
         */
        cnprintf("address space switches: %u, each a whole-TLB flush, "
          "against %u buffer halves\n", (unsigned int)all.vc_switch,
          (unsigned int)all.vc_halves);
#if SYS_TIER == 2
        if (all.vc_switch <= all.vc_halves) {
            panic("switching stopped dominating: %u switches, %u halves",
              (unsigned int)all.vc_switch, (unsigned int)all.vc_halves);
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

    cnprintf("frames: %u of %u used at most, %u still held\n",
      (unsigned int)page_highwater(), (unsigned int)page_count(),
      (unsigned int)page_used());

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

    intr_region_report();
    irq_dispatch_report();
    ipc_report();

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

    cnputs("stage 9 complete\n");
    machine_halt(0);
}
