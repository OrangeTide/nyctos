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

/*
 * The operation numbers are section 25.3's, and the subset is sparse
 * rather than renumbered. A dense subset with its own numbering is a
 * second protocol that happens to look like this one, and the first
 * program written against the document would speak it wrongly. What is
 * missing here is missing, not moved.
 */
#define FS_OP_WALK      1
#define FS_OP_READ      3
#define FS_OP_WRITE     4
#define FS_OP_CLOSE     11
#define FS_OP_BATCH     12

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

/*
 * A write carries its bytes inline, after the request header, exactly as a
 * read carries them back after the reply header. Section 25.5 puts bulk
 * payloads in the message buffer instead; what is here is the small case,
 * and a console line is the small case by construction.
 *
 * fr_length is how many bytes follow. The reply reports how many were
 * taken, which for a device that cannot refuse is all of them.
 */
#define FS_WRITE_MAX    (SYS_IPC_INLINE_BYTES - sizeof(fs_request_t))

/*
 * Compound requests, per section 25.7.
 *
 * Walk and read in one message instead of three. What this saves is not
 * mainly the four context switches: it is that the node the walk produces
 * lives only inside the server's execution of one request, so a whole-file
 * read stops drawing a connection from the pool section 25.2 requires a
 * build to size for every file open at once.
 *
 * Chaining is implicit and linear. Each entry operates on the node the
 * previous entry produced, and the first operates on the connection the
 * message arrived on. There is no reference field, because the only
 * sequence this exists for is a straight line and section 17.5 makes the
 * same argument about LINK: chains are not graphs. An entry that wants to
 * name something other than what came before it is a different request.
 *
 * There is no close entry. Section 25.7 releases an intermediate node when
 * the reply is sent whether or not anything asked, so a close inside a
 * compound would be a no-op that cost sixteen bytes of a sixty-four byte
 * payload.
 *
 * The whole sequence travels inline. It is small, bounded and fixed in
 * advance, which is the same reason section 17.2 can validate a batch once
 * before executing any of it. Only bulk data needs the message buffer.
 */
#define FS_BATCH_MAX    4

typedef struct fs_batch {
    uint32_t    fb_op;          /* FS_OP_BATCH */
    uint32_t    fb_count;       /* entries following this header */
} fs_batch_t;

/*
 * One entry, followed by fe_namelen path bytes padded to eight. Sixteen
 * bytes so that a walk and a read and their names fit an inline payload
 * with room to spare.
 */
typedef struct fs_entry {
    uint8_t     fe_op;
    uint8_t     fe_flags;
    uint16_t    fe_namelen;     /* path bytes following, padded to eight */
    uint32_t    fe_length;      /* READ: bytes wanted */
    uint64_t    fe_offset;      /* READ: where to start */
} fs_entry_t;

/*
 * The reply. There is no per-entry status, because abort-at-first-failure
 * makes one redundant: every entry below fb_failed succeeded and every
 * entry above it did not run. What is not redundant is a count, since a
 * short read is a success, so the one data-bearing entry reports its
 * length here.
 */
typedef struct fs_batch_reply {
    sys_err_t   fb_status;      /* the first failure, or SYS_OK */
    uint32_t    fb_failed;      /* entry that failed, when status is not OK */
    uint32_t    fb_completed;   /* entries that ran to completion */
    uint32_t    fb_length;      /* bytes the read produced */
} fs_batch_reply_t;

_Static_assert(sizeof(fs_batch_t) == 8, "compound header is 8 bytes");
_Static_assert(sizeof(fs_entry_t) == 16, "compound entry is 16 bytes");
_Static_assert(sizeof(fs_batch_reply_t) == 16, "compound reply is 16 bytes");

/* Round a name length up to the entry alignment. */
#define FS_ENTRY_PAD(n) (((n) + 7u) & ~7u)

#endif /* _SYS_FS_H_ */
