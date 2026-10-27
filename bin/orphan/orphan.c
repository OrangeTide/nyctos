/* orphan.c : a client of a server whose owner is terminating.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * It sends to a channel whose owning process is about to exit, and expects
 * the send to fail with an owner-dead error rather than get a reply. The
 * argument "ok" makes it expect a reply instead. It is a separate program
 * from wedged because at Tier 0 two live copies of one image cannot run
 * together.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <string.h>
#include <sys/fs.h>

/* Where the buffer goes, and the mark written after it comes home. */
static unsigned char lent[64];

#define MARK    0x5a

static int
same(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

int
main(int argc, char **argv)
{
    fs_request_t *req;
    sys_msg_t message, reply;
    sys_handle_t to;
    sys_err_t want;

    /*
     * Which answer this run is for. The same image serves both because
     * at Tier 0 two live copies of one image are one program written
     * over another, and these two never run at the same time.
     */
    want = (argc > 1 && same(argv[1], "ok")) ? SYS_OK : SYS_ERR_OWNER_DEAD;

    to = ns_lookup(SYS_NS_PRIVATE + 1);
    if (to == SYS_HANDLE_INVALID) {
        return 1;
    }

    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);

    /*
     * With a buffer, so that a server ending while it holds this has
     * to give the pages back as well as release the thread. A client
     * released into its own memory with a hole in it is worse than the
     * wait it was in.
     */
    message.buf = (uintptr_t)lent;
    message.buf_len = sizeof(lent);
    message.buf_flags = SYS_BUF_WRITE;

    req = (fs_request_t *)message.inline_data;
    req->fr_op = FS_OP_READ;

    if (sys_msg_send(to, &message, &reply) != want) {
        return 2;
    }

    /* Its own memory, the instant it is running again. */
    lent[0] = MARK;
    lent[sizeof(lent) - 1] = MARK;
    if (lent[0] != MARK || lent[sizeof(lent) - 1] != MARK) {
        return 3;
    }

    return 0;
}
