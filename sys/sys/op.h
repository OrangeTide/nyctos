/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The operation interface of section 23: one descriptor, one opcode, and no
 * system call numbers.
 *
 * A caller fills in a descriptor and the kernel executes it, writing the
 * result into the descriptor and returning the same value. There is exactly
 * one implementation of every operation, reached the same way from a user
 * trap and from inside the kernel, which is the property section 23.1 is
 * after.
 *
 * The trap carries a pointer and a count, and the count is one. Section 17
 * is what happens when it exceeds one; nothing here depends on that
 * existing, and this file is complete without it.
 *
 * This is the shared kernel and userland interface. The names follow the C
 * header in section 26 rather than the project's usual preference against
 * typedefs of structures, because this is the published interface and the
 * two should not disagree.
 */

#ifndef _SYS_OP_H_
#define _SYS_OP_H_

#include <stdint.h>

/*
 * A handle is one pointer-width word. The kernel splits it into a slot
 * index and a generation, per section 7.1, but that split is the kernel's
 * business: to a program a handle is opaque and the only operations on it
 * are passing it and comparing it to SYS_HANDLE_INVALID.
 *
 * Pointer width rather than a fixed 64 bits because the split is sized to
 * the pools, and a part whose whole system holds a few dozen objects
 * should not carry a million slots of index in every message.
 */
typedef int32_t   sys_err_t;
typedef uintptr_t sys_handle_t;

/*
 * A handle value that names nothing. Zero, so a zeroed structure carries
 * no authority by accident and a caller that forgot to fill one in is
 * refused rather than reaching slot zero.
 */
#define SYS_HANDLE_INVALID    ((sys_handle_t)0)

#define SYS_OK                    0
#define SYS_ERR_TIMEOUT          -1
#define SYS_ERR_WOULDBLOCK       -2
#define SYS_ERR_INVALID_ARG      -3
#define SYS_ERR_NO_MEMORY        -4   /* static pool exhausted; see section 8 */
#define SYS_ERR_PERM_DENIED      -5   /* handle lacks the required right */
#define SYS_ERR_DEADLOCK         -6
#define SYS_ERR_STALE            -7   /* generation mismatch */
#define SYS_ERR_NO_ENT           -8
#define SYS_ERR_CANCELED         -9
#define SYS_ERR_OWNER_DEAD      -10
#define SYS_ERR_TRUNCATED       -11
#define SYS_ERR_WRONG_TYPE      -12   /* handle names the wrong object type */
#define SYS_ERR_INTERRUPTED     -13
#define SYS_ERR_NOT_SUPPORTED   -14   /* not available at this tier or yet */
#define SYS_ERR_FAULT           -15   /* bad user address */

/*
 * Rights are uniform across object types rather than special-cased per
 * type, per section 7.2. Duplication may only attenuate.
 */
#define SYS_RIGHT_DUPLICATE   (1u << 0)
#define SYS_RIGHT_TRANSFER    (1u << 1)   /* may be moved over IPC */
#define SYS_RIGHT_READ        (1u << 2)
#define SYS_RIGHT_WRITE       (1u << 3)
#define SYS_RIGHT_EXEC        (1u << 4)
#define SYS_RIGHT_MAP         (1u << 5)
#define SYS_RIGHT_SEND        (1u << 6)
#define SYS_RIGHT_RECV        (1u << 7)
#define SYS_RIGHT_WAIT        (1u << 8)
#define SYS_RIGHT_SIGNAL      (1u << 9)
#define SYS_RIGHT_MEM_LOCK    (1u << 10)
#define SYS_RIGHT_IRQ         (1u << 11)
#define SYS_RIGHT_STAT        (1u << 12)
#define SYS_RIGHT_ALL         0xFFFFFFFFu

#define SYS_OBJ_NONE        0   /* lookup that does not care about the type */
#define SYS_OBJ_CHANNEL     1
#define SYS_OBJ_GRANT       2
#define SYS_OBJ_TIMELINE    3
#define SYS_OBJ_TIMER       4
#define SYS_OBJ_PROGRAM     5
#define SYS_OBJ_PROCESS     6
#define SYS_OBJ_THREAD      7
#define SYS_OBJ_IRQ         8
#define SYS_OBJ_CONNECTION  9

/* Blocking ops. These are the only ones that can block. */
#define SYS_OP_NOP                0
#define SYS_OP_IPC_SEND           1
#define SYS_OP_IPC_RECV           2
#define SYS_OP_FENCE_WAIT         3

/* Non-blocking ops. */
#define SYS_OP_IPC_REPLY          4
#define SYS_OP_FENCE_SIGNAL       5
#define SYS_OP_HANDLE_CLOSE       6
#define SYS_OP_HANDLE_DUP         7
#define SYS_OP_CHAN_CONNECT      11

/*
 * Obtain the completion timeline of a thread or a process, per section
 * 10.4. The timeline is embedded in the object, so this cannot fail for
 * want of a pool slot, and the handle it returns carries SYS_RIGHT_WAIT
 * and nothing else: a process able to signal its own completion could
 * fake its own exit and have whatever supervises it reclaim it while it
 * was still running.
 */
#define SYS_OP_OBJECT_COMPLETION 14

/*
 * Read the exit code and state of a process. A timeline value is a
 * monotonic counter and cannot carry a code, so the code is read from the
 * object rather than from the timeline that says it has finished.
 */
#define SYS_OP_PROC_STATUS       15

/*
 * Unmask an interrupt the handler silenced. Section 13.2 has the
 * handler advance a timeline and return, and a handler that cannot
 * finish with its device masks the source instead; this is the driver
 * saying it has drained the device and the source may assert again.
 * target names the interrupt and carries SYS_RIGHT_IRQ.
 */
#define SYS_OP_IRQ_ACK           16

/*
 * Ends the calling thread. It does not return, so it has no status to
 * write and no place to write one.
 */
#define SYS_OP_THREAD_EXIT       12

/*
 * Write bytes to the kernel console. A debug facility and nothing else,
 * present only in builds that opt into it; see SYS_DEBUG_CONSOLE and
 * section 22.4. Returns SYS_ERR_NOT_SUPPORTED where it is compiled out,
 * which is the default.
 */
#define SYS_OP_DEBUG_WRITE       13

/*
 * The most one call may write. The copy runs in the kernel at the caller's
 * priority, and on Tier 0 with interrupts disabled, so an unbounded write
 * is an unbounded non-preemptible section and lands in the interrupt
 * latency of section 13.3. A caller with more to say makes more calls, and
 * each is a preemption point. This is interface rather than
 * configuration, because a caller has to split against it.
 */
#define SYS_DEBUG_WRITE_MAX      256

/* Issuing ops, which need a device that does DMA. */
#define SYS_OP_DMA_ISSUE          8
#define SYS_OP_READ_ISSUE         9
#define SYS_OP_WRITE_ISSUE       10

#define SYS_OP_COUNT             17

/*
 * The reg_in, reg_out and flags fields are meaningful only to the
 * submission engine, which is not built. They are here so that the
 * descriptor does not change shape if it ever is.
 */
typedef struct sys_op {
    uint16_t        opcode;
    uint16_t        flags;
    uint16_t        reg_in;
    uint16_t        reg_out;

    sys_handle_t    target;
    uintptr_t       arg_ptr;        /* op-specific argument structure */
    uint32_t        arg_len;
    uint32_t        _reserved;
    uint64_t        user_data;      /* opaque caller tag, never interpreted */

    sys_err_t       status;         /* written by the kernel */
    uint32_t        _pad;
} sys_op_t;

/*
 * The message of section 12.4: a small inline payload, a handle that moves
 * with it, and a buffer for bulk data.
 *
 * The design allows zero or more handles; this carries at most one, which
 * is enough for a connection to be handed back and is the only shape
 * anything needs yet.
 *
 * The payload is a copy rather than a register transfer. Whether a port
 * also carries the first words in registers is its own business and does
 * not change what this means.
 */
#define SYS_IPC_INLINE_BYTES    64

/* Buffer access the receiver is being given, per section 6.1. */
#define SYS_BUF_READ            (1u << 0)
#define SYS_BUF_WRITE           (1u << 1)

typedef uint64_t sys_thread_id_t;

typedef struct sys_msg {
    uint32_t        flags;
    uint32_t        inline_len;
    sys_handle_t    handle;     /* moved with the message, or INVALID */

    /*
     * Bulk data, moved for the duration of the call. By section 6.1 the
     * sender may not touch this memory between send and reply, and the
     * right returns when the reply arrives. Tier 0 does not enforce that
     * and Tier 2 will, which is the whole of the difference between them.
     *
     * The address is the sender's on the way in and the receiver's on the
     * way out, rewritten by the kernel at the rendezvous. The rewrite is
     * the identity at Tier 0 because there is one address space, which is
     * why this is a field and not a handle: nothing here crosses a
     * boundary unchanged, so section 6.1's rule against a raw pointer
     * doing so is kept without an object to name the memory.
     *
     * Only the forward path carries one. A server does not hand back a
     * buffer of its own memory, per the reply-path caveat in section 6.1,
     * so a reply larger than the inline area writes into the buffer the
     * client supplied in the request.
     */
    uintptr_t       buf;
    uint32_t        buf_len;
    uint32_t        buf_flags;  /* SYS_BUF_* */

    uint8_t         inline_data[SYS_IPC_INLINE_BYTES];
} sys_msg_t;

/* Argument structure for SYS_OP_CHAN_CONNECT. target names the channel. */
typedef struct sys_op_connect_args {
    uint64_t        cookie;
    uint32_t        rights;     /* rights on the returned handle */
    uint32_t        _pad;
    sys_handle_t    out_handle; /* written by the kernel */
} sys_op_connect_args_t;

/* Argument structures for the three message operations. */
typedef struct sys_op_send_args {
    sys_msg_t       request;
    sys_msg_t       reply;      /* written by the kernel */
} sys_op_send_args_t;

typedef struct sys_op_recv_args {
    sys_msg_t       message;    /* written by the kernel */
    sys_thread_id_t peer;       /* written by the kernel; reply to this */

    /*
     * The connection this arrived on, as the server named it when it made
     * the connection. The kernel carries it and attaches no meaning.
     */
    uint64_t        cookie;     /* written by the kernel */
} sys_op_recv_args_t;

typedef struct sys_op_reply_args {
    sys_msg_t       reply;
    sys_thread_id_t peer;
} sys_op_reply_args_t;

/*
 * Argument structure for SYS_OP_FENCE_WAIT and SYS_OP_FENCE_SIGNAL. target
 * names the timeline. The deadline is ignored when signalling, which never
 * blocks.
 */
typedef struct sys_op_fence_args {
    uint64_t        value;

    /*
     * Absolute, on the monotonic timebase. Section 14.3: a relative
     * timeout restarted after interruption extends its own deadline by
     * however long it had already waited, which is the error real-time
     * code exists to avoid.
     */
    uint64_t        deadline;
} sys_op_fence_args_t;

/* Argument structure for SYS_OP_THREAD_EXIT. target is unused. */
typedef struct sys_op_exit_args {
    int32_t         code;
    uint32_t        _pad;
} sys_op_exit_args_t;

/*
 * The value a completion timeline is advanced to when its thread or
 * process finishes. It is a fixed value rather than a count because a
 * thread finishes once: a joiner waits for this and a value already
 * reached satisfies it immediately, which is what makes a late joiner
 * safe without a registration protocol.
 */
#define SYS_COMPLETION_EXITED    1

/* The state a status call reports, per section 10.6. */
#define SYS_STATUS_LIVE          0
#define SYS_STATUS_EXITED        1

/*
 * Argument structure for SYS_OP_OBJECT_COMPLETION. target names the
 * thread or the process whose completion is wanted.
 */
typedef struct sys_op_completion_args {
    sys_handle_t    out_handle;     /* written by the kernel */
} sys_op_completion_args_t;

/* Argument structure for SYS_OP_PROC_STATUS. target names the process. */
typedef struct sys_op_status_args {
    int32_t         out_code;       /* written by the kernel */
    uint32_t        out_state;      /* written by the kernel: SYS_STATUS_* */
} sys_op_status_args_t;

/*
 * Argument structure for SYS_OP_DEBUG_WRITE. target is unused: this takes
 * no handle, which is the whole of what is wrong with it and is why it is
 * a build option rather than an interface.
 */
typedef struct sys_op_debug_args {
    uintptr_t       buf;
    uint32_t        len;
    uint32_t        _pad;
} sys_op_debug_args_t;

/* Argument structure for SYS_OP_HANDLE_DUP. target names the source. */
typedef struct sys_op_dup_args {
    uint32_t        new_rights;
    uint32_t        _pad;
    sys_handle_t    out_handle;     /* written by the kernel */
} sys_op_dup_args_t;

#endif /* _SYS_OP_H_ */
