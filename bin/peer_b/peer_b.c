/* peer_b.c : the cohort member whose word peer_a writes.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * It holds a word in its data and sends peer_a the word's address with a new
 * value. It then checks that peer_a reported the old value and that the word
 * now holds the new one. The exit status is zero when every step holds, and
 * otherwise the number of the step that failed. It has no console handle and
 * prints nothing.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <string.h>
#include <sys/fs.h>
#include <sys/peer.h>

#define WAS     0xa5a5a5a5u
#define NOW     0x5a5a5a5au

/*
 * Not on the stack. A word in this program's data is at an address that
 * is the same on every run, which makes a failure here readable, and it
 * keeps the check away from the one part of a slot whose contents the
 * loader does not control.
 */
static volatile uint32_t word = WAS;

int
main(void)
{
    peer_request_t *req;
    const peer_reply_t *rep;
    sys_msg_t message, reply;
    sys_handle_t to;

    to = ns_lookup(SYS_NS_ROOT);
    if (to == SYS_HANDLE_INVALID) {
        return 1;
    }

    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);

    /*
     * No buffer. Section 6.1's descriptor is what a message carries when
     * the two ends are in different address spaces, and the point of a
     * cohort is that these two are not.
     */
    req = (peer_request_t *)message.inline_data;
    req->pr_addr = (uint64_t)(uintptr_t)&word;
    req->pr_value = NOW;

    if (sys_msg_send(to, &message, &reply) != SYS_OK) {
        return 2;
    }

    rep = (const peer_reply_t *)reply.inline_data;
    if (rep->pr_status != SYS_OK) {
        return 3;
    }

    /* What the peer read out of this program's memory. */
    if (rep->pr_was != WAS) {
        return 4;
    }

    /* And what it wrote into it. */
    if (word != NOW) {
        return 5;
    }

    /*
     * And the other half of the rule, which is the half that matters.
     * A member may describe the domain's memory because the domain can
     * already reach it. Describing it to somebody outside the domain
     * would be handing them a page nobody gave them, and that is
     * refused.
     *
     * The address is the slot below this program's, which is peer_a's:
     * memory this program may read directly and may not lend out. The
     * file server is in a different cohort, so it is an outsider here
     * even though it is a member of one.
     */
    {
        sys_handle_t out = ns_lookup(PEER_NS_OUTSIDER);
        fs_request_t *req;
        uintptr_t inside;

        if (out == SYS_HANDLE_INVALID) {
            return 6;
        }

        inside = ((uintptr_t)&word & ~((uintptr_t)SYS_USER_SLOT_BYTES - 1)) -
          SYS_USER_SLOT_BYTES;

        memset(&message, 0, sizeof(message));
        message.handle = SYS_HANDLE_INVALID;
        message.inline_len = sizeof(*req);
        message.buf = inside;
        message.buf_len = 64;
        message.buf_flags = SYS_BUF_WRITE;
        req = (fs_request_t *)message.inline_data;
        req->fr_op = FS_OP_READ;
        req->fr_offset = 0;
        req->fr_length = 64;

        if (sys_msg_send(out, &message, &reply) != SYS_ERR_FAULT) {
            return 7;
        }
    }

    return 0;
}
