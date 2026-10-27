/* fs_client.c : reads a file with a walk, a read and a close.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Each step is a separate request on the connection given to the program as
 * SYS_NS_ROOT, and the connection the walk returns is held until the close.
 * The program returns the number of bytes read as its exit status.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/fs.h>

static char buf[256];

static sys_err_t
request(sys_handle_t target, uint32_t op, const char *name, uint64_t off,
  uint32_t len, sys_msg_t *reply)
{
    fs_request_t req;
    fs_reply_t rep;
    struct wire_out out;
    struct wire_in in;
    sys_msg_t message;
    sys_err_t err;

    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;

    /*
     * The buffer travels with every request, because a reply larger than
     * the inline payload has nowhere else to go, and the server may not
     * hand back memory of its own.
     */
    message.buf = (uintptr_t)buf;
    message.buf_len = sizeof(buf);
    message.buf_flags = SYS_BUF_WRITE;

    memset(&req, 0, sizeof(req));
    req.fr_op = op;
    req.fr_offset = off;
    req.fr_length = len;
    if (name != NULL) {
        memcpy(req.fr_name, name, strlen(name) + 1);
    }
    wire_out_init(&out, message.inline_data, SYS_IPC_INLINE_BYTES);
    fs_request_put(&out, &req);
    message.inline_len = sizeof(fs_request_t);

    err = sys_msg_send(target, &message, reply);
    if (err != SYS_OK) {
        return err;
    }

    wire_in_init(&in, reply->inline_data, reply->inline_len);
    fs_reply_get(&in, &rep);
    if (!in.ok) {
        return SYS_ERR_TRUNCATED;
    }

    return rep.fr_status;
}

int
main(void)
{
    fs_reply_t rep;
    struct wire_in in;
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
    wire_in_init(&in, reply.inline_data, reply.inline_len);
    fs_reply_get(&in, &rep);
    got = rep.fr_length;

    size_t len = got < sizeof(buf) ? got : sizeof(buf);

    puts("fs_client read:");
    (void)write(STDOUT_FILENO, buf, len);

    /*
     * The close is what returns the server's per-connection record to its
     * pool. It is reclaimed here and nowhere else, because
     * destroying the handle locally would tell the server nothing.
     */
    if (request(file, FS_OP_CLOSE, NULL, 0, 0, &reply) != SYS_OK) {
        return 4;
    }
    (void)sys_handle_close(file);

    return (int)got;
}
