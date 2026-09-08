/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * A server that keeps a record per client, per M18 stage one.
 *
 * It exists to have something to lose. Sections 12.5 and 13.1 both
 * reason from a server that allocates on a client's behalf and would
 * leak that allocation if it never learned the client had gone, and no
 * server in this system had anything to allocate. This one takes a slot
 * out of a fixed table for each client that asks and holds it until the
 * client gives it back.
 *
 * At this stage it gives a record back only when asked. A death notice
 * arrives on the same channel as every request, and this counts it and
 * does nothing else with it, which is what every other server here does
 * with a pulse and is the state the milestone measures from.
 *
 * Records are keyed by the identifier the server has, which is the
 * thread that sent the request. Whether that is the right key is stage
 * three's question and is deliberately not answered here.
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <string.h>
#include <sys/ledger.h>

struct record {
    sys_thread_id_t     owner;
    int                 taken;
};

static struct record records[LEDGER_RECORDS];

/* Records ever taken, and death notices ever seen, both for COUNT. */
static uint32_t taken_ever;
static uint32_t notices;

static uint32_t
held(void)
{
    uint32_t n = 0;
    uint32_t i;

    for (i = 0; i < LEDGER_RECORDS; i++) {
        if (records[i].taken) {
            n++;
        }
    }

    return n;
}

/*
 * The record this client already has, or a new one. A client that asks
 * twice has one record and not two: what this milestone is about is a
 * client that never comes back, not a client that comes back often.
 */
static struct record *
record_for(sys_thread_id_t who)
{
    uint32_t i;

    for (i = 0; i < LEDGER_RECORDS; i++) {
        if (records[i].taken && records[i].owner == who) {
            return &records[i];
        }
    }

    for (i = 0; i < LEDGER_RECORDS; i++) {
        if (!records[i].taken) {
            records[i].taken = 1;
            records[i].owner = who;
            taken_ever++;
            return &records[i];
        }
    }

    return NULL;
}

int
main(int argc, char **argv)
{
    sys_handle_t chan;
    sys_msg_t message, reply;
    sys_thread_id_t peer;
    uint64_t cookie;

    (void)argc;
    (void)argv;

    chan = ns_lookup(SYS_NS_SERVICE);
    if (chan == SYS_HANDLE_INVALID) {
        return 1;
    }

    for (;;) {
        const ledger_request_t *req;
        ledger_reply_t *rep;
        struct record *r;

        memset(&message, 0, sizeof(message));
        if (sys_msg_recv(chan, &message, &peer, &cookie) != SYS_OK) {
            return 2;
        }

        /*
         * A pulse is not a request. Counted and otherwise ignored,
         * which is the whole of what a server here has ever done with
         * one and is the behaviour this stage is measuring.
         */
        if ((message.flags & SYS_MSG_IS_PULSE) != 0) {
            const sys_pulse_t *p = (const sys_pulse_t *)message.inline_data;

            if (p->code == SYS_PULSE_DEAD_OWNER) {
                notices++;
            }
            continue;
        }

        req = (const ledger_request_t *)message.inline_data;

        memset(&reply, 0, sizeof(reply));
        reply.handle = SYS_HANDLE_INVALID;
        reply.inline_len = sizeof(*rep);
        rep = (ledger_reply_t *)reply.inline_data;
        rep->lp_status = SYS_OK;

        switch (req->lr_op) {
        case LEDGER_OP_HOLD:
            /*
             * The record is taken and the client is not answered. It
             * stays inside the rendezvous, which is the state section
             * 12.5 announces a death from, and this loop goes back to
             * receiving rather than waiting on it.
             */
            if (record_for(peer) == NULL) {
                rep->lp_status = SYS_ERR_NO_MEMORY;
                break;
            }
            continue;

        case LEDGER_OP_TAKE:
            if (record_for(peer) == NULL) {
                rep->lp_status = SYS_ERR_NO_MEMORY;
            }
            break;

        case LEDGER_OP_DROP:
            r = record_for(peer);
            if (r != NULL) {
                r->taken = 0;
                r->owner = 0;
            }
            break;

        case LEDGER_OP_COUNT:
            break;

        default:
            rep->lp_status = SYS_ERR_INVALID_ARG;
            break;
        }

        rep->lp_held = held();
        rep->lp_taken = taken_ever;
        rep->lp_notices = notices;

        if (sys_msg_reply(peer, &reply) != SYS_OK) {
            return 3;
        }
    }
}
