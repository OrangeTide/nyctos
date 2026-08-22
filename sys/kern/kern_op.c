/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The operation entry point of section 23.
 *
 * Every operation arrives here, whether from a user trap or from kernel
 * code, which is what gives each one exactly one implementation instead of
 * a fast path and a general path that have to be kept in agreement.
 *
 * Validation happens once, before anything executes, and in a fixed order:
 * the opcode, then the operation's own arguments. An operation that fails
 * validation has performed no part of itself.
 *
 * Nothing here validates the descriptor's address. At Tier 0 there is no
 * protection to validate against and a bad pointer is simply a bad pointer,
 * which section 6.1 says plainly is what the weaker tier cannot catch. The
 * check that section 23.2 describes appears with the MMU in milestone 4.
 */

#include <sys/channel.h>
#include <sys/config.h>
#include <sys/connection.h>
#include <sys/handle.h>
#include <sys/intr.h>
#include <sys/op.h>
#include <sys/proc.h>
#include <sys/thread.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/timeline.h>

#include <stdbool.h>

static sys_err_t
op_fence(struct proc *p, struct sys_op *op, bool signalling)
{
    struct sys_op_fence_args *args;
    struct timeline *tl;
    sys_err_t err;
    void *obj;

    args = (struct sys_op_fence_args *)op->arg_ptr;
    if (args == NULL || op->arg_len < sizeof(*args)) {
        return SYS_ERR_INVALID_ARG;
    }

    err = handle_lookup(&p->p_handles, op->target, SYS_OBJ_TIMELINE,
      signalling ? SYS_RIGHT_SIGNAL : SYS_RIGHT_WAIT, &obj);
    if (err != SYS_OK) {
        return err;
    }
    tl = obj;

    return signalling ? timeline_signal(tl, args->value)
      : timeline_wait(tl, args->value, args->deadline);
}

static sys_err_t
op_send(struct proc *p, struct sys_op *op)
{
    struct sys_op_send_args *args;
    struct channel *ch;
    sys_err_t err;
    void *obj;

    args = (struct sys_op_send_args *)op->arg_ptr;
    if (args == NULL || op->arg_len < sizeof(*args)) {
        return SYS_ERR_INVALID_ARG;
    }

    /*
     * A send names either a connection, which is the ordinary case and
     * carries the server side state with it, or the channel itself, which
     * a holder of the raw channel may do and which carries no state.
     */
    err = handle_lookup(&p->p_handles, op->target, SYS_OBJ_CONNECTION,
      SYS_RIGHT_SEND, &obj);
    if (err == SYS_OK) {
        struct connection *cn = obj;

        return channel_send(cn->cn_channel, cn->cn_cookie, &args->request,
          &args->reply);
    }
    if (err != SYS_ERR_WRONG_TYPE) {
        return err;
    }

    err = handle_lookup(&p->p_handles, op->target, SYS_OBJ_CHANNEL,
      SYS_RIGHT_SEND, &obj);
    if (err != SYS_OK) {
        return err;
    }
    ch = obj;

    return channel_send(ch, 0, &args->request, &args->reply);
}

static sys_err_t
op_recv(struct proc *p, struct sys_op *op)
{
    struct sys_op_recv_args *args;
    struct channel *ch;
    sys_err_t err;
    void *obj;

    args = (struct sys_op_recv_args *)op->arg_ptr;
    if (args == NULL || op->arg_len < sizeof(*args)) {
        return SYS_ERR_INVALID_ARG;
    }

    err = handle_lookup(&p->p_handles, op->target, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV, &obj);
    if (err != SYS_OK) {
        return err;
    }
    ch = obj;

    return channel_recv(ch, &args->message, &args->peer, &args->cookie);
}

/*
 * A reply names a thread rather than a channel, so it takes no handle and
 * checks no right. The token is not authority: a server got it by being
 * sent to, and the only thing it permits is answering that one request.
 */
static sys_err_t
op_reply(struct proc *p, struct sys_op *op)
{
    struct sys_op_reply_args *args;

    (void)p;

    args = (struct sys_op_reply_args *)op->arg_ptr;
    if (args == NULL || op->arg_len < sizeof(*args)) {
        return SYS_ERR_INVALID_ARG;
    }

    return channel_reply(args->peer, &args->reply);
}

/*
 * Connect to a channel. The cookie is the server naming its own state for
 * this connection, so this is a server-side operation: a client receives a
 * connection rather than making one.
 */
static sys_err_t
op_chan_connect(struct proc *p, struct sys_op *op)
{
    struct sys_op_connect_args *args;
    struct connection *cn;
    sys_err_t err;
    void *obj;

    args = (struct sys_op_connect_args *)op->arg_ptr;
    if (args == NULL || op->arg_len < sizeof(*args)) {
        return SYS_ERR_INVALID_ARG;
    }

    err = handle_lookup(&p->p_handles, op->target, SYS_OBJ_CHANNEL,
      SYS_RIGHT_RECV, &obj);
    if (err != SYS_OK) {
        return err;
    }

    cn = connection_create(obj, args->cookie);
    if (cn == NULL) {
        return SYS_ERR_NO_MEMORY;
    }

    err = handle_insert(&p->p_handles, cn, SYS_OBJ_CONNECTION, args->rights,
      &args->out_handle);
    if (err != SYS_OK) {
        connection_destroy(cn);
    }

    return err;
}

static sys_err_t
op_handle_close(struct proc *p, struct sys_op *op)
{
    return handle_close(&p->p_handles, op->target);
}

static sys_err_t
op_handle_dup(struct proc *p, struct sys_op *op)
{
    struct sys_op_dup_args *args;
    sys_handle_t out;
    sys_err_t err;

    args = (struct sys_op_dup_args *)op->arg_ptr;
    if (args == NULL || op->arg_len < sizeof(*args)) {
        return SYS_ERR_INVALID_ARG;
    }

    err = handle_dup(&p->p_handles, op->target, args->new_rights, &out);
    if (err != SYS_OK) {
        return err;
    }

    args->out_handle = out;

    return SYS_OK;
}

/*
 * The debug console of section 22.4.
 *
 * A write that takes no handle, checks nothing about its caller, and goes
 * straight to the kernel's console. It is ambient authority, which is the
 * one thing the rest of this system does not have, and it is permanent
 * rather than scaffolding: a diagnostic that exists only in development
 * images is one that is missing exactly when a production fault needs it.
 *
 * Two gates instead of deletion. SYS_DEBUG_CONSOLE decides whether any of
 * this is compiled in. Where it is, a run-time flag decides whether the
 * operation does anything, so an image can ship with the console dormant
 * and have it turned on without becoming a different image.
 *
 * Both closed states return SYS_ERR_NOT_SUPPORTED and are deliberately
 * indistinguishable. A caller's correct behaviour is the same either way,
 * which is to carry on silently, and a program that could tell them apart
 * would be learning something about the build that it has no business
 * depending on. Which state a kernel is in is in its boot log, where the
 * person debugging can see it and the program cannot.
 */
#if SYS_DEBUG_CONSOLE
static bool debug_console_live = SYS_DEBUG_CONSOLE_INIT;

/*
 * The switch. It is a kernel-internal call and not an operation, because
 * an operation that opened this would be a hole any process could open for
 * itself, which is not a gate. Section 24.3's supervisor is what should
 * eventually drive it; until root exists, the kernel does.
 */
void
debug_console_set(bool live)
{
    debug_console_live = live;
}

bool
debug_console_is_live(void)
{
    return debug_console_live;
}
#endif

static sys_err_t
op_debug_write(struct proc *p, struct sys_op *op)
{
#if SYS_DEBUG_CONSOLE
    struct sys_op_debug_args *args;

    if (!debug_console_live) {
        return SYS_ERR_NOT_SUPPORTED;
    }

    args = (struct sys_op_debug_args *)op->arg_ptr;
    if (args == NULL || op->arg_len < sizeof(*args)) {
        return SYS_ERR_INVALID_ARG;
    }
    if (args->buf == 0) {
        return SYS_ERR_INVALID_ARG;
    }
    if (args->len > SYS_DEBUG_WRITE_MAX) {
        return SYS_ERR_INVALID_ARG;
    }

    /*
     * And the bytes themselves, per section 23.2. The debug console
     * takes no handle, so this pointer is the only thing it is given
     * and the only thing there is to check.
     */
    if (!proc_reaches(p, args->buf, args->len)) {
        return SYS_ERR_FAULT;
    }

    cnwrite((const char *)args->buf, args->len);

    return SYS_OK;
#else
    (void)p;
    (void)op;

    return SYS_ERR_NOT_SUPPORTED;
#endif
}

/*
 * Exit, per section 10.4. The thread advances its own completion
 * timeline and the last thread of a process advances the process one,
 * both inside thread_exit, and the status stays readable through a
 * handle to the object because nothing is reaped.
 *
 * TODO: milestone 7 is where this becomes death. A thread that exits
 * keeps its pool slot for ever, nothing is killed and nothing is
 * cancelled, which is deliberate: what a shell needs is a program
 * that stops and a parent that hears about it.
 */
static sys_err_t
op_thread_exit(struct proc *p, struct sys_op *op)
{
    struct sys_op_exit_args *args;

    args = (struct sys_op_exit_args *)op->arg_ptr;
    if (args == NULL || op->arg_len < sizeof(*args)) {
        return SYS_ERR_INVALID_ARG;
    }

    cnprintf("    %s exited with status %d\n", p->p_name, args->code);

    thread_exit(args->code);
}

/*
 * The completion timeline of a thread or a process, per section 10.4.
 *
 * The handle carries SYS_RIGHT_WAIT and nothing else, and the kernel keeps
 * the only means of advancing one: a process able to signal its own
 * completion could fake its own exit, and whatever supervises it would
 * reclaim it while it was still running.
 *
 * SYS_RIGHT_WAIT is what this asks for, and spawn is what hands it out.
 * A parent that passes a child on to something that should be able to
 * hear about the exit but not to read its status duplicates the handle
 * with one right and not the other, which is section 7.2 working
 * normally rather than a rule about processes.
 */
static sys_err_t
op_object_completion(struct proc *p, struct sys_op *op)
{
    struct sys_op_completion_args *args;
    struct timeline *tl;
    void *obj;
    sys_err_t err;

    args = (struct sys_op_completion_args *)op->arg_ptr;
    if (args == NULL || op->arg_len < sizeof(*args)) {
        return SYS_ERR_INVALID_ARG;
    }

    err = handle_lookup(&p->p_handles, op->target, SYS_OBJ_PROCESS,
      SYS_RIGHT_WAIT, &obj);
    if (err == SYS_OK) {
        tl = proc_completion(obj);
    } else if (err == SYS_ERR_WRONG_TYPE) {
        err = handle_lookup(&p->p_handles, op->target, SYS_OBJ_THREAD,
          SYS_RIGHT_WAIT, &obj);
        if (err != SYS_OK) {
            return err;
        }
        tl = &((struct thread *)obj)->t_completion;
    } else {
        return err;
    }

    return handle_insert(&p->p_handles, tl, SYS_OBJ_TIMELINE,
      SYS_RIGHT_WAIT, &args->out_handle);
}

/*
 * The exit code, which a monotonic counter cannot carry. Section 10.4 has
 * it read from the object rather than from the timeline that reports the
 * finishing, and the object keeps it because nothing is reaped.
 */
static sys_err_t
op_proc_status(struct proc *p, struct sys_op *op)
{
    struct sys_op_status_args *args;
    struct proc *target;
    void *obj;
    sys_err_t err;

    args = (struct sys_op_status_args *)op->arg_ptr;
    if (args == NULL || op->arg_len < sizeof(*args)) {
        return SYS_ERR_INVALID_ARG;
    }

    err = handle_lookup(&p->p_handles, op->target, SYS_OBJ_PROCESS,
      SYS_RIGHT_STAT, &obj);
    if (err != SYS_OK) {
        return err;
    }
    target = obj;

    args->out_code = target->p_exited ? target->p_exit_code : 0;
    /*
     * A process is running or it has finished. The three live states
     * belong to a thread, and a process with several threads is in all
     * of them at once.
     */
    args->out_state = target->p_exited ? SYS_STATE_EXITED
      : SYS_STATE_RUNNING;

    /*
     * Section 16.2: a fault is an exit with a reason, and this is
     * where the reason is read. A process that ended any other way
     * reports SYS_FAULT_NONE, which is what its fields already hold.
     */
    args->out_fault = target->p_fault;
    args->out_reserved = 0;
    args->out_fault_addr = target->p_fault_addr;
    args->out_fault_pc = target->p_fault_pc;

    return SYS_OK;
}

/*
 * A driver has drained its device, so the source may assert again.
 *
 * There are no arguments. What the operation needs is the interrupt, and
 * an interrupt is a handle rather than a number here, which is what stops
 * a program acknowledging one it was never given.
 */
static sys_err_t
op_irq_unmask(struct proc *p, struct sys_op *op)
{
    void *obj;
    sys_err_t err;

    err = handle_lookup(&p->p_handles, op->target, SYS_OBJ_IRQ,
      SYS_RIGHT_IRQ, &obj);
    if (err != SYS_OK) {
        return err;
    }

    return irq_unmask(obj);
}

/*
 * Create a process, per section 10.1, with the handles the caller names
 * moved into it, per section 20.4.
 *
 * The name is copied out of the caller's structure and terminated here
 * rather than trusted to be terminated there. Nothing else in this
 * interface is a string, and the one that is arrives from a program.
 */
static sys_err_t
op_proc_spawn(struct proc *p, struct sys_op *op)
{
    struct sys_op_spawn_args *args;
    char image[SYS_IMAGE_NAME_MAX + 1];
    sys_handle_t h = SYS_HANDLE_INVALID;
    uint32_t prio;
    sys_err_t err;

    args = (struct sys_op_spawn_args *)op->arg_ptr;
    if (args == NULL || op->arg_len < sizeof(*args)) {
        return SYS_ERR_INVALID_ARG;
    }

    prio = args->prio;
    if (prio == SYS_PRIO_INHERIT) {
        prio = thread_current()->t_base_prio;
    }
    if (prio > SYS_PRIO_MAX) {
        return SYS_ERR_INVALID_ARG;
    }

    memcpy(image, args->image, SYS_IMAGE_NAME_MAX);
    image[SYS_IMAGE_NAME_MAX] = '\0';

    err = proc_spawn_op(p, image, (uint8_t)prio,
      (const sys_ns_spec_t *)args->ns, args->nns,
      (const char *const *)args->argv, args->argc, &h);
    if (err != SYS_OK) {
        return err;
    }

    args->out_handle = h;

    return SYS_OK;
}

/*
 * A thread the program created, per section 10.2.
 */
static sys_err_t
op_thread_create(struct proc *p, struct sys_op *op)
{
    struct sys_op_thread_args *args;
    sys_handle_t h = SYS_HANDLE_INVALID;
    uint32_t prio;
    sys_err_t err;

    args = (struct sys_op_thread_args *)op->arg_ptr;
    if (args == NULL || op->arg_len < sizeof(*args)) {
        return SYS_ERR_INVALID_ARG;
    }

    prio = args->prio;
    if (prio == SYS_PRIO_INHERIT) {
        prio = thread_current()->t_base_prio;
    }
    if (prio > SYS_PRIO_MAX) {
        return SYS_ERR_INVALID_ARG;
    }

    err = proc_thread_create(p, args->entry, args->arg, args->stack,
      (uint8_t)prio, &h);
    if (err != SYS_OK) {
        return err;
    }

    args->out_handle = h;

    return SYS_OK;
}

/*
 * The exit code of a thread. The same structure as a process status and
 * for the same reason: a counter cannot carry a code, so the code is read
 * from the object rather than from the timeline that reports the finish.
 */
static sys_err_t
op_thread_status(struct proc *p, struct sys_op *op)
{
    struct sys_op_status_args *args;
    struct thread *t;
    void *obj;
    sys_err_t err;

    args = (struct sys_op_status_args *)op->arg_ptr;
    if (args == NULL || op->arg_len < sizeof(*args)) {
        return SYS_ERR_INVALID_ARG;
    }

    err = handle_lookup(&p->p_handles, op->target, SYS_OBJ_THREAD,
      SYS_RIGHT_STAT, &obj);
    if (err != SYS_OK) {
        return err;
    }
    t = obj;

    args->out_code = t->t_state == THREAD_DEAD ? t->t_exit_code : 0;

    args->out_fault = t->t_fault;
    args->out_reserved = 0;
    args->out_fault_addr = t->t_fault_addr;
    args->out_fault_pc = t->t_fault_pc;

    switch (t->t_state) {
    case THREAD_RUNNING:
        args->out_state = SYS_STATE_RUNNING;
        break;
    case THREAD_BLOCKED:
        args->out_state = SYS_STATE_BLOCKED;
        break;
    case THREAD_DEAD:
        args->out_state = SYS_STATE_EXITED;
        break;
    default:
        args->out_state = SYS_STATE_READY;
        break;
    }

    return SYS_OK;
}
sys_err_t
sys_op_exec(struct sys_op *op)
{
    struct proc *p;
    sys_err_t err;

    /*
     * The one path that returns without writing a status, because there is
     * nowhere to write it. Every other failure leaves the descriptor
     * carrying the same value this returns.
     */
    if (op == NULL) {
        return SYS_ERR_FAULT;
    }

    p = proc_current();

    /*
     * Section 23.2 step 2 again, for the memory the descriptor points at
     * rather than the descriptor itself. Every operation below reads its
     * arguments through this pointer and most write results back through
     * it, so one check here is the "once, before execution" the section
     * asks for: the alternative is the same check written into thirty
     * operations, twenty-nine of which would have it right.
     *
     * A null pointer with a length is a malformed descriptor rather than
     * an unreachable one, and each operation already says so with
     * SYS_ERR_INVALID_ARG. Letting it through to them keeps that answer.
     */
    if (op->arg_ptr != 0 &&
      !proc_reaches(p, op->arg_ptr, op->arg_len)) {
        op->status = SYS_ERR_FAULT;
        return SYS_ERR_FAULT;
    }

    switch (op->opcode) {
    case SYS_OP_NOP:
        /*
         * Does nothing, deliberately. It exercises the entry, the opcode
         * check, the status write and the return with no other machinery
         * involved, which makes it the first operation worth having and the
         * one to reach for when the path itself is suspect.
         */
        err = SYS_OK;
        break;
    case SYS_OP_HANDLE_CLOSE:
        err = op_handle_close(p, op);
        break;
    case SYS_OP_HANDLE_DUP:
        err = op_handle_dup(p, op);
        break;
    case SYS_OP_OBJECT_COMPLETION:
        err = op_object_completion(p, op);
        break;
    case SYS_OP_THREAD_CREATE:
        err = op_thread_create(p, op);
        break;
    case SYS_OP_THREAD_STATUS:
        err = op_thread_status(p, op);
        break;
    case SYS_OP_PROC_SPAWN:
        err = op_proc_spawn(p, op);
        break;
    case SYS_OP_IRQ_UNMASK:
        err = op_irq_unmask(p, op);
        break;
    case SYS_OP_PROC_STATUS:
        err = op_proc_status(p, op);
        break;
    case SYS_OP_THREAD_EXIT:
        err = op_thread_exit(p, op);
        break;
    case SYS_OP_DEBUG_WRITE:
        err = op_debug_write(p, op);
        break;
    case SYS_OP_CHAN_CONNECT:
        err = op_chan_connect(p, op);
        break;
    case SYS_OP_FENCE_WAIT:
        err = op_fence(p, op, false);
        break;
    case SYS_OP_FENCE_SIGNAL:
        err = op_fence(p, op, true);
        break;
    case SYS_OP_IPC_SEND:
        err = op_send(p, op);
        break;
    case SYS_OP_IPC_RECV:
        err = op_recv(p, op);
        break;
    case SYS_OP_IPC_REPLY:
        err = op_reply(p, op);
        break;
    case SYS_OP_DMA_ISSUE:
    case SYS_OP_READ_ISSUE:
    case SYS_OP_WRITE_ISSUE:
        /* Defined, and not built yet. Distinct from an unknown opcode. */
        err = SYS_ERR_NOT_SUPPORTED;
        break;
    default:
        err = SYS_ERR_INVALID_ARG;
        break;
    }

    /*
     * Section 23.3: the status goes into the descriptor and is also
     * returned. The duplication is deliberate, because the register is what
     * a C wrapper returns and the field is what survives when the
     * descriptor is one of many.
     */
    op->status = err;

    return err;
}
