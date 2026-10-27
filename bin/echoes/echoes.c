/* echoes.c : a server that answers every request with success.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * It replies to each request with a success status and no data, and it keeps
 * no state. Given the argument holds, it receives one request and never
 * replies, and given deaf, it never receives, so clients queue behind it.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <string.h>
#include <sys/fs.h>

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
    sys_handle_t chan;
    sys_msg_t message, reply;
    sys_thread_id_t peer;
    uint64_t cookie;

    chan = ns_lookup(SYS_NS_SERVICE);
    if (chan == SYS_HANDLE_INVALID) {
        return 1;
    }

    /*
     * Deaf: holds the channel and never receives on it, so a client
     * queues rather than being answered. That is what a server looks
     * like from outside when it has wedged, and it is the state a
     * client has to be in for its release to be worth watching.
     */
    /*
     * Holds: receives one request and never answers it, so the client
     * is reply-blocked with its buffer here rather than queued with
     * its buffer still its own. That is the state a kill has to unwind
     * as well as release.
     */
    if (argc > 1 && same(argv[1], "holds")) {
        if (sys_msg_recv(chan, &message, &peer, &cookie) != SYS_OK) {
            return 6;
        }
        for (;;) {
            sys_handle_t never = ns_lookup(SYS_NS_PRIVATE + 0);

            if (never == SYS_HANDLE_INVALID) {
                return 4;
            }
            (void)sys_fence_wait(never, 1, SYS_DEADLINE_NEVER);
        }
    }

    if (argc > 1 && same(argv[1], "deaf")) {
        sys_handle_t never = ns_lookup(SYS_NS_PRIVATE + 0);

        if (never == SYS_HANDLE_INVALID) {
            return 4;
        }
        (void)sys_fence_wait(never, 1, SYS_DEADLINE_NEVER);

        return 5;
    }

    for (;;) {
        fs_reply_t *rep;

        if (sys_msg_recv(chan, &message, &peer, &cookie) != SYS_OK) {
            return 2;
        }

        /* A pulse is not a request, and this one has nobody to reply to. */
        if ((message.flags & SYS_MSG_IS_PULSE) != 0) {
            continue;
        }

        memset(&reply, 0, sizeof(reply));
        reply.handle = SYS_HANDLE_INVALID;
        reply.inline_len = sizeof(*rep);
        rep = (fs_reply_t *)reply.inline_data;
        rep->fr_status = SYS_OK;
        rep->fr_length = 0;

        if (sys_msg_reply(peer, &reply) != SYS_OK) {
            return 3;
        }
    }
}
