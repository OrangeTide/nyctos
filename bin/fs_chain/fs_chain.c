/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * The same file read as fs_client, written as one submission.
 *
 * fs_client walks, reads and closes as three calls, and holds the
 * connection the walk returned in a local variable between them. That
 * variable is the dependency: the second request cannot be written down
 * until the first has answered, which is why it had to be a second call.
 *
 * Section 17.4's registers are where that variable goes instead. The
 * walk says to put the handle its reply moved into register zero, and
 * the three entries after it say to take their target from register
 * zero. Nothing here names a handle the program does not already hold,
 * and the program never learns the value of the one it is passing
 * along.
 *
 * What this is for is the comparison. fs_client makes the same three
 * round trips this does and pays four crossings of the trap edge for
 * them; fs_batch makes one round trip by asking the server a compound
 * question, which is section 25.7 and a different mechanism entirely.
 * Set beside those two, this one separates what batching saves from
 * what the protocol saves.
 *
 * The last entry closes the local handle, which the program could not
 * do for itself: the handle exists in its table and its value was never
 * anywhere the program could read. Section 17.5 names that as a limit
 * when a chain aborts. It is the same limit when a chain succeeds, and
 * an entry that takes its target from a register is the answer to it.
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/fs.h>

static char buf[256];

static sys_msg_t walk_reply, fetch_reply, close_reply;
static struct sys_op_send_args walk, fetch, shut;

static void
request(struct sys_op_send_args *a, uint32_t op, const char *name,
  uint32_t len)
{
    fs_request_t *req;

    memset(a, 0, sizeof(*a));
    a->request.handle = SYS_HANDLE_INVALID;
    a->request.inline_len = sizeof(*req);

    /*
     * The buffer travels with every request, because a reply larger
     * than the inline payload has nowhere else to go: section 6.1
     * forbids the server handing back memory of its own.
     */
    a->request.buf = (uintptr_t)buf;
    a->request.buf_len = sizeof(buf);
    a->request.buf_flags = SYS_BUF_WRITE;

    req = (fs_request_t *)a->request.inline_data;
    req->fr_op = op;
    req->fr_offset = 0;
    req->fr_length = len;
    if (name != NULL) {
        memcpy(req->fr_name, name, strlen(name) + 1);
    }
}

int
main(void)
{
    const fs_reply_t *rep;
    sys_op_t ops[4];
    sys_handle_t dir;
    uint32_t got;
    size_t len;

    dir = ns_lookup(SYS_NS_ROOT);
    if (dir == SYS_HANDLE_INVALID) {
        return 1;
    }

    request(&walk, FS_OP_WALK, "motd", 0);
    request(&fetch, FS_OP_READ, NULL, sizeof(buf));
    request(&shut, FS_OP_CLOSE, NULL, 0);

    memset(ops, 0, sizeof(ops));

    /* Walk, and keep what the reply moved. */
    ops[0].opcode = SYS_OP_IPC_SEND;
    ops[0].target = dir;
    ops[0].flags = SYS_OP_RESULT_TO_REG;
    ops[0].reg_out = 0;
    ops[0].arg_ptr = (uintptr_t)&walk;
    ops[0].arg_len = sizeof(walk);

    /* Read it, on whatever the walk came back with. */
    ops[1].opcode = SYS_OP_IPC_SEND;
    ops[1].flags = SYS_OP_TARGET_FROM_REG;
    ops[1].reg_in = 0;
    ops[1].arg_ptr = (uintptr_t)&fetch;
    ops[1].arg_len = sizeof(fetch);

    /*
     * Tell the server it is finished with, which is what returns the
     * per-connection record to its pool. Section 25.2 reclaims it here
     * and nowhere else, because destroying the handle locally would
     * tell the server nothing.
     */
    ops[2].opcode = SYS_OP_IPC_SEND;
    ops[2].flags = SYS_OP_TARGET_FROM_REG;
    ops[2].reg_in = 0;
    ops[2].arg_ptr = (uintptr_t)&shut;
    ops[2].arg_len = sizeof(shut);

    /* And give the handle itself back, which needs no name for it. */
    ops[3].opcode = SYS_OP_HANDLE_CLOSE;
    ops[3].flags = SYS_OP_TARGET_FROM_REG;
    ops[3].reg_in = 0;

    if (sys_op_submit(ops, 4) != SYS_OK) {
        return 2;
    }

    walk_reply = walk.reply;
    fetch_reply = fetch.reply;
    close_reply = shut.reply;

    rep = (const fs_reply_t *)walk_reply.inline_data;
    if (rep->fr_status != SYS_OK) {
        return 3;
    }
    rep = (const fs_reply_t *)close_reply.inline_data;
    if (rep->fr_status != SYS_OK) {
        return 5;
    }

    rep = (const fs_reply_t *)fetch_reply.inline_data;
    if (rep->fr_status != SYS_OK) {
        return 4;
    }
    got = rep->fr_length;

    len = got < sizeof(buf) ? got : sizeof(buf);

    puts("fs_chain read:");
    (void)write(STDOUT_FILENO, buf, len);

    return (int)got;
}
