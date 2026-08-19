/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The filesystem protocol of section 25, in the subset the vertical slice
 * needs: walk, read, close.
 *
 * It is a protocol rather than a layer. Nothing here is a kernel
 * interface; these are the messages a filesystem server understands, and
 * anything that speaks them is a filesystem. The kernel carries them and
 * does not know what they mean.
 *
 * A connection names a node. Walking one returns a connection to another,
 * so a client reaches a file by being handed a directory and asking, never
 * by naming a path from a root it was not given. There is no root here at
 * all, which is what section 20.3 means by naming being handle-relative.
 *
 * A connection carries no position. Every read states its own offset, so
 * two threads may share one connection without coordinating and a retry
 * means what the original meant.
 */

#ifndef _SYS_FS_H_
#define _SYS_FS_H_

#include <sys/op.h>

#include <stdint.h>

#define FS_OP_WALK      1
#define FS_OP_READ      2
#define FS_OP_CLOSE     3

#define FS_NAME_MAX     24

/* Request, carried in the inline payload of a message. */
typedef struct fs_request {
    uint32_t    fr_op;
    uint32_t    fr_length;
    uint64_t    fr_offset;
    char        fr_name[FS_NAME_MAX];
} fs_request_t;

/*
 * Reply. A walk answers with a connection in the message handle rather
 * than in the payload, because it is authority and not data.
 */
typedef struct fs_reply {
    sys_err_t   fr_status;
    uint32_t    fr_length;
    uint64_t    fr_size;
} fs_reply_t;

#endif /* _SYS_FS_H_ */
