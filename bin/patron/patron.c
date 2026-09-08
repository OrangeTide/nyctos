/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * A client of the ledger, per M18 stage one.
 *
 * It takes a record and then stops existing in whichever of the ways
 * its argument names. What the milestone is measuring is what the
 * server is left holding afterwards, so the only thing this program has
 * to do carefully is take exactly one record and then be predictable
 * about how it goes.
 *
 *   hold    asks for a record and is not answered, so it dies inside
 *           the rendezvous section 12.5 announces a death from
 *   idle    takes a record, is answered, and then waits for something
 *           nobody will signal, so it dies with nothing outstanding
 *   leave   takes a record and returns, which is a client that ends
 *           without saying anything to the server
 *   polite  takes a record, gives it back, and returns
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <string.h>
#include <sys/ledger.h>

static int
same(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

static sys_err_t
ask(sys_handle_t to, uint32_t op, ledger_reply_t *out)
{
    sys_msg_t message, reply;
    ledger_request_t *req;
    sys_err_t err;

    memset(&message, 0, sizeof(message));
    memset(&reply, 0, sizeof(reply));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);
    req = (ledger_request_t *)message.inline_data;
    req->lr_op = op;

    err = sys_msg_send(to, &message, &reply);
    if (err == SYS_OK && out != NULL) {
        memcpy(out, reply.inline_data, sizeof(*out));
    }

    return err;
}

int
main(int argc, char **argv)
{
    sys_handle_t to;
    ledger_reply_t rep;
    const char *how;

    if (argc < 2) {
        return 1;
    }
    how = argv[1];

    to = ns_lookup(SYS_NS_PRIVATE + 1);
    if (to == SYS_HANDLE_INVALID) {
        return 2;
    }

    if (same(how, "hold")) {
        /*
         * Never returns on its own. The server takes the record and
         * does not answer, so this thread is REPLY-blocked until
         * something ends it, and what ends it is the check.
         */
        (void)ask(to, LEDGER_OP_HOLD, NULL);
        return 3;
    }

    if (ask(to, LEDGER_OP_TAKE, &rep) != SYS_OK) {
        return 4;
    }
    if (rep.lp_status != SYS_OK) {
        return 5;
    }

    if (same(how, "idle")) {
        sys_handle_t never = ns_lookup(SYS_NS_PRIVATE + 0);

        if (never == SYS_HANDLE_INVALID) {
            return 6;
        }
        (void)sys_fence_wait(never, 1, SYS_DEADLINE_NEVER);

        return 7;
    }

    if (same(how, "polite")) {
        if (ask(to, LEDGER_OP_DROP, &rep) != SYS_OK) {
            return 8;
        }
    }

    return 0;
}
