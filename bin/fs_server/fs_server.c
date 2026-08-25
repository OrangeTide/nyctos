/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * A filesystem server, as an ordinary program.
 *
 * It is handed a receive handle for its channel as SYS_NS_SERVICE in its
 * startup namespace, and creates nothing. There is no operation for making a channel, and
 * there should not be: a server that could conjure one would be naming
 * something nobody granted it. Section 24.5 has root create the channel
 * and pass both ends, which is what the namespace entry is.
 *
 * The loop is section 12.6: receive, switch on the operation, reply. The
 * cookie says which node the request is about, because the server put it
 * there when it minted the connection, and it is the only thing the server
 * needs to know about a caller.
 *
 * The program produces no output. There is no console for a user program
 * yet, so results travel by IPC and by exit status; a console service is
 * milestone 3.
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <sys/blk.h>
#include <sys/fs.h>

/*
 * Two directories, not one.
 *
 * A server serving one tree is a server that cannot demonstrate what a
 * union directory is, and section 20.4 makes a union a library feature
 * needing no kernel involvement: several handles under one name, tried in
 * order. Two directories from one server is the smallest thing that shows
 * it, and is honest about what a union actually joins, which is
 * directories rather than servers. Which one a connection sees is its
 * cookie, which is section 25.2's per-connection state and is chosen by
 * whoever minted the connection.
 *
 * `motd` is in both, deliberately and with different bytes in it, because
 * the whole of union semantics is which one wins.
 */
#define FS_NODE_ROOT    0
#define FS_NODE_ALT     2

/*
 * A file is a block and a length, and a directory is neither.
 *
 * The bytes are not here any more. They are in the block store this
 * server reads through, which is the whole of what this stage changed:
 * a file server with no copy of its own files has to ask for them, and
 * a request from a client now crosses two boundaries to reach them.
 *
 * FS_NOT_A_FILE rather than a separate flag, because every place that
 * used to ask whether a node had bytes has to keep asking the same
 * question, and a second field to keep consistent with the first is a
 * second field that will one day disagree.
 */
#define FS_NOT_A_FILE   (-1)

static const struct {
    const char *name;
    int32_t     block;      /* in the store, or FS_NOT_A_FILE */
    uint32_t    len;
    uint32_t    dir;        /* the directory this entry is in */
} fs_nodes[] = {
    { "/",     FS_NOT_A_FILE,       0,  FS_NODE_ROOT },
    { "motd",  BLK_MOTD_BLOCK,     BLK_MOTD_BYTES,     FS_NODE_ROOT },
    { "/",     FS_NOT_A_FILE,      0,                  FS_NODE_ALT },
    { "motd",  BLK_ALT_MOTD_BLOCK, BLK_ALT_MOTD_BYTES, FS_NODE_ALT },
    { "notes", BLK_NOTES_BLOCK,    BLK_NOTES_BYTES,    FS_NODE_ALT },
};

#define FS_NODE_COUNT   (sizeof(fs_nodes) / sizeof(fs_nodes[0]))

/*
 * Whether any component of a name is `..`, which is the one thing a
 * server must refuse whatever its own shape is. See sys/sys/fs.h.
 */
static int
climbs_out(const char *name, uint32_t namelen)
{
    uint32_t i = 0;

    while (i < namelen) {
        uint32_t start = i;

        while (i < namelen && name[i] != '/') {
            i++;
        }
        if (i - start == 2 && name[start] == '.' && name[start + 1] == '.') {
            return 1;
        }
        while (i < namelen && name[i] == '/') {
            i++;
        }
    }

    return 0;
}

/*
 * Resolve one name under this connection's node. A walk that would leave
 * the subtree the connection represents is refused here rather than
 * anywhere else, per section 25.3: it is a rule about the connection's own
 * scope, so the server needs to know nothing about the caller's namespace
 * to enforce it.
 *
 * Returns FS_NODE_COUNT for anything it will not resolve.
 */
static uint32_t
walk(uint32_t node, const char *name, uint32_t namelen)
{
    uint32_t i;

    if (namelen == 0 || namelen > FS_NAME_MAX) {
        return FS_NODE_COUNT;
    }
    if (node >= FS_NODE_COUNT || fs_nodes[node].block != FS_NOT_A_FILE) {
        return FS_NODE_COUNT;       /* not a directory */
    }

    for (i = 0; i < FS_NODE_COUNT; i++) {
        if (fs_nodes[i].dir != node || fs_nodes[i].block == FS_NOT_A_FILE) {
            continue;
        }
        if (strlen(fs_nodes[i].name) == namelen &&
          strncmp(fs_nodes[i].name, name, namelen) == 0) {
            return i;
        }
    }

    return FS_NODE_COUNT;
}

/*
 * The block store, and one block of it.
 *
 * The buffer is this server's own memory, because that is the only
 * memory it may lend: section 23.2 checks a buffer descriptor against
 * the sender's own image, and a client's buffer is mapped here at the
 * client's address without becoming this process's to hand on. So a
 * read through two layers copies once, here, and the copy is not
 * something a cohort removes. What a cohort removes is the mapping, and
 * stage five is where that shows.
 *
 * One block, because every file here fits in one and a second would be
 * memory spent on a case the check never reaches. A file spanning
 * blocks reads them one at a time through this.
 */
static sys_handle_t store;
static uint8_t block[BLK_BLOCK_BYTES];

/*
 * Ask the store for the block at this offset. Returns what the store
 * said, so a store that refuses is a read that fails rather than a read
 * that quietly returns nothing.
 */
static sys_err_t
store_read(uint32_t blocknr)
{
    const fs_reply_t *rep;
    fs_request_t *req;
    sys_msg_t message, reply;
    sys_err_t err;

    if (store == SYS_HANDLE_INVALID) {
        return SYS_ERR_NOT_SUPPORTED;
    }

    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);
    message.buf = (uintptr_t)block;
    message.buf_len = sizeof(block);
    message.buf_flags = SYS_BUF_WRITE;

    req = (fs_request_t *)message.inline_data;
    req->fr_op = FS_OP_READ;
    req->fr_offset = (uint64_t)blocknr * BLK_BLOCK_BYTES;
    req->fr_length = BLK_BLOCK_BYTES;

    err = sys_msg_send(store, &message, &reply);
    if (err != SYS_OK) {
        return err;
    }

    rep = (const fs_reply_t *)reply.inline_data;
    if (rep->fr_status != SYS_OK) {
        return rep->fr_status;
    }

    /*
     * How much the store says it wrote, and not merely that it said
     * yes. The buffer below is reused from one block to the next, so a
     * store answering SYS_OK without filling it would have this server
     * copy out the block it fetched last time, which is one file's
     * bytes served as another's. Reading the field that says so costs
     * a comparison; not reading it costs a leak that looks like data.
     */
    if (rep->fr_length != BLK_BLOCK_BYTES) {
        return SYS_ERR_TRUNCATED;
    }

    return SYS_OK;
}

/*
 * Copy file bytes into the buffer the client supplied, per the reply-path
 * caveat of section 6.1: a server does not hand back memory of its own,
 * because it could never tell when the client had finished with it.
 *
 * Reading past the end yields a short count and not an error. A client
 * that does not know a file's length has to be able to ask for more than
 * there is, which is section 25.7's rule and the reason a compound read
 * does not abort its own sequence.
 *
 * A store that refuses is different, and returns a short count of zero
 * having failed rather than a short count of zero having reached the
 * end. The caller can tell them apart because this reports the error
 * separately; before the store existed there was nothing that could
 * fail here and nothing to report.
 */
static uint32_t
read_into(void *dst, uint32_t dstlen, uint32_t node, uint64_t off,
  uint32_t want, sys_err_t *err)
{
    uint32_t done = 0;
    uint32_t avail;

    *err = SYS_OK;

    if (dst == NULL || node >= FS_NODE_COUNT ||
      fs_nodes[node].block == FS_NOT_A_FILE) {
        return 0;
    }

    avail = off < fs_nodes[node].len ?
      fs_nodes[node].len - (uint32_t)off : 0;
    if (avail > want) {
        avail = want;
    }
    if (avail > dstlen) {
        avail = dstlen;
    }

    /*
     * Block at a time, because that is what the store serves. The file
     * starts at a block boundary, so the block a byte offset falls in
     * is arithmetic and not a lookup, which is the whole reason a
     * filesystem sits above a block store rather than beside it.
     */
    while (done < avail) {
        uint64_t pos = off + done;
        uint32_t within = (uint32_t)(pos % BLK_BLOCK_BYTES);
        uint32_t n = BLK_BLOCK_BYTES - within;

        if (n > avail - done) {
            n = avail - done;
        }

        *err = store_read((uint32_t)fs_nodes[node].block +
          (uint32_t)(pos / BLK_BLOCK_BYTES));
        if (*err != SYS_OK) {
            return 0;
        }

        memcpy((uint8_t *)dst + done, &block[within], n);
        done += n;
    }

    return done;
}

/* The length of a fixed-width name field, which need not be terminated. */
static uint32_t
name_len(const char *field)
{
    uint32_t i;

    for (i = 0; i < FS_NAME_MAX; i++) {
        if (field[i] == '\0') {
            break;
        }
    }

    return i;
}

static void
do_walk(sys_handle_t chan, const fs_request_t *req, uint32_t node,
  sys_msg_t *reply, fs_reply_t *rep)
{
    sys_handle_t conn;
    uint32_t found;
    uint32_t len;

    len = name_len(req->fr_name);

    /*
     * Not a directory before not permitted. A file connection has no
     * subtree to climb out of, so answering a `..` on one with a
     * refusal would be the right answer to a question nobody asked:
     * what is wrong is that this connection does not name a place
     * things can be found in.
     */
    if (node >= FS_NODE_COUNT || fs_nodes[node].block != FS_NOT_A_FILE) {
        rep->fr_status = SYS_ERR_NO_ENT;
        return;
    }

    /*
     * And then the backstop of section 20.3, before anything is
     * looked up: a client may not reach above the root of the
     * subtree this connection represents. See sys/sys/fs.h for why
     * the refusal is this server's to make and why refusing every
     * `..` is stricter than the rule.
     */
    if (climbs_out(req->fr_name, len)) {
        rep->fr_status = SYS_ERR_PERM_DENIED;
        return;
    }

    found = walk(node, req->fr_name, len);
    if (found >= FS_NODE_COUNT) {
        rep->fr_status = SYS_ERR_NO_ENT;
        return;
    }

    /*
     * A new connection naming the node found, handed back in the message.
     * The client could not have constructed this itself, which is what it
     * means for the authority to be a handle rather than a name.
     *
     * This is the allocation section 25.7 exists to avoid: a client that
     * only wants to read the file end to end holds this across two more
     * round trips and then closes it.
     */
    rep->fr_status = sys_chan_connect(chan, found,
      SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER, &conn);
    if (rep->fr_status == SYS_OK) {
        reply->handle = conn;
        rep->fr_size = fs_nodes[found].len;
    }
}

static void
do_read(const sys_msg_t *message, const fs_request_t *req, uint32_t node,
  sys_msg_t *reply, fs_reply_t *rep)
{
    if (node >= FS_NODE_COUNT || fs_nodes[node].block == FS_NOT_A_FILE) {
        rep->fr_status = SYS_ERR_NO_ENT;
        return;
    }

    /*
     * Into the buffer the client supplied, or inline when it supplied
     * none. Section 25.5 puts bulk payloads in the buffer and small
     * ones inline, and which one a reply is depends on the client's
     * request rather than on which server answered it: a client that
     * asks a console and a client that asks a filesystem should not
     * have to send different messages to read a line.
     */
    if (message->buf != 0 && message->buf_len > 0) {
        rep->fr_length = read_into((void *)message->buf,
          message->buf_len, node, req->fr_offset, req->fr_length,
          &rep->fr_status);
    } else {
        rep->fr_length = read_into(&reply->inline_data[sizeof(*rep)],
          SYS_IPC_INLINE_BYTES - (uint32_t)sizeof(*rep), node,
          req->fr_offset, req->fr_length, &rep->fr_status);
    }
}

/*
 * Execute a compound request, per section 25.7.
 *
 * Bounds first, then execution: the whole sequence is checked against the
 * payload it arrived in before any entry runs, which is the same reason
 * section 17.2 validates a batch once rather than as it goes. A sequence
 * that does not fit is rejected and nothing has happened.
 *
 * Chaining is implicit. `node` starts as the connection's node and each
 * entry replaces it, so an entry always acts on what the entry before it
 * produced. Nothing durable is created: the node exists in this variable
 * and is gone when the reply is sent, which is what keeps the connection
 * pool untouched.
 */
static void
do_batch(const sys_msg_t *message, uint32_t node, sys_msg_t *reply,
  fs_batch_reply_t *rep)
{
    const fs_batch_t *hdr = (const fs_batch_t *)message->inline_data;
    uint32_t off, i;

    rep->fb_status = SYS_OK;
    rep->fb_failed = 0;
    rep->fb_completed = 0;
    rep->fb_length = 0;

    if (hdr->fb_count == 0 || hdr->fb_count > FS_BATCH_MAX) {
        rep->fb_status = SYS_ERR_INVALID_ARG;
        return;
    }

    /*
     * Walk the sequence once to check it fits, before running any of it.
     * A name that runs past the end of the payload would otherwise be read
     * out of bounds by the entry that used it.
     */
    off = sizeof(*hdr);
    for (i = 0; i < hdr->fb_count; i++) {
        const fs_entry_t *e;

        if (off + sizeof(*e) > message->inline_len) {
            rep->fb_status = SYS_ERR_INVALID_ARG;
            return;
        }
        e = (const fs_entry_t *)&message->inline_data[off];
        off += (uint32_t)sizeof(*e) + FS_ENTRY_PAD(e->fe_namelen);
        if (off > message->inline_len) {
            rep->fb_status = SYS_ERR_INVALID_ARG;
            return;
        }
    }

    off = sizeof(*hdr);
    for (i = 0; i < hdr->fb_count; i++) {
        const fs_entry_t *e = (const fs_entry_t *)&message->inline_data[off];
        const char *name = (const char *)(e + 1);

        switch (e->fe_op) {
        case FS_OP_WALK:
            node = walk(node, name, e->fe_namelen);
            if (node >= FS_NODE_COUNT) {
                rep->fb_status = SYS_ERR_NO_ENT;
                rep->fb_failed = i;
                return;
            }
            break;

        case FS_OP_READ:
            /*
             * Into the buffer the client supplied, per the reply-path
             * caveat of section 6.1. A short read is a success: a client
             * that does not know a file's length must be able to ask for
             * more than there is without aborting its own sequence.
             */
            if (node >= FS_NODE_COUNT || fs_nodes[node].block == FS_NOT_A_FILE) {
                rep->fb_status = SYS_ERR_NO_ENT;
                rep->fb_failed = i;
                return;
            }
            {
                sys_err_t serr;

                rep->fb_length = read_into((void *)message->buf,
                  message->buf_len, node, e->fe_offset, e->fe_length,
                  &serr);
                if (serr != SYS_OK) {
                    rep->fb_status = serr;
                    rep->fb_failed = i;
                    return;
                }
            }
            break;

        default:
            rep->fb_status = SYS_ERR_INVALID_ARG;
            rep->fb_failed = i;
            return;
        }

        rep->fb_completed++;
        off += (uint32_t)sizeof(*e) + FS_ENTRY_PAD(e->fe_namelen);
    }

    (void)reply;
}

int
main(void)
{
    sys_handle_t chan;
    sys_msg_t message, reply;
    sys_thread_id_t peer;
    fs_request_t *req;
    fs_reply_t *rep;
    uint64_t cookie;
    uint32_t node;

    chan = ns_lookup(SYS_NS_SERVICE);
    if (chan == SYS_HANDLE_INVALID) {
        return 1;
    }

    /*
     * The store this server reads through. Not optional: with the
     * bytes in the block store there is nothing here to serve without
     * it, and a file server that started anyway would answer every
     * read with an error nobody could act on.
     */
    store = ns_lookup(SYS_NS_STORE);
    if (store == SYS_HANDLE_INVALID) {
        return 4;
    }

    for (;;) {
        if (sys_msg_recv(chan, &message, &peer, &cookie) != SYS_OK) {
            return 1;
        }

        req = (fs_request_t *)message.inline_data;
        node = (uint32_t)cookie;

        memset(&reply, 0, sizeof(reply));
        reply.handle = SYS_HANDLE_INVALID;
        reply.inline_len = sizeof(*rep);
        rep = (fs_reply_t *)reply.inline_data;
        rep->fr_status = SYS_ERR_NO_ENT;

        switch (req->fr_op) {
        case FS_OP_WALK:
            do_walk(chan, req, node, &reply, rep);
            break;
        case FS_OP_READ:
            do_read(&message, req, node, &reply, rep);
            reply.inline_len = sizeof(*rep) + rep->fr_length;
            break;
        case FS_OP_CLOSE:
            rep->fr_status = SYS_OK;
            break;
        case FS_OP_BATCH:
            reply.inline_len = sizeof(fs_batch_reply_t);
            do_batch(&message, node, &reply,
              (fs_batch_reply_t *)reply.inline_data);
            break;
        default:
            rep->fr_status = SYS_ERR_INVALID_ARG;
            break;
        }

        if (sys_msg_reply(peer, &reply) != SYS_OK) {
            return 1;
        }
    }
}
