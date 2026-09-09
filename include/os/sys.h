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
#include <sys/syspage.h>
#include <sys/startup.h>

#include <stdbool.h>
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
 * The system page of section 6.2, mapped read-only into this process,
 * or NULL for one that has none. What it carries is in sys/syspage.h.
 */
const sys_syspage_t *sys_syspage(void);

/*
 * The clock of section 6.2, read with no system call: ticks from the
 * counter, and how many of them a second is from the system page. The
 * frequency is zero for a process that has no page.
 */
uint64_t sys_uptime(void);
uint64_t sys_uptime_hz(void);

/*
 * And a deadline from now, per section 14.3: absolute is what a call
 * takes, and turning a relative one into it is the caller's to do. A
 * plain uint64_t because that is what a deadline is everywhere else
 * here; section 14.3 spells the type sys_deadline_t.
 * SYS_DEADLINE_NOW for a program with no page, which has no scale to
 * turn milliseconds into ticks with.
 */
uint64_t sys_deadline_in(uint64_t ticks);
uint64_t sys_deadline_in_ms(uint32_t ms);

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
 * allocates no user memory at any tier. stack is the *lowest* address of
 * the region and size its length; what the kernel is given is the
 * highest, aligned down, since stacks grow down on both ports.
 *
 * Passing the top and the length instead is the mistake this wording
 * exists to prevent, and it is silent: the library computes stack plus
 * size and writes a pair of words just below that, which is a whole
 * region past the end of the array and lands on whatever the linker put
 * there next.
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

/*
 * Release a thread from whatever it is blocked on, per section 10.5. Its
 * pending call returns SYS_ERR_INTERRUPTED. Needs SYS_RIGHT_WRITE on the
 * handle, because this changes what the thread does.
 */
sys_err_t sys_thread_cancel(sys_handle_t thread);

/*
 * Move a thread to a named core, per section 11.4. Nothing moves on its
 * own, so this is the only way anything moves: where a thread runs is a
 * fact about the program that placed it.
 */
sys_err_t sys_thread_migrate(sys_handle_t thread, uint32_t core);

/*
 * Leave a pulse on a channel, per section 13.1. Never blocks. A program
 * may send only user codes, at or above SYS_PULSE_USER_BASE: the codes
 * below it reach the reserved slots that keep a death notice from being
 * the pulse that is dropped.
 */
sys_err_t sys_pulse_send(sys_handle_t chan, const sys_pulse_t *pulse);

/*
 * Collect one departed peer of a channel, per M18 stage four. The
 * first call with enable set says this channel keeps a connection
 * whose client handles have all closed, rather than freeing it; every
 * call returns one such connection's cookie and lets it go, and
 * SYS_ERR_NO_ENT when there are none left. Needs SYS_RIGHT_RECV.
 *
 * The SYS_PULSE_PEER_GONE pulse is a hint that this is worth calling.
 * It may be coalesced or lost, so what a server acts on is the answer
 * here and not the count of pulses it saw.
 */
sys_err_t sys_chan_reap(sys_handle_t chan, bool enable, uint64_t *out_cookie);

/*
 * End a process, per section 10.5. Every thread of it is unblocked as
 * if cancelled and leaves at its next return from the kernel. Needs
 * SYS_RIGHT_WRITE on the handle.
 */
sys_err_t sys_proc_kill(sys_handle_t proc, int32_t code);

/* What ended a thread, when the hardware ended it. See sys_proc_fault. */
sys_err_t sys_thread_fault(sys_handle_t thread, sys_fault_t *out);

/*
 * Put a grant's region where this process can reach it, and say where.
 * Section 19.2: this is how a driver learns where its registers are,
 * having been handed authority over them rather than an address.
 *
 * The handle must carry SYS_RIGHT_MAP. What the memory permits once it
 * is there comes from the other rights on the same handle, so a grant
 * given read-only is memory that cannot be written.
 */
sys_err_t sys_grant_map(sys_handle_t grant, uintptr_t *out_base,
  uintptr_t *out_len);

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
