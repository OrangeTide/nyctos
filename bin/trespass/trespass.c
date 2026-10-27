/* trespass.c : asks the file server to read memory it does not own.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * It sends read requests whose buffers point at addresses outside its own
 * memory, and checks that each one is refused. A refusal by the server appears
 * in the reply status, and a refusal by the kernel is a failed send, and the
 * program tells the two apart. It exits with zero when both refusals happen,
 * and otherwise with the number of the step it stopped at.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>

#include <string.h>
#include <sys/fs.h>

static char buf[256];

/*
 * Ask for a file's bytes into a named address. Everything but the buffer
 * is what any client would send.
 */
/*
 * What the server said, as opposed to whether the message arrived. A
 * refusal by a server is a status in the reply; a refusal by the kernel
 * is the send failing. This program provokes both and has to tell them
 * apart.
 */
static sys_err_t
status_of(const sys_msg_t *reply)
{
    return ((const fs_reply_t *)reply->inline_data)->fr_status;
}

static sys_err_t
read_into(sys_handle_t target, uintptr_t addr, uint32_t len)
{
    fs_request_t *req;
    sys_msg_t message;
    sys_msg_t reply;

    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);
    message.buf = addr;
    message.buf_len = len;
    message.buf_flags = SYS_BUF_WRITE;

    req = (fs_request_t *)message.inline_data;
    req->fr_op = FS_OP_READ;
    req->fr_offset = 0;
    req->fr_length = len;

    return sys_msg_send(target, &message, &reply);
}

int
main(void)
{
    fs_request_t *req;
    sys_msg_t message;
    sys_msg_t reply;
    sys_handle_t dir, file;
    uintptr_t neighbour;
    sys_op_t op;

    dir = ns_lookup(SYS_NS_ROOT);
    if (dir == SYS_HANDLE_INVALID) {
        return 1;
    }

    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);
    req = (fs_request_t *)message.inline_data;
    req->fr_op = FS_OP_WALK;
    memcpy(req->fr_name, "motd", sizeof("motd"));
    if (sys_msg_send(dir, &message, &reply) != SYS_OK) {
        return 2;
    }
    file = reply.handle;

    /* This program's own memory, which is the ordinary case. */
    if (read_into(file, (uintptr_t)buf, sizeof(buf)) != SYS_OK) {
        return 3;
    }

    /*
     * The next slot up, which belongs to whatever the loader put there
     * and never to this program. One slot is the granularity the loader
     * works in, so an address a slot away is another program's memory
     * whichever slot this one landed in.
     */
    neighbour = ((uintptr_t)buf & ~((uintptr_t)SYS_USER_SLOT_BYTES - 1)) +
      SYS_USER_SLOT_BYTES;
    if (read_into(file, neighbour, sizeof(buf)) != SYS_ERR_FAULT) {
        return 4;
    }

    /* And an address that straddles the end of this program's slot. */
    if (read_into(file, (uintptr_t)buf, SYS_USER_SLOT_BYTES) !=
      SYS_ERR_FAULT) {
        return 5;
    }

    /*
     * A buffer that grants no access at all. The flags
     * say what the receiver may do, so this describes nothing, and there
     * is no mapping that means it: on one port it becomes a page nobody
     * may touch, and on the other the encoding for "no access" is the
     * encoding for "this is a page table". Before it was refused, this
     * message killed the file server, because the hardware walked into a
     * page whose contents this program had chosen.
     */
    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);
    message.buf = (uintptr_t)buf;
    message.buf_len = sizeof(buf);
    message.buf_flags = 0;
    req = (fs_request_t *)message.inline_data;
    req->fr_op = FS_OP_READ;
    req->fr_offset = 0;
    req->fr_length = sizeof(buf);
    if (sys_msg_send(file, &message, &reply) != SYS_ERR_INVALID_ARG) {
        return 13;
    }


    /*
     * A descriptor array that is not ours. This is
     * the first address the kernel dereferences on a program's word, so
     * it is the one that halts the machine when nothing checks it, and
     * that is what it did.
     */
    if (sys_op_submit((sys_op_t *)neighbour, 1) != SYS_ERR_FAULT) {
        return 6;
    }

    /*
     * And a descriptor of ours pointing at arguments that are not. The
     * kernel reads every operation's arguments through that pointer and
     * writes most results back through it.
     */
    memset(&op, 0, sizeof(op));
    op.opcode = SYS_OP_PROC_STATUS;
    op.target = SYS_HANDLE_INVALID;
    op.arg_ptr = neighbour;
    op.arg_len = 64;
    if (sys_op_submit(&op, 1) != SYS_ERR_FAULT || op.status != SYS_ERR_FAULT) {
        return 7;
    }

    /*
     * A batch whose second entry names a register that does not exist.
     *
     * Every entry is checked before any entry runs,
     * and which register a number names is a property of the entry
     * rather than of the execution, so this has to be caught before
     * the first entry does anything. The first entry's status is how
     * that is visible: an entry never considered is left as the caller
     * wrote it, so finding it untouched is finding
     * that the batch did not half-run.
     */
    {
        sys_op_t pair[2];

        memset(pair, 0, sizeof(pair));
        pair[0].opcode = SYS_OP_NOP;
        pair[0].status = SYS_ERR_INTERRUPTED;   /* nothing writes this */
        pair[1].opcode = SYS_OP_IPC_SEND;
        pair[1].flags = SYS_OP_TARGET_FROM_REG;
        pair[1].reg_in = SYS_BATCH_REGS;        /* one past the end */

        if (sys_op_submit(pair, 2) != SYS_ERR_INVALID_ARG) {
            return 20;
        }
        if (pair[1].status != SYS_ERR_INVALID_ARG) {
            return 21;
        }
        if (pair[0].status != SYS_ERR_INTERRUPTED) {
            return 22;
        }
    }



    /*
     * And a walk that climbs out of the subtree the connection stands
     * for, sent by hand rather than through the library.
     *
     * `..` is handled in two places for two reasons. The library
     * reduces it lexically before choosing a handle, so that a path
     * crossing a namespace boundary reaches the server that can answer
     * it; that is about correctness and not about confinement. The
     * server refuses it whatever the client did, and that is the
     * backstop, because a program is entitled to build its own messages
     * and this program just did.
     *
     * Refused rather than missing. A client told a name does not exist
     * goes looking for it elsewhere; a client told it may not ask has
     * been told the truth.
     */
    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);
    req = (fs_request_t *)message.inline_data;
    req->fr_op = FS_OP_WALK;
    memcpy(req->fr_name, "..", sizeof(".."));
    /*
     * The send reports what the server said, so
     * a refusal arrives as the return value and not only in the
     * payload. Both are checked: they are the same answer in two
     * places and a protocol that let them differ would be lying to
     * one of its two readers.
     */
    if (sys_msg_send(dir, &message, &reply) != SYS_ERR_PERM_DENIED ||
      status_of(&reply) != SYS_ERR_PERM_DENIED) {
        return 14;
    }

    /* And with something after it, which is the shape that would climb. */
    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);
    req = (fs_request_t *)message.inline_data;
    req->fr_op = FS_OP_WALK;
    memcpy(req->fr_name, "../motd", sizeof("../motd"));
    if (sys_msg_send(dir, &message, &reply) != SYS_ERR_PERM_DENIED ||
      status_of(&reply) != SYS_ERR_PERM_DENIED) {
        return 15;
    }

    /*
     * A dot-dot in the middle, which never leaves the subtree and is
     * refused anyway. That is this server being stricter than the
     * rule rather than the rule: it has one level and nothing to
     * walk, so refusing every `..` costs it nothing. A server with
     * real directories would resolve this and check where it landed.
     */
    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);
    req = (fs_request_t *)message.inline_data;
    req->fr_op = FS_OP_WALK;
    memcpy(req->fr_name, "a/../b", sizeof("a/../b"));
    if (sys_msg_send(dir, &message, &reply) != SYS_ERR_PERM_DENIED ||
      status_of(&reply) != SYS_ERR_PERM_DENIED) {
        return 16;
    }

    /*
     * And one that only looks like it. A name beginning with two dots is
     * not a `..` component, and refusing it would be refusing a file
     * somebody is entitled to have called that.
     */
    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);
    req = (fs_request_t *)message.inline_data;
    req->fr_op = FS_OP_WALK;
    memcpy(req->fr_name, "..quiet", sizeof("..quiet"));
    if (sys_msg_send(dir, &message, &reply) != SYS_ERR_NO_ENT ||
      status_of(&reply) != SYS_ERR_NO_ENT) {
        return 17;
    }


    /*
     * And the same name on a connection that is a file rather than a
     * directory. A file has no subtree to climb out of, so the true
     * answer is that this connection does not name a place things can be
     * found in, and not that the client may not ask. Two refusals that
     * would otherwise be one.
     */
    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);
    req = (fs_request_t *)message.inline_data;
    req->fr_op = FS_OP_WALK;
    memcpy(req->fr_name, "..", sizeof(".."));
    if (sys_msg_send(file, &message, &reply) != SYS_ERR_NO_ENT ||
      status_of(&reply) != SYS_ERR_NO_ENT) {
        return 18;
    }

    return 0;
}
