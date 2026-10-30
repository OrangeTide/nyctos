/* op.h : operation descriptor, error codes, rights and opcodes.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares sys_err_t, sys_handle_t, the SYS_ERR_* codes, the SYS_RIGHT_* rights,
 * the SYS_OBJ_* object types, and the SYS_OP_* opcodes and flags. Also declares
 * sys_op, the operation descriptor, and the argument structures for each
 * opcode. Included by sys/startup.h, sys/systm.h, sys/thread.h, include/os/sys.h
 * and much of sys/kern.
 *
 */

#ifndef _SYS_OP_H_
#define _SYS_OP_H_

#include <stdint.h>

/*
 * A handle is one pointer-width word. The kernel splits it into a slot
 * index and a generation, but that split is the kernel's
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
#define SYS_ERR_NO_MEMORY        -4   /* static pool exhausted */
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
 * type. Duplication may only attenuate.
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

/*
 * A prepared program: a batch validated once and
 * executed many times. Distinct from SYS_OBJ_PROGRAM, which is a member
 * of the boot archive and was named first.
 */
#define SYS_OBJ_PREPARED   10

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
 * Obtain the completion timeline of a thread or a process. The timeline
 * is embedded in the object, so this cannot fail for
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
 * Unmask an interrupt the handler silenced. A handler normally advances
 * a timeline and returns, and a handler that cannot finish with its
 * device masks the source instead; this is the driver
 * saying it has drained the device to empty and the source may assert
 * again. Draining first is the driver's obligation: unmasking with
 * bytes still in the device interrupts again at once.
 * target names the interrupt and carries SYS_RIGHT_IRQ.
 */
#define SYS_OP_IRQ_UNMASK        16

/*
 * Create a process from a member of the boot archive, with the handles
 * the caller names moved into it.
 *
 * The image is named rather than handed over. The kernel parses no
 * image format, so root walks the archive and this operation is where
 * that walk is visible. No process exists yet that could load an image
 * itself.
 */
#define SYS_OP_PROC_SPAWN        17

/*
 * Add a thread to the calling process. The creator
 * supplies the stack, because the kernel allocates no user memory at
 * any tier, and the thread begins with none of its own code having
 * run: a first thread has crt1 and a created thread has nothing.
 */
#define SYS_OP_THREAD_CREATE     18

/* The exit code and state of a thread, as SYS_OP_PROC_STATUS is for a
 * process. Requires SYS_RIGHT_STAT. */
#define SYS_OP_THREAD_STATUS     19

/*
 * Release a thread from whatever it is blocked on. Its
 * pending call returns SYS_ERR_INTERRUPTED. target names the thread and
 * carries SYS_RIGHT_WRITE, because this changes what the thread does
 * rather than asking about it.
 *
 * SYS_ERR_WOULDBLOCK if it is not blocked on anything, which is not the
 * same answer as having released it and must not share one: a caller
 * told SYS_OK may assume the thread is on its way out of a call, and a
 * caller told this has to look again.
 */
#define SYS_OP_THREAD_CANCEL     21

/*
 * Move a thread to a named core. target names the
 * thread and carries SYS_RIGHT_WRITE, because where a thread runs is
 * something done to it rather than something asked about it. The core
 * is the value in the argument structure.
 *
 * Explicit, and the only way anything moves. Nothing balances and
 * nothing steals, so where a thread runs is a fact about the program
 * that placed it rather than about how busy the machine was.
 *
 * SYS_ERR_INVALID_ARG for a core this build does not have.
 * SYS_ERR_WOULDBLOCK for a thread that is running, because stopping a
 * thread on another core means telling that core to stop it.
 * SYS_ERR_STALE for a thread that has ended.
 */
#define SYS_OP_THREAD_MIGRATE    24

/*
 * Prepared programs. Load validates a batch once and
 * returns a handle to it; exec runs it again without validating it
 * again. The handles the entries name are looked up at every
 * execution, because a handle is a slot in a table and what it means
 * can change between one execution and the next.
 */
#define SYS_OP_PROG_LOAD         25
#define SYS_OP_PROG_EXEC         26

/*
 * Leave a pulse on a channel. target names a
 * connection to it and carries SYS_RIGHT_SEND, which is how a message
 * reaches a channel too. Never blocks and does not fail
 * for want of room: a full user region costs an older user pulse and
 * the receiver is told it happened.
 *
 * A program may only send user pulses. A code below SYS_PULSE_USER_BASE
 * is refused with SYS_ERR_INVALID_ARG, because the reserved slots exist
 * to keep a death notice safe from exactly the program that would like
 * to fill them.
 */
#define SYS_OP_PULSE_SEND        22

/*
 * End a process. target names it and carries
 * SYS_RIGHT_WRITE, because this changes what it does rather than asking
 * about it.
 *
 * Every thread of the target is unblocked exactly as if each had been
 * cancelled, which includes telling
 * every server holding one of their requests. A thread then leaves at
 * its next return from the kernel: unblocking is prompt, and stopping a
 * thread that is running in user mode and never calls in is not
 * something this kernel can do without a hook in the trap path it does
 * not have.
 */
#define SYS_OP_PROC_KILL         23

/*
 * Put a grant's region where the caller can reach it, and say where that
 * is. target names the grant and carries SYS_RIGHT_MAP; the access the
 * mapping permits comes from the other rights on the same handle, so a
 * grant given with SYS_RIGHT_READ alone becomes memory that cannot be
 * written.
 *
 * This is how a driver learns where its registers are. It
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
 * present only in builds that opt into it; see SYS_DEBUG_CONSOLE.
 * Returns SYS_ERR_NOT_SUPPORTED where it is compiled out,
 * which is the default.
 */
#define SYS_OP_DEBUG_WRITE       13

/*
 * The most one call may write. The copy runs in the kernel at the caller's
 * priority, and on Tier 0 with interrupts disabled, so an unbounded write
 * is an unbounded non-preemptible section and lands in the interrupt
 * latency. A caller with more to say makes more calls, and
 * each is a preemption point. This is interface rather than
 * configuration, because a caller has to split against it.
 */
#define SYS_DEBUG_WRITE_MAX      256

/* Issuing ops, which need a device that does DMA. */
#define SYS_OP_DMA_ISSUE          8
#define SYS_OP_READ_ISSUE         9
#define SYS_OP_WRITE_ISSUE       10

/*
 * Collect a connection whose peers have all gone.
 * target names the channel and carries SYS_RIGHT_RECV, because this is
 * the server's question about its own clients.
 *
 * This is the level half of what a death means to a server. The pulse
 * says something has happened and may be coalesced or
 * lost; this says what, and keeps saying it until the server has
 * collected it. A server that acts on the pulse alone is a server
 * counting edges, and one that acts on this is draining a queue: the
 * difference is the difference between `SIGCHLD` and `waitpid`, and it
 * is why losing a pulse costs latency rather than a record.
 *
 * SYS_CHAN_REAP_ENABLE says this channel is one whose departed peers
 * are kept for collecting. Without it a connection returns to its pool
 * when its last handle closes, which is what a channel nobody collects
 * on wants: keeping every connection anybody ever made would be a pool
 * spent on servers that were never going to ask.
 *
 * Returns the cookie the connection was minted with, which is the
 * server's own name for it, and SYS_ERR_NO_ENT when there is nothing to
 * collect.
 */
#define SYS_OP_CHAN_REAP         27

/*
 * Set how many senders a channel will hold before it turns one away.
 * Needs SYS_RIGHT_RECV, so only a side that can
 * receive can bound the queue that feeds it: this is `listen`, and the
 * depth is the backlog.
 *
 * The analogy stops at the depth, because there is no accept here. A
 * send is the connect and the request in one, so joining the queue is
 * connecting, and a client taken off the queue afterwards has not been
 * refused a connection: it made one and was ended before its first
 * request was answered. That is a close after accept.
 *
 * So the rule is one rule. A send always joins the queue, and while the
 * queue is over depth its lowest-priority sender is taken off and
 * failed with SYS_ERR_WOULDBLOCK. When that is the arriving client the
 * effect is a refusal it gets at once; when it is a client already
 * waiting the effect is the close after accept. One error for both,
 * because a client cannot usefully act differently on them: in each
 * case nothing happened, nothing was lent, and the server never saw it.
 *
 * A channel nobody bounds queues without limit, which is what every
 * channel did before this and what a channel with one client wants.
 */
#define SYS_OP_CHAN_LISTEN       28

/*
 * Raise the top of the calling process's heap. There is no target: a
 * process moves its own top and no other's.
 *
 * The region above a program's image is unmapped at load, and this is
 * what opens it: the top moves up by the requested amount, the reach
 * check follows it so an address below the new top becomes one the
 * program may name, and at Tier 2 a store there faults onto a writable
 * page the pager supplies. Tier 0 has the frames already, flat in the
 * reserved slot, so the move is only the bound.
 *
 * Grow only for now. A delta is unsigned and the top does not come back
 * down, because reclaiming a page a program gave back is a later
 * version's problem; a delta of zero moves nothing and reads the top.
 * SYS_ERR_NO_MEMORY if the new top would pass the end of the slot.
 */
#define SYS_OP_HEAP_SET          29

#define SYS_OP_COUNT             30


/*
 * Per-entry flags.
 *
 * SYS_OP_LINK says the entry after this one belongs to the same chain.
 * Linked entries run in order, the first failure cancels the rest of
 * that chain with SYS_ERR_CANCELED, and the chain ends at the first
 * entry without the flag. Several chains may share one batch and one
 * failing does not touch another.
 *
 * It means abort-on-error grouping and nothing else. Entries run in
 * submission order whether linked or not, because the engine executes
 * in one thread, so chaining does not make anything concurrent.
 */
#define SYS_OP_LINK             (1u << 0)

/*
 * Where an entry's target comes from, and where its result goes.
 *
 * SYS_OP_TARGET_FROM_REG says the target is not the handle in the
 * descriptor but whatever register `reg_in` names. The register must
 * hold a handle: one holding a scalar, or holding nothing because no
 * earlier entry wrote it, fails the entry with SYS_ERR_INVALID_ARG
 * before the operation runs. That refusal is the whole reason the file
 * is typed, because the alternative is a handle made out of an integer
 * the caller chose.
 *
 * SYS_OP_RESULT_TO_REG says the handle this entry produces goes into
 * the register `reg_out` names. There are two producers: the
 * issuing ops, which are not built, and a send, whose reply may carry
 * a handle that moved with it. A reply that carries none leaves the
 * register empty rather than writing an invalid handle into it, so an
 * entry that depends on it fails on the tag instead of on a value that
 * looks like a handle and is not.
 */
#define SYS_OP_TARGET_FROM_REG  (1u << 1)
#define SYS_OP_RESULT_TO_REG    (1u << 2)

/*
 * SYS_OP_DEADLINE says the deadline in the argument structure means
 * something. Without it a blocking operation waits for ever, which is
 * SYS_DEADLINE_NEVER.
 *
 * A flag rather than a field that is always read, because
 * SYS_DEADLINE_NOW is zero and zero is what memset leaves behind. Every
 * program here clears its argument structure before filling in the
 * parts it cares about, so a deadline read unconditionally would turn
 * each of them into a poll, and the ones that meant to poll would be
 * indistinguishable from the ones that had never heard of deadlines.
 * Both sentinels' values are fixed, so the ambiguity is in the
 * interface and not something a different constant could remove: what
 * removes it is saying whether the field was filled in.
 */
#define SYS_OP_DEADLINE         (1u << 3)


/*
 * How many registers one execution has.
 *
 * Four because the chains of linked entries carry one intermediate
 * value each, a walk handing a connection to the entry that reads on
 * it, and a batch may hold several independent chains. It is interface
 * rather than configuration: a program names a register by number, so
 * changing this changes what a program may say.
 */
#define SYS_BATCH_REGS            4
/*
 * The reg_in and reg_out fields are meaningful only to the parts of the
 * submission engine that are not built. They are here so that the
 * descriptor does not change shape if they ever are. The flags field is
 * live: see SYS_OP_LINK above.
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
 * The message: a small inline payload, a handle that moves
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

/* Buffer access the receiver is being given. */
#define SYS_BUF_READ            (1u << 0)
#define SYS_BUF_WRITE           (1u << 1)

typedef uint64_t sys_thread_id_t;

/*
 * The low half of an identifier, which is the index and as much
 * generation as fits beside it. A pulse value is 32
 * bits and an identifier may be 64, so a pulse that named a thread by
 * bare index would be an ABA hazard: the slot is recycled between the
 * enqueue and the collection and a server matches the notice against
 * somebody else's request. Twelve generation bits mean a false match
 * needs four thousand recycles of one slot in that window.
 */
#define SYS_ID_TAG32(id)        ((uint32_t)((id) & 0xffffffffu))

/*
 * A pulse: a code and a value, no payload and no reply. Sending one
 * never blocks, which is what makes it safe from
 * places where blocking is not permitted.
 */
#define SYS_PULSE_DEAD_OWNER    1       /* system; uses the reserved slots */

/*
 * A connection on this channel has lost its last peer. A hint and
 * nothing more: what says which connection it was is
 * SYS_OP_CHAN_REAP, so this may be coalesced or lost without costing
 * anything. System, so a program cannot flood it out of the queue.
 */
#define SYS_PULSE_PEER_GONE     2
#define SYS_PULSE_USER_BASE     1024

typedef struct sys_pulse {
    uint16_t        code;
    uint16_t        _reserved;
    uint32_t        value;
} sys_pulse_t;

/*
 * What a receive found. A server has one receive point for messages and
 * pulses both, and these are how it tells them apart.
 *
 * SYS_MSG_PULSE_LOST says a pulse was dropped since the last receive.
 * It is reported once and cleared, because it answers "did I miss
 * anything" and not "how much". A server that sees this flag has to go
 * and look at whatever a pulse would have told it, and can do that by
 * collecting with SYS_OP_CHAN_REAP.
 */
#define SYS_MSG_IS_PULSE        (1u << 0)
#define SYS_MSG_PULSE_LOST      (1u << 3)

typedef struct sys_msg {
    uint32_t        flags;
    uint32_t        inline_len;

    /*
     * What the server made of the request, and the one field of a reply
     * the kernel reads.
     *
     * A transport that succeeded and a request that succeeded are
     * different things, and without this the kernel cannot tell them
     * apart: a walk for a file that does not exist is a completely
     * successful send, and a linked chain carries on to read from a
     * connection that was never returned. Servers report failure as
     * sys_err_t and invent no codes of their own, so this costs a
     * protocol nothing it was not already doing.
     *
     * The kernel attaches no meaning to it beyond zero and non-zero,
     * in exactly the way it attaches none to the cookie. Zero on a
     * request, where nothing reads it.
     */
    sys_err_t       status;
    uint32_t        _pad0;

    sys_handle_t    handle;     /* moved with the message, or INVALID */

    /*
     * Bulk data, moved for the duration of the call. The sender may not
     * touch this memory between send and reply, and the
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
     * buffer of its own memory, so a reply larger than the inline area
     * writes into the buffer the client supplied in the request.
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

/*
 * Argument structure for SYS_OP_CHAN_REAP. target names the channel.
 */
#define SYS_CHAN_REAP_ENABLE    (1u << 0)

/* Argument structure for SYS_OP_CHAN_LISTEN. target names the channel. */
typedef struct sys_op_chan_listen_args {
    uint32_t        depth;
    uint32_t        _reserved;
} sys_op_chan_listen_args_t;

typedef struct sys_op_chan_reap_args {

    uint32_t    flags;
    uint32_t    _reserved;
    uint64_t    out_cookie;     /* the collected connection's cookie */
} sys_op_chan_reap_args_t;

/* Argument structures for the three message operations. */
typedef struct sys_op_send_args {
    sys_msg_t       request;
    sys_msg_t       reply;      /* written by the kernel */

    /*
     * Absolute, on the monotonic timebase, and read only when the entry
     * carries SYS_OP_DEADLINE. A client that gives up
     * before a server answers is released with SYS_ERR_TIMEOUT, and
     * what the server sees depends on whether it had taken the message
     * yet.
     */
    uint64_t        deadline;
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
     * Absolute, on the monotonic timebase. A relative
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

/* Argument structure for SYS_OP_PROC_KILL. target names the process. */
typedef struct sys_op_kill_args {
    int32_t         code;       /* what its status will report */
    uint32_t        _pad;
} sys_op_kill_args_t;

/* Argument structure for SYS_OP_PULSE_SEND. target names the channel. */
typedef struct sys_op_pulse_args {
    sys_pulse_t     pulse;
} sys_op_pulse_args_t;

/*
 * Argument structure for SYS_OP_THREAD_MIGRATE. target names the
 * thread.
 */
typedef struct sys_op_migrate_args {
    uint32_t    core;
    uint32_t    pad;
} sys_op_migrate_args_t;

/*
 * Argument structure for SYS_OP_PROG_LOAD. target is unused; entries
 * names the array and count how many. The handle comes back in
 * out_handle.
 */
typedef struct sys_op_prog_args {
    uintptr_t       entries;
    uint32_t        count;
    uint32_t        pad;
    sys_handle_t    out_handle;
} sys_op_prog_args_t;

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
 * The state a status call reports.
 */
#define SYS_STATE_READY          0
#define SYS_STATE_RUNNING        1
#define SYS_STATE_BLOCKED        2
#define SYS_STATE_EXITED         3

/*
 * The fault kinds. Every synchronous exception a port can raise while a
 * thread is running maps to one of these, because a kind that is not in
 * the list has nowhere to go.
 *
 * SYS_FAULT_PAGE is not yet raised by either port. It is the one
 * recoverable kind, and what makes it recoverable is a pager. Until a
 * pager exists, a reference to an address the tables do not describe is
 * SYS_FAULT_ACCESS, because nothing could supply the page: reporting it
 * as recoverable would be reporting a mechanism the system does not have.
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
 * exit with a reason, so it travels the path an exit
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
 * And what a member of a cohort reports when a peer faulted and it went
 * with it. Not SYS_EXIT_FAULT, because this process
 * did not fault and its fault kind says so: a supervisor reading the
 * status of a torn-down cohort can tell which member was the reason
 * from which one reports a fault.
 */
#define SYS_EXIT_COHORT         (-17)

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
 * The fault fields describe a thread the kernel ended for a
 * fault has exited, so the state says EXITED and the code says
 * SYS_EXIT_FAULT, and these say which fault, where, and what instruction.
 * They are SYS_FAULT_NONE and zero for everything else, which includes a
 * thread that is still running.
 *
 * A process reports the fault of the thread whose exit finished it, which
 * with one thread per process is the program's own. Which thread speaks
 * for a process with several is left open, and so is this.
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
 * Argument structure for SYS_OP_HEAP_SET. There is no target.
 *
 * delta is how many bytes to raise the top by, which the kernel rounds up
 * to a page. out_base comes back as the old top, the first address of the
 * range the call just claimed, so a caller writes from there without
 * knowing where its heap began. A delta of zero returns the current top
 * and moves nothing.
 */
typedef struct sys_op_heap_args {
    uintptr_t       delta;          /* in: bytes to raise the top by */
    uintptr_t       out_base;       /* written by the kernel: the old top */
} sys_op_heap_args_t;

/*
 * A page fault, as the message the faulting thread sends: the address
 * it touched and what it was doing there, in the inline
 * payload, because a fault is small and a grant is what comes back.
 */
typedef struct sys_fault_msg {
    uintptr_t       addr;       /* the address, page-aligned */
    uintptr_t       pc;         /* the instruction that touched it */
    uint32_t        access;     /* SYS_RIGHT_READ, WRITE or EXEC */
    uint32_t        _pad;
} sys_fault_msg_t;

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
 * What a program may be started with. The kernel is the loader, so it
 * copies the strings into the process object alongside the record, and these
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
