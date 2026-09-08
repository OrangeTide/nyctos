/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * A server that keeps a record per client, for M18.
 *
 * Every server in this system so far has been able to forget a client
 * the moment it answers one: `fs_server` holds no position, because a
 * connection carries none and every request states its own offset, and
 * `echoes` holds nothing at all. Sections 12.5 and 13.1 both argue from
 * a server that does hold something, and say that a death notice is
 * what keeps it from leaking. This is that server, built so that the
 * argument has something to be true or false about.
 *
 * The record is deliberately a slot out of a fixed table rather than
 * something notional. A leak that costs nothing is a leak nothing
 * notices, and what this milestone measures is how many slots are still
 * taken when every client that took one has gone.
 */

#ifndef _SYS_LEDGER_H_
#define _SYS_LEDGER_H_

#include <stdint.h>

/*
 * How many clients the server can hold records for at once. Small,
 * because the table filling up is a thing a check should be able to
 * reach rather than a theoretical end state.
 */
#define LEDGER_RECORDS      8u

/*
 * Take a record for the sender and answer. Taking twice from the same
 * client is the same record, so a client that asks again has not leaked
 * one: what leaks is a client that never comes back.
 *
 * The very first request a server gets is the one it never answers,
 * whatever it asked for. That is not a request code but an arrangement:
 * a client has to be left inside a rendezvous for the death section
 * 12.5 announces to be arrangeable at all, and the thread that holds it
 * may not receive again while it does.
 */
#define LEDGER_OP_TAKE      1u

/* Give the record back, which is what a client that leaves politely does. */
#define LEDGER_OP_DROP      3u

/*
 * How many records are held. Answering this does not take one, so a
 * check may ask without becoming another client to account for.
 */
#define LEDGER_OP_COUNT     4u

typedef struct ledger_request {
    uint32_t    lr_op;
    uint32_t    lr_reserved;
} ledger_request_t;

typedef struct ledger_reply {
    int32_t     lp_status;
    uint32_t    lp_held;        /* records held, after this request */
    uint32_t    lp_taken;       /* records ever taken, for COUNT */
    uint32_t    lp_notices;     /* death notices seen, for COUNT */
} ledger_reply_t;

#endif /* _SYS_LEDGER_H_ */
