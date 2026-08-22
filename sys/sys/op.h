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
 * Unmask an interrupt the handler silenced, which is sys_irq_unmask in
 * section 26 and is why that call is in the list. Section 13.2 has the
 * handler advance a timeline and return, and a handler that cannot
 * finish with its device masks the source instead; this is the driver
 * saying it has drained the device to empty and the source may assert
 * again. Draining first is the driver's obligation: unmasking with
 * bytes still in the device interrupts again at once.
 * target names the interrupt and carries SYS_RIGHT_IRQ.
 */
#define SYS_OP_IRQ_UNMASK        16

/*
 * Create a process from a member of the boot archive, per section 10.1,
 * with the handles the caller names moved into it, per section 20.4.
 *
 * The image is named rather than handed over, and that is a deviation
 * section 24.1 is explicit about: the kernel parses no image format and
 * root walks the archive. What is missing is a process able to load one,
 * which is the loader of section 15.2, so the walk stays inside the
 * kernel until M4 and this operation is where the debt shows.
 */
#define SYS_OP_PROC_SPAWN        17

/*
 * Add a thread to the calling process, per section 10.2. The creator
 * supplies the stack, because the kernel allocates no user memory at
 * any tier, and the thread begins with none of its own code having
 * run: a first thread has crt1 and a created thread has nothing.
 */
#define SYS_OP_THREAD_CREATE     18

/* The exit code and state of a thread, as SYS_OP_PROC_STATUS is for a
 * process. Requires SYS_RIGHT_STAT. */
#define SYS_OP_THREAD_STATUS     19

/*
 * Put a grant's region where the caller can reach it, and say where that
 * is. target names the grant and carries SYS_RIGHT_MAP; the access the
 * mapping permits comes from the other rights on the same handle, so a
 * grant given with SYS_RIGHT_READ alone becomes memory that cannot be
 * written.
 *
 * Section 19.2: this is how a driver learns where its registers are. It
 * is asked for rather than done at spawn because a holder that never
 * maps a grant should not pay for it, and because the address belongs in
 * the answer to a question rather than in a table a program has to know
 * the shape of.
 */
#define SYS_OP_GRANT_MAP         20

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

#define SYS_OP_COUNT             21

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
     * the identity on both tiers as things stand: Tier 0 has one address
     * space, and Tier 2 maps the pages into the receiver at the address
     * they came with, so that a server can dereference what it was
     * handed. It stays a rewrite in the interface because the day a
     * receiver needs a different address, every program that reads this
     * field rather than remembering what it sent already works.
     *
     * The memory must be the sender's own image, and the flags are the
     * access the receiver is being given rather than a description of
     * what the sender has. A buffer that is neither is refused before
     * the message is delivered.
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

/*
 * A deadline that never arrives, and one that has already passed. They
 * are here rather than with the clock because a deadline is part of
 * every blocking operation and a program has to be able to name one
 * without knowing anything else about time.
 */
#define SYS_DEADLINE_NOW        ((uint64_t)0)
#define SYS_DEADLINE_NEVER      (~(uint64_t)0)

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

/*
 * The state a status call reports, per section 10.6, with the names and
 * the values section 26 already gives them. They were SYS_STATUS_LIVE
 * and SYS_STATUS_EXITED here, invented alongside a specification that
 * had already named them, and the second one had a different value from
 * the constant it shared most of a name with. A program written against
 * the document would have read a live thread as an exited one.
 */
#define SYS_STATE_READY          0
#define SYS_STATE_RUNNING        1
#define SYS_STATE_BLOCKED        2
#define SYS_STATE_EXITED         3

/*
 * The fault kinds of section 16.1. Every synchronous exception a port can
 * raise while a thread is running maps to one of these, because a kind
 * that is not in the list has nowhere to go.
 *
 * SYS_FAULT_PAGE is here and is not yet raised by either port. It is the
 * one recoverable kind, and what makes it recoverable is a pager, which
 * is stage 8 of this milestone. Until then a reference to an address the
 * tables do not describe is SYS_FAULT_ACCESS, because nothing exists that
 * could supply the page: reporting it as recoverable would be reporting a
 * mechanism the system does not have.
 */
#define SYS_FAULT_NONE          0   /* it did not fault */
#define SYS_FAULT_PAGE          1
#define SYS_FAULT_ACCESS        2
#define SYS_FAULT_ALIGN         3
#define SYS_FAULT_INSTRUCTION   4
#define SYS_FAULT_ARITH         5
#define SYS_FAULT_DEBUG         6
#define SYS_FAULT_EXTERNAL      7

/*
 * The exit code of a thread the kernel ended for a fault. A fault is an
 * exit with a reason, per section 16.2, so it travels the path an exit
 * travels and needs a code to travel with; the reason itself is in the
 * fault fields beside it.
 *
 * It is not a value only the kernel produces. A program may return it,
 * and one that reads a word out of memory and returns it can return
 * anything at all. What tells the two apart is the fault kind, which is
 * SYS_FAULT_NONE for every thread that chose its own code, and a reader
 * that looks only at the number is asking the wrong field.
 */
#define SYS_EXIT_FAULT          (-16)

/*
 * Argument structure for SYS_OP_OBJECT_COMPLETION. target names the
 * thread or the process whose completion is wanted.
 */
typedef struct sys_op_completion_args {
    sys_handle_t    out_handle;     /* written by the kernel */
} sys_op_completion_args_t;

/*
 * Argument structure for SYS_OP_PROC_STATUS and SYS_OP_THREAD_STATUS.
 * target names the process or the thread.
 *
 * The fault fields are section 16.2's: a thread the kernel ended for a
 * fault has exited, so the state says EXITED and the code says
 * SYS_EXIT_FAULT, and these say which fault, where, and what instruction.
 * They are SYS_FAULT_NONE and zero for everything else, which includes a
 * thread that is still running.
 *
 * A process reports the fault of the thread whose exit finished it, which
 * with one thread per process is the program's own. Section 16.2 leaves
 * open which thread speaks for a process with several, and so does this.
 */
typedef struct sys_op_status_args {
    int32_t         out_code;       /* written by the kernel */
    uint32_t        out_state;      /* written by the kernel: SYS_STATE_* */
    uint32_t        out_fault;      /* written by the kernel: SYS_FAULT_* */
    uint32_t        out_reserved;
    uintptr_t       out_fault_addr; /* written by the kernel */
    uintptr_t       out_fault_pc;   /* written by the kernel */
} sys_op_status_args_t;

/*
 * Argument structure for SYS_OP_GRANT_MAP. target names the grant.
 *
 * The length comes back as well as the base because a holder is told
 * what it was given rather than told to assume: a driver that knows its
 * register block is four kilobytes and is handed one page has a bug that
 * says so here rather than a fault later.
 */
typedef struct sys_op_grant_args {
    uintptr_t       out_base;       /* written by the kernel */
    uintptr_t       out_len;        /* written by the kernel */
} sys_op_grant_args_t;

/*
 * The longest name a boot archive member can have, which is what an image
 * is named by until there is a filesystem to load one from. It matches
 * BOOT_NAME_MAX, and the kernel asserts that it still does.
 */
#define SYS_IMAGE_NAME_MAX      16

/*
 * Ask for the priority the caller is already running at. A program does
 * not know its own number and has no operation that would tell it, and
 * the common case is a parent starting something alongside itself. Zero
 * cannot mean this, because zero is a priority.
 */
#define SYS_PRIO_INHERIT        0xffffffffu

/*
 * What a program may be started with. Section 20.4 has the loader place
 * the strings in the child's memory alongside the record; the kernel is
 * the loader here, so it copies them into the process object, and these
 * are the bounds of that copy. Eight arguments and two hundred and
 * fifty-six bytes is a command line, not a configuration file: a
 * program that needs more than this needs a file.
 */
#define SYS_MAX_ARGS            8
#define SYS_ARG_BYTES           256

/*
 * Argument structure for SYS_OP_PROC_SPAWN. target is unused: there is
 * nothing to hold a handle to until a loader exists.
 *
 * The name is inline rather than a pointer because it is short, bounded,
 * and the only string in the interface. ns points at nns entries the
 * caller assembled; every one of them is moved out of the caller's table,
 * and a caller that names a handle it does not hold is refused before any
 * of them move.
 */
typedef struct sys_op_spawn_args {
    char            image[SYS_IMAGE_NAME_MAX];
    uint32_t        prio;
    uint32_t        nns;
    uintptr_t       ns;             /* sys_ns_spec_t *, nns of them */
    uint32_t        argc;
    uint32_t        _pad;
    uintptr_t       argv;           /* const char *const *, argc of them */
    sys_handle_t    out_handle;     /* written by the kernel */
} sys_op_spawn_args_t;

/*
 * Argument structure for SYS_OP_THREAD_CREATE. target is unused: a thread
 * joins the process that asked for it, and a process cannot name another.
 *
 * stack is the highest address of the region, since stacks grow down on
 * both ports, and must be sixteen-byte aligned. entry is where the thread
 * starts and arg is its one argument, which is how the C library hands a
 * thread both a function and its argument through a single word.
 */
typedef struct sys_op_thread_args {
    uintptr_t       entry;
    uintptr_t       arg;
    uintptr_t       stack;
    uint32_t        prio;
    uint32_t        _pad;
    sys_handle_t    out_handle;     /* written by the kernel */
} sys_op_thread_args_t;

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
