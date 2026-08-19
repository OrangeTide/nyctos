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

#include <sys/handle.h>
#include <sys/op.h>
#include <sys/proc.h>
#include <sys/systm.h>

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
    case SYS_OP_IPC_SEND:
    case SYS_OP_IPC_RECV:
    case SYS_OP_IPC_REPLY:
    case SYS_OP_FENCE_WAIT:
    case SYS_OP_FENCE_SIGNAL:
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
