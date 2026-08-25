/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * The other half of a cohort, and the one whose memory gets written.
 *
 * It holds a word, tells peer_a where that word is, and checks
 * afterwards that peer_a both read the old value and wrote the new one.
 * Two directions in one round trip, because a domain that only allowed
 * reads would be a different mechanism.
 *
 * The exit status is the report, in the manner of every other check
 * program here: zero is everything happening, and anything else names
 * the step it stopped at. There is no console handle in this
 * program's namespace and nothing to print to.
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <string.h>
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

    return 0;
}
