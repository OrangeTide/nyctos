/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * A program that opens a file and reads it.
 *
 * It is handed one connection, as SYS_NS_ROOT, and holds nothing else. There is
 * no root to name, no path that starts anywhere but the connection it was
 * given, and nothing it could have guessed: section 20.3 is what makes the
 * directory it was handed the entirety of what it can reach.
 *
 * Walk, read, close. Three round trips, which is what section 25.7 exists
 * to collapse once a server implements the compound form.
 *
 * The program produces no output. There is no console for a user program
 * yet, so the byte count comes back as the exit status; a console service
 * is milestone 3.
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <sys/fs.h>

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
    char buf[SYS_IPC_INLINE_BYTES];
    const fs_reply_t *rep;
    sys_handle_t dir, file;
    sys_msg_t reply;

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
    memcpy(buf, &reply.inline_data[sizeof(*rep)], rep->fr_length);

    /*
     * The close is what returns the server's per-connection record to its
     * pool. Section 25.2 reclaims it here and nowhere else, because
     * destroying the handle locally would tell the server nothing.
     */
    if (request(file, FS_OP_CLOSE, NULL, 0, 0, &reply) != SYS_OK) {
        return 4;
    }
    (void)sys_handle_close(file);

    return (int)((const fs_reply_t *)reply.inline_data)->fr_length;
}
