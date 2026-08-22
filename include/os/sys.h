/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The system call interface, as a program sees it. Section 26 of
 * design.md is the specification this file is the working subset of.
 *
 * There is one entry point. sys_op_submit() is it, and everything below is
 * a wrapper that fills in a descriptor and calls it, per section 23.1.
 * That is not a convenience layer over several system calls; it is the
 * only call there is, and the wrappers exist so that ordinary code does
 * not have to write out a descriptor by hand.
 */

#ifndef _OS_SYS_H_
#define _OS_SYS_H_

#include <sys/op.h>
#include <sys/startup.h>

#include <stddef.h>

/*
 * Execute count operations. The count is one until the submission engine
 * of section 17 exists, and a kernel without it rejects anything else
 * rather than ignoring the excess, so a program built against that engine
 * fails loudly instead of silently doing less.
 *
 * The status is written into each descriptor and also returned, per
 * section 23.3.
 */
sys_err_t sys_op_submit(sys_op_t *ops, unsigned long count);

/*
 * Blocks until a server receives the request and replies to it. target
 * names a connection, or a channel handle carrying SYS_RIGHT_SEND.
 */
sys_err_t sys_msg_send(sys_handle_t target, const sys_msg_t *request,
  sys_msg_t *reply);

/*
 * Blocks until a message arrives. Writes the client's thread id, which is
 * what a reply is addressed to, and the cookie the server named this
 * connection with when it minted it.
 */
sys_err_t sys_msg_recv(sys_handle_t chan, sys_msg_t *message,
  sys_thread_id_t *peer, uint64_t *cookie);

/*
 * Never blocks. SYS_ERR_STALE means the client is no longer waiting on
 * this request, which covers one that died, timed out, or was cancelled.
 */
sys_err_t sys_msg_reply(sys_thread_id_t peer, const sys_msg_t *reply);

/*
 * Mint a connection to a channel and return a handle to it. Requires
 * SYS_RIGHT_RECV on the channel, so only a server can do this; the handle
 * is then given to a client, in a reply or in its startup namespace. The
 * cookie is the server's own per-connection state and the kernel never
 * looks at it.
 */
sys_err_t sys_chan_connect(sys_handle_t chan, uint64_t cookie,
  uint32_t rights, sys_handle_t *out_conn);

/*
 * Create a process from a member of the boot archive, per section 10.1.
 *
 * Every handle in ns is moved out of the caller's table, per section 7.4,
 * so this is a list of what the caller is giving up. A caller that names a
 * handle it does not hold, or one it may not transfer, is refused with
 * nothing moved and no process built.
 *
 * The child may not outrank the caller. Priority is authority over the
 * core, and a program cannot hand out what it was not given.
 *
 * The arguments are copied into the child, so the caller may reuse the
 * array and the strings the moment this returns. A command line that
 * does not fit is refused rather than truncated.
 *
 * The handle returned carries SYS_RIGHT_WAIT and SYS_RIGHT_STAT, which is
 * what joining and reading a status need and nothing else.
 */
sys_err_t sys_proc_spawn(const char *image, uint32_t prio,
  const sys_ns_spec_t *ns, uint32_t nns, const char *const *argv,
  uint32_t argc, sys_handle_t *out_proc);

/*
 * The completion timeline of a thread or a process, per section 10.4.
 * Waiting on it is joining, and it cannot fail for want of a pool slot
 * because the timeline is part of the object. Requires SYS_RIGHT_WAIT.
 */
sys_err_t sys_object_completion(sys_handle_t obj,
  sys_handle_t *out_timeline);

/*
 * The exit code, which a monotonic counter cannot carry, and the state
 * that says whether the code means anything yet. Requires
 * SYS_RIGHT_STAT on the handle, which is what spawn hands out.
 */
sys_err_t sys_proc_status(sys_handle_t proc, int32_t *out_code,
  uint32_t *out_state);

/*
 * What ended a process, when the hardware ended it, per section 16.2.
 * The kind is SYS_FAULT_NONE for a process that exited by choice, which
 * is every process that was not stopped by the machine it ran on.
 */
typedef struct sys_fault {
    uint32_t    kind;       /* SYS_FAULT_* */
    uintptr_t   addr;
    uintptr_t   pc;
} sys_fault_t;

sys_err_t sys_proc_fault(sys_handle_t proc, sys_fault_t *out);

/*
 * Block until a timeline reaches value, or until the absolute deadline
 * passes. SYS_DEADLINE_NEVER waits without one, and a deadline already
 * passed is a poll.
 */
sys_err_t sys_fence_wait(sys_handle_t timeline, uint64_t value,
  uint64_t deadline);

/*
 * Add a thread to this process, per section 10.2.
 *
 * The stack is the caller's, at any address it likes, because the kernel
 * allocates no user memory at any tier. stack names the region and size
 * its length; what the kernel is given is the highest address, aligned
 * down, since stacks grow down on both ports.
 *
 * The function may return. The library puts it and its argument on the new
 * stack and starts the thread in a trampoline that calls it and then
 * exits, which is the whole of why the operation takes one word where a
 * thread needs two.
 *
 * The handle returned carries SYS_RIGHT_WAIT and SYS_RIGHT_STAT, so a
 * creator can wait on the completion timeline of section 10.4 and read the
 * status afterwards.
 */
sys_err_t sys_thread_create(void (*fn)(void *), void *arg, void *stack,
  size_t size, uint32_t prio, sys_handle_t *out_thread);

/*
 * Unmask an interrupt the handler silenced, per section 13.2. The
 * driver has drained its device and the source may assert again;
 * draining first is the caller's obligation, since unmasking with
 * bytes still in the device interrupts again at once. Requires
 * SYS_RIGHT_IRQ.
 */
sys_err_t sys_irq_unmask(sys_handle_t irq);

/* The exit code and state of a thread. Requires SYS_RIGHT_STAT. */
sys_err_t sys_thread_status(sys_handle_t thread, int32_t *out_code,
  uint32_t *out_state);

/* What ended a thread, when the hardware ended it. See sys_proc_fault. */
sys_err_t sys_thread_fault(sys_handle_t thread, sys_fault_t *out);

/*
 * Ends the calling thread and does not return. Section 10.4 makes exit
 * advance a completion timeline a joiner reads; none of that exists yet,
 * so this stops the thread and records the status and nothing more.
 */
void sys_thread_exit(int32_t code) __attribute__((noreturn));

/*
 * Write to the kernel console. A debug backdoor, per section 22.4: it
 * takes no handle and is ambient authority, so it is present only in
 * builds that opt into it and returns SYS_ERR_NOT_SUPPORTED otherwise.
 * A console service reached by a handle is what replaces it.
 */
sys_err_t sys_debug_write(const void *buf, uint32_t len);

sys_err_t sys_handle_close(sys_handle_t handle);

sys_err_t sys_handle_dup(sys_handle_t handle, uint32_t new_rights,
  sys_handle_t *out_handle);

#endif /* _OS_SYS_H_ */
