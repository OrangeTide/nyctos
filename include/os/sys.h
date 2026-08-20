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
