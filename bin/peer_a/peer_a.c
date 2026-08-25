/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * One half of a cohort, and the half that does the dereferencing.
 *
 * It is handed a receive handle as SYS_NS_SERVICE and waits. Each
 * message carries an address in the sender's own slot and a value. This
 * program reads that address, writes the value to it, and replies with
 * what was there before. Nothing in the message is a buffer descriptor,
 * so the kernel maps nothing and validates nothing: the address is
 * dereferenced exactly as it arrived.
 *
 * That is the whole of what a shared protection domain buys, stated as
 * small as it can be stated.
 *
 * It is also the whole of what makes such a server dangerous, and the
 * danger is not the cohort's doing. Nothing in a message says whether
 * the sender is a member, so a program that dereferences an address
 * because it arrived is trusting whoever holds a send handle rather
 * than trusting its peers. This one is safe because the only
 * connection to its channel was minted for peer_b and moved there. A
 * member that served the same channel to outsiders would be handing
 * them the whole cohort, and section 18 has nothing to say about that
 * because it is a question about who holds handles. At Tier 2 this works because peer_b's slot
 * is mapped in the address space this program runs in, which is true
 * because the build says the two are one cohort. Give the same message
 * to a program outside the cohort and it faults, which is what stage
 * one checks by hand.
 *
 * It never exits. A server that returned would be an exit nobody asked
 * about, which halts the machine, and there is nothing here for a
 * supervisor to do until section 18.2 is built in M7.
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
