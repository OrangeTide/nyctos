/* fs.h : filesystem protocol operations and message formats.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Defines the FS_OP_* operation numbers, the FS_KIND_* node kinds, and the
 * request, reply, directory entry, and batch structures that filesystem
 * servers exchange with clients. A connection names one node and carries
 * no read position, so each read supplies its own offset. Included by the
 * filesystem and block servers, the filesystem client programs, and the C
 * library.
 *
 */

#ifndef _SYS_FS_H_
#define _SYS_FS_H_

#include <sys/op.h>
#include <sys/wire.h>

#include <stdint.h>

/*
 * The operation numbers are the protocol's, and the subset is sparse
 * rather than renumbered. A dense subset with its own numbering is a
 * second protocol that happens to look like this one, and the first
 * program written against the document would speak it wrongly. What is
 * missing here is missing, not moved.
 */
#define FS_OP_WALK      1
#define FS_OP_READ      3
#define FS_OP_WRITE     4
#define FS_OP_CREATE    5
#define FS_OP_UNLINK    6
#define FS_OP_READDIR   8
#define FS_OP_TRUNCATE  9
#define FS_OP_CLOSE     11
#define FS_OP_BATCH     12

/*
 * The kind of a node, reported by a directory read so a reader can tell a
 * subdirectory from a file without walking to it.
 */
#define FS_KIND_FILE    1
#define FS_KIND_DIR     2

/*
 * A directory entry, packed into the reply buffer by FS_OP_READDIR: this
 * header, then fd_namelen name bytes, padded to eight so the next entry is
 * aligned. A reader steps by sizeof(fs_dirent_t) plus the padded name.
 */
typedef struct fs_dirent {
    uint32_t    fd_kind;        /* FS_KIND_FILE or FS_KIND_DIR */
    uint32_t    fd_namelen;     /* name bytes following, padded to eight */
} fs_dirent_t;

/*
 * The reply to FS_OP_READDIR. The request carries the cursor in fr_offset,
 * zero to start, and this returns how many entries were packed and the
 * cursor to resume from. The cursor is opaque: a client stores it and
 * sends it back, and may not compute one. It names a position and not a
 * snapshot, so an entry unchanged across the whole read appears exactly
 * once, and a cursor minted against one directory is refused against
 * another. See PROPOSALS.md, "Walk and Directory-Read Semantics".
 */
typedef struct fs_readdir_reply {
    sys_err_t   fr_status;
    uint32_t    fr_count;       /* entries packed into the buffer */
    uint64_t    fr_cursor;      /* pass as fr_offset in the next request */
} fs_readdir_reply_t;

/* Round a directory entry's name length up to the entry alignment. */
#define FS_DIRENT_PAD(n)    (((n) + 7u) & ~7u)

/*
 * FS_OP_CREATE and FS_OP_UNLINK name a single entry in the directory the
 * connection names, carried in fr_name, not a path. A single component
 * carries no `..` and crosses no `/`, so the subtree backstop is
 * automatic and a client that wants to act deeper walks to the
 * parent first.
 *
 * A create reads fr_length as the kind: FS_KIND_DIR makes a directory, and
 * anything else a file. It answers as a walk does, with a connection to
 * the new node in the reply handle, so what was made can be used at once.
 * A create under a connection that names a file rather than a directory is
 * SYS_ERR_WRONG_TYPE, the same answer a walk gives a non-directory in the
 * middle of a path.
 *
 * An unlink frees the node and answers with status alone. A connection a
 * client already holds to the unlinked node is not revoked, because an
 * unlink does not reach across the rendezvous; the next request on that
 * connection is answered SYS_ERR_NO_ENT. There is as yet no EEXIST for a
 * name a create finds taken, and no ENOTEMPTY for an unlink of a directory
 * that still has children; both are SYS_ERR_INVALID_ARG until the error
 * set grows the distinction.
 *
 * FS_OP_TRUNCATE sets the length of the node the connection names, carried
 * in fr_length. A length below the current one drops the bytes above it; a
 * length above it is not a grow on its own, since a file grows where a
 * write reaches past its end. It answers with status and the new size.
 */


/*
 * What a walk carries, and the one rule a server must enforce itself.
 *
 * The name is relative to the connection the request arrives on, and may
 * have several components in it. The C library reduces
 * `.` and `..` lexically before it chooses which handle to send to, and
 * that rule is not a security measure: it exists so that a path crossing
 * a namespace boundary reaches the right server, because a server cannot
 * see the boundary above itself and could only refuse.
 *
 * **A server must refuse to resolve above the root of the subtree its
 * channel represents.** That is this rule, and it is the backstop for a
 * client that builds its own messages rather than calling the library. A
 * program is entitled to do that and gains nothing by it, because it
 * still holds only the handles it was given; what it must not gain is
 * reach above the one it is using, which is why the refusal is the
 * server's to make and not the library's.
 *
 * It is a rule about a channel's own scope, so a server needs to know
 * nothing about any client's namespace to enforce it.
 *
 * How to enforce it is the server's choice, and the two ways are not the
 * same rule. A server with no hierarchy to speak of refuses any name
 * with a `..` component in it, which is simple, needs no walk, and is
 * stricter than the rule: `a/../b` never leaves the subtree and is
 * refused anyway. A server with real directories may instead resolve the
 * name and check that where it arrived is at or below its own root,
 * which is what the rule actually says and what a hierarchy needs, since
 * a client is entitled to write a path that goes down and comes back up.
 *
 * The one this tree has takes the strict route because it has one level
 * and nothing to walk. A server implementing
 * physical parent links must apply its root check to the physical walk,
 * because lexical and physical resolution diverge where a name is bound
 * to a directory elsewhere in the tree.
 *
 * The refusal is SYS_ERR_PERM_DENIED and deliberately not SYS_ERR_NO_ENT.
 * A client that is told a name does not exist will look for it somewhere
 * else; a client that is told it may not ask has learned the true thing.
 */
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
 * Pack and unpack the request and reply through a payload cursor, so a
 * program reads one field at a time with the bounds checked rather than
 * casting a struct onto bytes a client controls. The byte layout is the
 * struct's own: a put or a get in field order reproduces it, and a get
 * past the payload clears the cursor's flag rather than reading beyond it.
 */
static inline void
fs_request_put(struct wire_out *o, const fs_request_t *r)
{
    wire_put_u32(o, r->fr_op);
    wire_put_u32(o, r->fr_length);
    wire_put_u64(o, r->fr_offset);
    wire_put_bytes(o, (const uint8_t *)r->fr_name, FS_NAME_MAX);
}

static inline void
fs_request_get(struct wire_in *in, fs_request_t *r)
{
    r->fr_op = wire_get_u32(in);
    r->fr_length = wire_get_u32(in);
    r->fr_offset = wire_get_u64(in);
    wire_get_bytes(in, (uint8_t *)r->fr_name, FS_NAME_MAX);
}

static inline void
fs_reply_put(struct wire_out *o, const fs_reply_t *r)
{
    wire_put_u32(o, (uint32_t)r->fr_status);
    wire_put_u32(o, r->fr_length);
    wire_put_u64(o, r->fr_size);
}

static inline void
fs_reply_get(struct wire_in *in, fs_reply_t *r)
{
    r->fr_status = (sys_err_t)wire_get_u32(in);
    r->fr_length = wire_get_u32(in);
    r->fr_size = wire_get_u64(in);
}

/* Pack the directory-read reply header through the cursor. */
static inline void
fs_readdir_reply_put(struct wire_out *o, const fs_readdir_reply_t *r)
{
    wire_put_u32(o, (uint32_t)r->fr_status);
    wire_put_u32(o, r->fr_count);
    wire_put_u64(o, r->fr_cursor);
}

/*
 * A write carries its bytes inline, after the request header, exactly as a
 * read carries them back after the reply header. Bulk
 * payloads in the message buffer instead; what is here is the small case,
 * and a console line is the small case by construction.
 *
 * fr_length is how many bytes follow. The reply reports how many were
 * taken, which for a device that cannot refuse is all of them.
 */
#define FS_WRITE_MAX    (SYS_IPC_INLINE_BYTES - sizeof(fs_request_t))

/*
 * Compound requests.
 *
 * Walk and read in one message instead of three. What this saves is not
 * mainly the four context switches: it is that the node the walk produces
 * lives only inside the server's execution of one request, so a whole-file
 * read stops drawing a connection from the pool that a
 * build to size for every file open at once.
 *
 * Chaining is implicit and linear. Each entry operates on the node the
 * previous entry produced, and the first operates on the connection the
 * message arrived on. There is no reference field, because the only
 * sequence this exists for is a straight line, and the
 * same argument about LINK: chains are not graphs. An entry that wants to
 * name something other than what came before it is a different request.
 *
 * There is no close entry. An intermediate node is released when
 * the reply is sent whether or not anything asked, so a close inside a
 * compound would be a no-op that cost sixteen bytes of a sixty-four byte
 * payload.
 *
 * The whole sequence travels inline. It is small, bounded and fixed in
 * advance, which is the same reason a batch can be validated once
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

/* Pack the compound reply through the cursor, in the struct's field order. */
static inline void
fs_batch_reply_put(struct wire_out *o, const fs_batch_reply_t *r)
{
    wire_put_u32(o, (uint32_t)r->fb_status);
    wire_put_u32(o, r->fb_failed);
    wire_put_u32(o, r->fb_completed);
    wire_put_u32(o, r->fb_length);
}

/* Unpack the compound reply a client built with fs_batch_reply_put. */
static inline void
fs_batch_reply_get(struct wire_in *in, fs_batch_reply_t *r)
{
    r->fb_status = (sys_err_t)wire_get_u32(in);
    r->fb_failed = wire_get_u32(in);
    r->fb_completed = wire_get_u32(in);
    r->fb_length = wire_get_u32(in);
}

/* Round a name length up to the entry alignment. */
#define FS_ENTRY_PAD(n) (((n) + 7u) & ~7u)

/* Pack the compound header through the cursor, in the struct's field order. */
static inline void
fs_batch_put(struct wire_out *o, const fs_batch_t *h)
{
    wire_put_u32(o, h->fb_op);
    wire_put_u32(o, h->fb_count);
}

/*
 * Pack one compound entry, then its name padded to the entry alignment, the
 * layout do_batch consumes. The name length is the entry's own fe_namelen,
 * and a null name is a zero-length one.
 */
static inline void
fs_entry_put(struct wire_out *o, const fs_entry_t *e, const char *name)
{
    uint16_t namelen = e->fe_namelen;

    wire_put_u8(o, e->fe_op);
    wire_put_u8(o, e->fe_flags);
    wire_put_u16(o, e->fe_namelen);
    wire_put_u32(o, e->fe_length);
    wire_put_u64(o, e->fe_offset);
    if (namelen != 0) {
        wire_put_bytes(o, (const uint8_t *)name, namelen);
        wire_put_zeros(o, FS_ENTRY_PAD(namelen) - namelen);
    }
}

#endif /* _SYS_FS_H_ */
