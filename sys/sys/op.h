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

typedef int32_t   sys_err_t;
typedef uint64_t  sys_handle_t;

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

/* Issuing ops, which need a device that does DMA. */
#define SYS_OP_DMA_ISSUE          8
#define SYS_OP_READ_ISSUE         9
#define SYS_OP_WRITE_ISSUE       10

#define SYS_OP_COUNT             11

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
 * Argument structure for SYS_OP_FENCE_WAIT and SYS_OP_FENCE_SIGNAL. target
 * names the timeline. There is no deadline yet: section 14 has waits take
 * an absolute one, and absolute deadlines need a clock.
 */
typedef struct sys_op_fence_args {
    uint64_t        value;
} sys_op_fence_args_t;

/* Argument structure for SYS_OP_HANDLE_DUP. target names the source. */
typedef struct sys_op_dup_args {
    uint32_t        new_rights;
    uint32_t        _pad;
    sys_handle_t    out_handle;     /* written by the kernel */
} sys_op_dup_args_t;

#endif /* _SYS_OP_H_ */
