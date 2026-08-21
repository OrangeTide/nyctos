/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Print a file, named relative to the directory this process was given.
 *
 * There is no path here and no root to start one from. The shell handed
 * this program a connection to a directory, per section 20.4, and the name
 * on the command line is resolved against that and against nothing else,
 * which is what section 20.3 means by naming being handle-relative. A
 * program started without a directory can name no file at all, and says so
 * rather than falling back to something ambient.
 *
 * It reads in a loop with the offset in the request, per section 25.4, so
 * a file larger than one reply arrives in pieces without the connection
 * carrying a position.
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/fs.h>
#include <unistd.h>

static sys_err_t
request(sys_handle_t target, uint32_t op, const char *name, uint64_t off,
  uint32_t len, sys_msg_t *reply)
{
    sys_msg_t message;
    fs_request_t *req;
    sys_err_t err;

    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);

    req = (fs_request_t *)message.inline_data;
    req->fr_op = op;
    req->fr_offset = off;
    req->fr_length = len;
    if (name != NULL) {
        uint32_t n = (uint32_t)strlen(name);

        if (n >= FS_NAME_MAX) {
            return SYS_ERR_INVALID_ARG;
        }
        memcpy(req->fr_name, name, n + 1);
    }

    err = sys_msg_send(target, &message, reply);
    if (err != SYS_OK) {
        return err;
    }

    return ((const fs_reply_t *)reply->inline_data)->fr_status;
}

int
main(int argc, char **argv)
{
    const fs_reply_t *rep;
    sys_handle_t dir, file;
    sys_msg_t reply;
    uint64_t off = 0;
    int total = 0;

    if (argc < 2) {
        printf("usage: cat name\n");
        return 1;
    }

    dir = ns_lookup(SYS_NS_ROOT);
    if (dir == SYS_HANDLE_INVALID) {
        printf("cat: no directory to name %s in\n", argv[1]);
        return 2;
    }

    if (request(dir, FS_OP_WALK, argv[1], 0, 0, &reply) != SYS_OK) {
        printf("cat: %s: no such file\n", argv[1]);
        return 3;
    }
    file = reply.handle;

    for (;;) {
        uint32_t got;

        if (request(file, FS_OP_READ, NULL, off,
          SYS_IPC_INLINE_BYTES - sizeof(fs_reply_t), &reply) != SYS_OK) {
            printf("cat: %s: read failed\n", argv[1]);
            return 4;
        }
        rep = (const fs_reply_t *)reply.inline_data;
        got = rep->fr_length;
        if (got == 0) {
            break;
        }

        (void)write(STDOUT_FILENO, &reply.inline_data[sizeof(*rep)], got);
        off += got;
        total += (int)got;
    }

    /*
     * The close is what returns the server's per-connection record to its
     * pool. Section 25.2 reclaims it here and nowhere else, because
     * destroying the handle locally would tell the server nothing.
     */
    (void)request(file, FS_OP_CLOSE, NULL, 0, 0, &reply);
    (void)sys_handle_close(file);

    /*
     * Zero, because a status says whether it worked. The byte count is
     * the output's business and the output already carried it.
     */
    (void)total;

    return 0;
}
