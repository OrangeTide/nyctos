/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Opening a file and reading it, the plain way: walk, read, close.
 *
 * Three round trips, six context switches, and a connection held in the
 * server's pool across two of them. This is the baseline that fs_batch is
 * measured against; section 25.7 is the argument that it is the wrong
 * shape for a client that only wants the bytes.
 *
 * It is handed one connection, as SYS_NS_ROOT, and holds nothing else.
 * There is no root to name, no path that starts anywhere but the
 * connection it was given, and nothing it could have guessed.
 *
 * The program produces no output. There is no console for a user program
 * yet, so the byte count comes back as the exit status.
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <sys/fs.h>

static char buf[256];

static sys_err_t
request(sys_handle_t target, uint32_t op, const char *name, uint64_t off,
  uint32_t len, sys_msg_t *reply)
{
    fs_request_t *req;
    sys_msg_t message;
    sys_err_t err;

    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);

    /*
     * The buffer travels with every request, because a reply larger than
     * the inline payload has nowhere else to go: section 6.1 forbids the
     * server handing back memory of its own.
     */
    message.buf = (uintptr_t)buf;
    message.buf_len = sizeof(buf);
    message.buf_flags = SYS_BUF_WRITE;

    req = (fs_request_t *)message.inline_data;
    req->fr_op = op;
    req->fr_offset = off;
    req->fr_length = len;
    if (name != NULL) {
        memcpy(req->fr_name, name, strlen(name) + 1);
    }

    err = sys_msg_send(target, &message, reply);
    if (err != SYS_OK) {
        return err;
    }

    return ((const fs_reply_t *)reply->inline_data)->fr_status;
}

int
main(void)
{
    const fs_reply_t *rep;
    sys_handle_t dir, file;
    sys_msg_t reply;
    uint32_t got;

    dir = ns_lookup(SYS_NS_ROOT);
    if (dir == SYS_HANDLE_INVALID) {
        return 1;
    }

    if (request(dir, FS_OP_WALK, "motd", 0, 0, &reply) != SYS_OK) {
        return 2;
    }
    file = reply.handle;

    if (request(file, FS_OP_READ, NULL, 0, sizeof(buf), &reply) != SYS_OK) {
        return 3;
    }
    rep = (const fs_reply_t *)reply.inline_data;
    got = rep->fr_length;

    /*
     * The close is what returns the server's per-connection record to its
     * pool. Section 25.2 reclaims it here and nowhere else, because
     * destroying the handle locally would tell the server nothing.
     */
    if (request(file, FS_OP_CLOSE, NULL, 0, 0, &reply) != SYS_OK) {
        return 4;
    }
    (void)sys_handle_close(file);

    return (int)got;
}
