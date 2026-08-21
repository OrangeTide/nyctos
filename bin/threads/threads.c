/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * A program with more than one thread, which is the shape section 19.3
 * prescribes for every driver and which nothing here could write until
 * now.
 *
 * Two things are being checked, and they are different. The first is that
 * a created thread runs, blocks on something of its own while the thread
 * that created it blocks on something else, exits, and can be joined and
 * asked how it went. The second is that a process finishes when its last
 * thread does and not when its first one does: the main thread exits
 * immediately while the second is still blocked, and the status the
 * process reports has to be the second thread's.
 *
 * The stacks are ordinary arrays in this program's .bss, per section 10.2.
 * There is no allocator in the C library and the kernel allocates no user
 * memory at any tier, so a thread's stack is memory its creator already
 * had.
 */

#include <os/sys.h>
#include <debug.h>
#include <stddef.h>
#include <string.h>

#define STACK_BYTES 4096

static unsigned char stack_a[STACK_BYTES] __attribute__((aligned(16)));
static unsigned char stack_b[STACK_BYTES] __attribute__((aligned(16)));

#define ERR_CREATE      -1
#define ERR_JOIN        -2
#define ERR_STATUS      -3
#define ERR_CODE        -4
#define ERR_ALIGN       -5

/* What each of the two threads exits with, so neither can be mistaken for
 * the other or for a status nobody wrote. */
#define CODE_A          3
#define CODE_B          9

static int
join_thread(sys_handle_t th, int32_t *out_code)
{
    sys_handle_t done;
    uint32_t state;

    if (sys_object_completion(th, &done) != SYS_OK) {
        return ERR_JOIN;
    }
    if (sys_fence_wait(done, SYS_COMPLETION_EXITED,
      SYS_DEADLINE_NEVER) != SYS_OK) {
        return ERR_JOIN;
    }
    if (sys_thread_status(th, out_code, &state) != SYS_OK) {
        return ERR_STATUS;
    }
    if (state != SYS_STATUS_EXITED) {
        return ERR_STATUS;
    }

    (void)sys_handle_close(done);

    return 0;
}

/*
 * Blocks on a child of its own, so that while the main thread is blocked
 * on this thread's completion the two are waiting on different objects.
 * That is the arrangement section 19.3 calls two threads and no shared
 * state, with a filesystem standing in for a device.
 */
static void
thread_a(void *arg)
{
    sys_handle_t child, done;

    (void)arg;

    if (sys_proc_spawn("status", SYS_PRIO_INHERIT, NULL, 0, &child)
      != SYS_OK) {
        sys_thread_exit(ERR_CREATE);
    }
    if (sys_object_completion(child, &done) != SYS_OK) {
        sys_thread_exit(ERR_JOIN);
    }
    if (sys_fence_wait(done, SYS_COMPLETION_EXITED,
      SYS_DEADLINE_NEVER) != SYS_OK) {
        sys_thread_exit(ERR_JOIN);
    }

    sys_thread_exit(CODE_A);
}

/*
 * Outlives the thread that created it. main exits while this is still
 * blocked, so the process is finished only when this returns, and the
 * status the process reports is this one's.
 */
static void
thread_b(void *arg)
{
    sys_handle_t child, done;

    (void)arg;

    if (sys_proc_spawn("spin", SYS_PRIO_INHERIT, NULL, 0, &child)
      != SYS_OK) {
        sys_thread_exit(ERR_CREATE);
    }
    if (sys_object_completion(child, &done) != SYS_OK) {
        sys_thread_exit(ERR_JOIN);
    }
    if (sys_fence_wait(done, SYS_COMPLETION_EXITED,
      SYS_DEADLINE_NEVER) != SYS_OK) {
        sys_thread_exit(ERR_JOIN);
    }

    DBG_PUTS("threads: the second thread is the last one out");
    sys_thread_exit(CODE_B);
}

int
main(void)
{
    sys_op_thread_args_t args;
    sys_op_t op;
    sys_handle_t th;
    int32_t code = 0;
    int err;

    /*
     * A stack the operation will not take, submitted as a descriptor
     * rather than through the wrapper. Both ports need sixteen-byte
     * alignment and one of them faults on it, so the kernel refuses
     * a stack it cannot use rather than letting the thread find out.
     *
     * The wrapper cannot ask this question, because aligning the
     * region it was handed is part of what it does: an earlier
     * version of this check passed a misaligned array to
     * sys_thread_create, got a thread, and was testing the library.
     */
    memset(&args, 0, sizeof(args));
    args.entry = (uintptr_t)thread_a;
    args.stack = (uintptr_t)stack_a + 1;
    args.prio = SYS_PRIO_INHERIT;

    memset(&op, 0, sizeof(op));
    op.opcode = SYS_OP_THREAD_CREATE;
    op.arg_ptr = (uintptr_t)&args;
    op.arg_len = sizeof(args);

    if (sys_op_submit(&op, 1) != SYS_ERR_INVALID_ARG) {
        DBG_PUTS("threads: a misaligned stack was accepted");
        return ERR_ALIGN;
    }
    DBG_PUTS("threads: one thread waiting while another waits");
    if (sys_thread_create(thread_a, NULL, stack_a, STACK_BYTES,
      SYS_PRIO_INHERIT, &th) != SYS_OK) {
        return ERR_CREATE;
    }

    err = join_thread(th, &code);
    if (err != 0) {
        return err;
    }
    if (code != CODE_A) {
        DBG_PUTS("threads: the first thread reported the wrong status");
        return ERR_CODE;
    }
    (void)sys_handle_close(th);

    /*
     * The second thread outlives this one. Nothing here joins it, and the
     * process is not finished until it exits, which is what the kernel
     * checks by reading a status that only this thread could not have
     * written.
     */
    if (sys_thread_create(thread_b, NULL, stack_b, STACK_BYTES,
      SYS_PRIO_INHERIT, &th) != SYS_OK) {
        return ERR_CREATE;
    }

    DBG_PUTS("threads: the first thread leaves, the second is still here");

    return 0;
}
