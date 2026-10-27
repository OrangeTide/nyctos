/* ledger.h : ledger server protocol records and operation codes.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares the request and reply structures and the LEDGER_OP_* codes of the
 * ledger server, which keeps one record per client in a table of
 * LEDGER_RECORDS slots. Included by bin/ledger/ledger.c, bin/patron/patron.c
 * and sys/kern/init_main.c.
 *
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
 */
#define LEDGER_OP_TAKE      1u

/*
 * Take a record and do not answer, per M18 stage three. The thread that
 * takes this request keeps it: it starts a replacement to go on serving
 * and then stops, still owing the reply, which is how a client is left
 * inside the rendezvous section 12.5 announces a death from.
 *
 * A server thread may not receive again while it owes a reply, per
 * section 19.3, so holding a request costs a thread. That is the price
 * of the arrangement and it is why the number of them is bounded.
 */
#define LEDGER_OP_HOLD      2u

/* Give the record back, which is what a client that leaves politely does. */
#define LEDGER_OP_DROP      3u

/*
 * How many records are held. Answering this does not take one, so a
 * check may ask without becoming another client to account for.
 */
#define LEDGER_OP_COUNT     4u

/*
 * Whether a record is held for the client named by the tag, which is
 * `SYS_ID_TAG32` of its thread id: the same 32 bits a death notice
 * carries, so a check can ask the server the question the notice makes
 * it answer. SYS_OK if it is held, SYS_ERR_NO_ENT if it is not.
 */
#define LEDGER_OP_HELD_FOR  5u

typedef struct ledger_request {
    uint32_t    lr_op;
    uint32_t    lr_tag;         /* the client asked about, for HELD_FOR */
} ledger_request_t;

typedef struct ledger_reply {
    int32_t     lp_status;
    uint32_t    lp_held;        /* records held, after this request */
    uint32_t    lp_taken;       /* records ever taken, for COUNT */
    uint32_t    lp_notices;     /* death notices seen, for COUNT */
    uint32_t    lp_released;    /* records a notice released, for COUNT */

    /*
     * What collecting found, per M18 stage four. `lp_reaped` counts
     * connections whose clients had all gone and this server
     * collected; `lp_drained` counts the records it let go because one
     * was collected; `lp_hints` counts the pulses that said there was
     * something to collect, which this server counts and does not act
     * on.
     */
    uint32_t    lp_reaped;
    uint32_t    lp_drained;
    uint32_t    lp_hints;

    /*
     * Receives that came with SYS_MSG_PULSE_LOST, per M18 stage five.
     * A server that acts on what it is told would have to care; this
     * one collects before every answer and does not.
     */
    uint32_t    lp_losses;
} ledger_reply_t;

#endif /* _SYS_LEDGER_H_ */
