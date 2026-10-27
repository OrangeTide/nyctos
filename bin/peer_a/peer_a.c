/* peer_a.c : a cohort member that reads and writes sent addresses.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * It waits on a channel for requests that carry an address and a value. For
 * each one it reads the word at that address, writes the value there, and
 * replies with the old contents. The address is used without validation,
 * which is safe only because the channel was given to peer_b alone. The
 * program loops forever and never returns.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <string.h>
#include <sys/peer.h>

int
main(void)
{
    sys_handle_t chan;
    sys_msg_t message, reply;
    sys_thread_id_t peer;
    uint64_t cookie;

    chan = ns_lookup(SYS_NS_SERVICE);
    if (chan == SYS_HANDLE_INVALID) {
        return 1;
    }

    for (;;) {
        const peer_request_t *req;
        peer_reply_t *rep;
        volatile uint32_t *at;

        if (sys_msg_recv(chan, &message, &peer, &cookie) != SYS_OK) {
            return 2;
        }

        req = (const peer_request_t *)message.inline_data;

        memset(&reply, 0, sizeof(reply));
        reply.handle = SYS_HANDLE_INVALID;
        reply.inline_len = sizeof(*rep);
        rep = (peer_reply_t *)reply.inline_data;

        /*
         * The peer's memory, by address, with nothing in between. A
         * volatile pointer because the peer wrote it and will read it
         * again, and this compiler has no reason to believe anything
         * else touches it.
         */
        at = (volatile uint32_t *)(uintptr_t)req->pr_addr;
        rep->pr_was = *at;
        *at = req->pr_value;
        rep->pr_status = SYS_OK;

        if (sys_msg_reply(peer, &reply) != SYS_OK) {
            return 3;
        }
    }
}
