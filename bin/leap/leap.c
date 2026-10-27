/* leap.c : jumps into writable memory, which must not be executable.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * With no argument it jumps into writable program memory. With an argument
 * it sends a buffer to a server, and when the buffer comes back it jumps into
 * that buffer. The bytes at the jump target are never meant to be a valid
 * instruction.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/fs.h>

static char buf[256];

static sys_err_t
request(sys_handle_t target, uint32_t op, const char *name, uint32_t len,
  sys_msg_t *reply)
{
    fs_request_t *req;
    sys_msg_t message;
    sys_err_t err;

    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);
    message.buf = (uintptr_t)buf;
    message.buf_len = sizeof(buf);
    message.buf_flags = SYS_BUF_WRITE;

    req = (fs_request_t *)message.inline_data;
    req->fr_op = op;
    req->fr_offset = 0;
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

/*
 * Read a file into buf, so that buf has been out to a server and back.
 * What is in it afterwards does not matter; that it travelled does.
 */
static int
lend(void)
{
    sys_handle_t dir, file;
    sys_msg_t reply;

    dir = ns_lookup(SYS_NS_ROOT);
    if (dir == SYS_HANDLE_INVALID) {
        return 1;
    }
    if (request(dir, FS_OP_WALK, "motd", 0, &reply) != SYS_OK) {
        return 2;
    }
    file = reply.handle;
    if (request(file, FS_OP_READ, NULL, sizeof(buf), &reply) != SYS_OK) {
        return 3;
    }
    if (request(file, FS_OP_CLOSE, NULL, 0, &reply) != SYS_OK) {
        return 4;
    }
    (void)sys_handle_close(file);

    return 0;
}

int
main(int argc, char **argv)
{
    volatile uint32_t code[8];
    void (*fn)(void);
    uintptr_t at;
    uint32_t i;

    if (argc == 2 && argv[1] != NULL && argv[1][0] == 'l') {
        int err = lend();

        if (err != 0) {
            return err;
        }
        at = (uintptr_t)(void *)buf;
    } else {
        for (i = 0; i < 8; i++) {
            code[i] = 0;
        }
        at = (uintptr_t)(void *)code;
    }

    printf("leap: jumping to 0x%x\n", (unsigned int)at);

    /*
     * Through an integer, because a pointer to an object and a pointer
     * to a function are different kinds of thing and a compiler is
     * entitled to say so. The whole point of this program is to make
     * the hardware answer a question the language does not have.
     */
    fn = (void (*)(void))at;
    fn();

    /*
     * Reaching here means the fetch was permitted and whatever was
     * there returned, which is the failure either way.
     */
    return 3;
}
