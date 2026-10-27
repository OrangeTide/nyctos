/* fs_chain.c : reads a file as one submission of chained entries.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * The walk, read and close entries go out together. The walk stores its
 * resulting handle in register zero, and the later entries take their target
 * from that register, so the program never holds the intermediate handle.
 * fs_client makes the same read as three separate calls.
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
    ops[0].flags = SYS_OP_RESULT_TO_REG | SYS_OP_LINK;
    ops[0].reg_out = 0;
    ops[0].arg_ptr = (uintptr_t)&walk;
    ops[0].arg_len = sizeof(walk);

    /* Read it, on whatever the walk came back with. */
    ops[1].opcode = SYS_OP_IPC_SEND;
    ops[1].flags = SYS_OP_TARGET_FROM_REG | SYS_OP_LINK;
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
    ops[2].flags = SYS_OP_TARGET_FROM_REG | SYS_OP_LINK;
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

/*
     * In the order they ran, so that a failing run names the entry
     * that failed first rather than a later one that failed because
     * of it.
     */
    rep = (const fs_reply_t *)walk.reply.inline_data;
    if (rep->fr_status != SYS_OK) {
        return 3;
    }

    rep = (const fs_reply_t *)fetch.reply.inline_data;
    if (rep->fr_status != SYS_OK) {
        return 4;
    }

    rep = (const fs_reply_t *)shut.reply.inline_data;
    if (rep->fr_status != SYS_OK) {
        return 5;
    }

    /*
     * From the read's reply and not from whichever was looked at
     * last, which is a mistake this made once and the byte count
     * caught at once.
     */
    got = ((const fs_reply_t *)fetch.reply.inline_data)->fr_length;

    len = got < sizeof(buf) ? got : sizeof(buf);

    puts("fs_chain read:");
    (void)write(STDOUT_FILENO, buf, len);

    return (int)got;
}
