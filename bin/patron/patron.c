/* patron.c : a ledger client that ends in one of several ways.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * The first argument names how it ends: hold, idle, leave, polite, or
 * giveup. The hold case dies inside the server's first rendezvous, idle
 * waits with nothing outstanding, leave returns without telling the server,
 * polite returns the record first, and giveup sends with a deadline to a
 * server that will not answer. The server's state afterwards is what a
 * check measures.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <string.h>
#include <sys/ledger.h>

/* Long enough to be unmistakably a wait, short enough to spend once. */
#define GIVEUP_MS   50u

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

    if (same(how, "giveup")) {
        /*
         * A send with a deadline, to a server that has the request and
         * will not answer. The client abandons work in
         * progress and the server finds out when its reply is refused,
         * so this death is not a death at all and the server hears
         * nothing about it: that is the case this mode is for.
         */
        sys_op_t send;
        struct sys_op_send_args args;
        ledger_request_t *req;

        memset(&args, 0, sizeof(args));
        args.request.handle = SYS_HANDLE_INVALID;
        args.request.inline_len = sizeof(*req);
        req = (ledger_request_t *)args.request.inline_data;
        req->lr_op = LEDGER_OP_TAKE;
        args.deadline = sys_deadline_in_ms(GIVEUP_MS);

        memset(&send, 0, sizeof(send));
        send.opcode = SYS_OP_IPC_SEND;
        send.target = to;
        send.flags = SYS_OP_DEADLINE;
        send.arg_ptr = (uintptr_t)&args;
        send.arg_len = sizeof(args);

        if (sys_op_submit(&send, 1) != SYS_ERR_TIMEOUT) {
            return 9;
        }

        return 0;
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
