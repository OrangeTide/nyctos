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
#include <sys/handle.h>
#include <sys/op.h>
#include <sys/proc.h>
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

    err = handle_lookup(&p->p_handles, op->target, SYS_OBJ_CHANNEL,
      SYS_RIGHT_SEND, &obj);
    if (err != SYS_OK) {
        return err;
    }
    ch = obj;

    return channel_send(ch, &args->request, &args->reply);
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

    return channel_recv(ch, &args->message, &args->peer);
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
