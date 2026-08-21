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

void
sys_thread_exit(int32_t code)
{
    sys_op_exit_args_t args;

    memset(&args, 0, sizeof(args));
    args.code = code;

    (void)submit(SYS_OP_THREAD_EXIT, SYS_HANDLE_INVALID, &args,
      sizeof(args));

    /*
     * Only reached if the kernel refused the operation, which it does for
     * a malformed descriptor. There is nothing useful to do about it and
     * nowhere to report it, so stop here rather than returning into a
     * caller that has already given up its stack frame.
     */
    for (;;) {
    }
}

sys_err_t
sys_debug_write(const void *buf, uint32_t len)
{
    sys_op_debug_args_t args;

    memset(&args, 0, sizeof(args));
    args.buf = (uintptr_t)buf;
    args.len = len;

    return submit(SYS_OP_DEBUG_WRITE, SYS_HANDLE_INVALID, &args,
      sizeof(args));
}

sys_err_t
sys_proc_spawn(const char *image, uint32_t prio, const sys_ns_spec_t *ns,
  uint32_t nns, sys_handle_t *out_proc)
{
    sys_op_spawn_args_t args;
    sys_err_t err;
    uint32_t i;

    memset(&args, 0, sizeof(args));

    /*
     * Copied rather than pointed at, and truncated here rather than in the
     * kernel: the field is the interface, so a name too long to fit is the
     * caller's problem and is refused as one.
     */
    for (i = 0; i < SYS_IMAGE_NAME_MAX && image[i] != '\0'; i++) {
        args.image[i] = image[i];
    }
    if (i == SYS_IMAGE_NAME_MAX && image[i] != '\0') {
        return SYS_ERR_INVALID_ARG;
    }

    args.prio = prio;
    args.nns = nns;
    args.ns = (uintptr_t)ns;

    err = submit(SYS_OP_PROC_SPAWN, SYS_HANDLE_INVALID, &args,
      sizeof(args));
    if (out_proc != NULL) {
        *out_proc = err == SYS_OK ? args.out_handle : SYS_HANDLE_INVALID;
    }

    return err;
}

sys_err_t
sys_object_completion(sys_handle_t obj, sys_handle_t *out_timeline)
{
    sys_op_completion_args_t args;
    sys_err_t err;

    memset(&args, 0, sizeof(args));

    err = submit(SYS_OP_OBJECT_COMPLETION, obj, &args, sizeof(args));
    if (out_timeline != NULL) {
        *out_timeline = err == SYS_OK ? args.out_handle
          : SYS_HANDLE_INVALID;
    }

    return err;
}

sys_err_t
sys_proc_status(sys_handle_t proc, int32_t *out_code, uint32_t *out_state)
{
    sys_op_status_args_t args;
    sys_err_t err;

    memset(&args, 0, sizeof(args));

    err = submit(SYS_OP_PROC_STATUS, proc, &args, sizeof(args));
    if (out_code != NULL) {
        *out_code = args.out_code;
    }
    if (out_state != NULL) {
        *out_state = args.out_state;
    }

    return err;
}

sys_err_t
sys_fence_wait(sys_handle_t timeline, uint64_t value, uint64_t deadline)
{
    sys_op_fence_args_t args;

    memset(&args, 0, sizeof(args));
    args.value = value;
    args.deadline = deadline;

    return submit(SYS_OP_FENCE_WAIT, timeline, &args, sizeof(args));
}

/*
 * What a created thread actually starts in.
 *
 * The operation carries one word to the new thread and a thread needs two,
 * a function and its argument, so the pair is written to the top of the
 * new stack and the trampoline is what unpacks it. It also gives the
 * function somewhere to return to: a thread that runs off the end of its
 * entry function has no return address at all, and this turns that into an
 * ordinary exit.
 */
struct thread_start {
    void  (*fn)(void *);
    void   *arg;
};

static void
thread_trampoline(void *p)
{
    struct thread_start *start = p;

    start->fn(start->arg);

    sys_thread_exit(0);
}

sys_err_t
sys_thread_create(void (*fn)(void *), void *arg, void *stack, size_t size,
  uint32_t prio, sys_handle_t *out_thread)
{
    sys_op_thread_args_t args;
    struct thread_start *start;
    uintptr_t top;
    sys_err_t err;

    if (fn == NULL || stack == NULL || size < sizeof(*start) + 64) {
        return SYS_ERR_INVALID_ARG;
    }

    /*
     * The pair lives at the top of the new stack and the stack pointer
     * starts at it, which is safe because both ports push below the
     * stack pointer and neither writes above it. Nothing else can hold
     * the pair: there is no allocator in this library, and a static
     * would make two threads share one.
     */
    top = ((uintptr_t)stack + size) & ~(uintptr_t)15;
    top -= sizeof(*start);
    top &= ~(uintptr_t)15;
    start = (struct thread_start *)top;
    start->fn = fn;
    start->arg = arg;

    memset(&args, 0, sizeof(args));
    args.entry = (uintptr_t)thread_trampoline;
    args.arg = (uintptr_t)start;
    args.stack = top;
    args.prio = prio;

    err = submit(SYS_OP_THREAD_CREATE, SYS_HANDLE_INVALID, &args,
      sizeof(args));
    if (out_thread != NULL) {
        *out_thread = err == SYS_OK ? args.out_handle : SYS_HANDLE_INVALID;
    }

    return err;
}

sys_err_t
sys_thread_status(sys_handle_t thread, int32_t *out_code,
  uint32_t *out_state)
{
    sys_op_status_args_t args;
    sys_err_t err;

    memset(&args, 0, sizeof(args));

    err = submit(SYS_OP_THREAD_STATUS, thread, &args, sizeof(args));
    if (out_code != NULL) {
        *out_code = args.out_code;
    }
    if (out_state != NULL) {
        *out_state = args.out_state;
    }

    return err;
}

sys_err_t
sys_irq_unmask(sys_handle_t irq)
{
    return submit(SYS_OP_IRQ_UNMASK, irq, NULL, 0);
}
