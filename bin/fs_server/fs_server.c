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
#include <sys/fs.h>

#define FS_NODE_ROOT    0
#define FS_NODE_MOTD    1

static const char motd_text[] = "served from outside the kernel\n";

static const struct {
    const char *name;
    const char *data;
    uint32_t    len;
} fs_nodes[] = {
    { "/",    NULL,      0 },
    { "motd", motd_text, sizeof(motd_text) - 1 },
};

#define FS_NODE_COUNT   (sizeof(fs_nodes) / sizeof(fs_nodes[0]))

/*
 * Resolve one name under this connection's node. A walk that would leave
 * the subtree the connection represents is refused here rather than
 * anywhere else, per section 25.3: it is a rule about the connection's own
 * scope, so the server needs to know nothing about the caller's namespace
 * to enforce it.
 */
static uint32_t
walk(uint32_t node, const char *name)
{
    uint32_t i;

    if (node != FS_NODE_ROOT) {
        return FS_NODE_COUNT;
    }
    for (i = 1; i < FS_NODE_COUNT; i++) {
        if (strncmp(fs_nodes[i].name, name, FS_NAME_MAX) == 0) {
            return i;
        }
    }

    return FS_NODE_COUNT;
}

static void
do_walk(sys_handle_t chan, const fs_request_t *req, uint32_t node,
  sys_msg_t *reply, fs_reply_t *rep)
{
    sys_handle_t conn;
    uint32_t found;

    found = walk(node, req->fr_name);
    if (found >= FS_NODE_COUNT) {
        rep->fr_status = SYS_ERR_NO_ENT;
        return;
    }

    /*
     * A new connection naming the node found, handed back in the message.
     * The client could not have constructed this itself, which is what it
     * means for the authority to be a handle rather than a name.
     */
    rep->fr_status = sys_chan_connect(chan, found,
      SYS_RIGHT_SEND | SYS_RIGHT_TRANSFER, &conn);
    if (rep->fr_status == SYS_OK) {
        reply->handle = conn;
        rep->fr_size = fs_nodes[found].len;
    }
}

static void
do_read(const fs_request_t *req, uint32_t node, sys_msg_t *reply,
  fs_reply_t *rep)
{
    uint32_t avail, room;
    uint64_t off;

    if (node >= FS_NODE_COUNT || fs_nodes[node].data == NULL) {
        rep->fr_status = SYS_ERR_NO_ENT;
        return;
    }

    off = req->fr_offset;
    avail = off < fs_nodes[node].len ?
      fs_nodes[node].len - (uint32_t)off : 0;
    if (avail > req->fr_length) {
        avail = req->fr_length;
    }

    /*
     * Everything travels inline for now. Section 25.5 puts bulk payloads
     * in a grant; there are no grants yet, so a short count is the honest
     * answer and section 25.7 says a short read is not a failure.
     */
    room = SYS_IPC_INLINE_BYTES - (uint32_t)sizeof(*rep);
    if (avail > room) {
        avail = room;
    }

    memcpy(&reply->inline_data[sizeof(*rep)], &fs_nodes[node].data[off],
      avail);
    reply->inline_len = (uint32_t)sizeof(*rep) + avail;
    rep->fr_length = avail;
    rep->fr_status = SYS_OK;
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
            do_read(req, node, &reply, rep);
            break;
        case FS_OP_CLOSE:
            rep->fr_status = SYS_OK;
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
