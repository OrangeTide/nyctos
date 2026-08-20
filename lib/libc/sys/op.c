/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The one system call, and the wrappers that fill in a descriptor for it.
 *
 * Every function here builds a sys_op_t and submits it. There is no second
 * path, which is the property section 23.1 exists for: an operation has
 * one implementation whether it was reached from a wrapper, from a batch,
 * or from inside the kernel.
 *
 * The argument structures are on the caller's stack and the kernel writes
 * its results back into them, so each wrapper copies out what its caller
 * asked for before returning.
 */

#include <os/sys.h>

#include <stddef.h>
#include <string.h>

long __sysop(sys_op_t *ops, unsigned long count);

static sys_err_t
submit(uint16_t opcode, sys_handle_t target, void *args, uint32_t len)
{
    sys_op_t op;

    memset(&op, 0, sizeof(op));
    op.opcode = opcode;
    op.target = target;
    op.arg_ptr = (uintptr_t)args;
    op.arg_len = len;

    return (sys_err_t)__sysop(&op, 1);
}

sys_err_t
sys_op_submit(sys_op_t *ops, unsigned long count)
{
    return (sys_err_t)__sysop(ops, count);
}

sys_err_t
sys_msg_send(sys_handle_t target, const sys_msg_t *request, sys_msg_t *reply)
{
    sys_op_send_args_t args;
    sys_err_t err;

    memset(&args, 0, sizeof(args));
    args.request = *request;

    err = submit(SYS_OP_IPC_SEND, target, &args, sizeof(args));
    if (reply != NULL) {
        *reply = args.reply;
    }

    return err;
}

sys_err_t
sys_msg_recv(sys_handle_t chan, sys_msg_t *message, sys_thread_id_t *peer,
  uint64_t *cookie)
{
    sys_op_recv_args_t args;
    sys_err_t err;

    memset(&args, 0, sizeof(args));

    err = submit(SYS_OP_IPC_RECV, chan, &args, sizeof(args));
    if (message != NULL) {
        *message = args.message;
    }
    if (peer != NULL) {
        *peer = args.peer;
    }
    if (cookie != NULL) {
        *cookie = args.cookie;
    }

    return err;
}

sys_err_t
sys_msg_reply(sys_thread_id_t peer, const sys_msg_t *reply)
{
    sys_op_reply_args_t args;

    memset(&args, 0, sizeof(args));
    args.peer = peer;
    args.reply = *reply;

    /*
     * A reply names its target in the argument structure rather than in
     * the descriptor, because a thread id is not a handle: it confers no
     * authority and permits exactly one thing, which is completing the
     * request the client is blocked in. See section 12.2.
     */
    return submit(SYS_OP_IPC_REPLY, SYS_HANDLE_INVALID, &args, sizeof(args));
}

sys_err_t
sys_chan_connect(sys_handle_t chan, uint64_t cookie, uint32_t rights,
  sys_handle_t *out_conn)
{
    sys_op_connect_args_t args;
    sys_err_t err;

    memset(&args, 0, sizeof(args));
    args.cookie = cookie;
    args.rights = rights;

    err = submit(SYS_OP_CHAN_CONNECT, chan, &args, sizeof(args));
    if (out_conn != NULL) {
        *out_conn = err == SYS_OK ? args.out_handle : SYS_HANDLE_INVALID;
    }

    return err;
}

sys_err_t
sys_handle_close(sys_handle_t handle)
{
    return submit(SYS_OP_HANDLE_CLOSE, handle, NULL, 0);
}

sys_err_t
sys_handle_dup(sys_handle_t handle, uint32_t new_rights,
  sys_handle_t *out_handle)
{
    sys_op_dup_args_t args;
    sys_err_t err;

    memset(&args, 0, sizeof(args));
    args.new_rights = new_rights;

    err = submit(SYS_OP_HANDLE_DUP, handle, &args, sizeof(args));
    if (out_handle != NULL) {
        *out_handle = err == SYS_OK ? args.out_handle : SYS_HANDLE_INVALID;
    }

    return err;
}
