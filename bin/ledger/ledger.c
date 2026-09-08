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
 *
 * **Two threads, because one of them stops.** Section 19.3 has a server
 * thread serve one request at a time, and this server needs one request
 * left unanswered for as long as its client lives: that is the state
 * section 12.5 announces a death from, and it is the only way to
 * arrange a client that dies inside a rendezvous. So the first thread
 * takes that request, starts the second, and stops. Everything after
 * the first request is the second thread's, including the death notice
 * when the first thread's client is killed.
 *
 * The first version of this had one thread receive again while it still
 * owed a reply, and the kernel let it: the second rendezvous overwrote
 * the client it owed and pushed it onto the channel's serving list a
 * second time, which left that list naming a thread serving nothing.
 * The review measured that and this server stopped doing it. What the
 * kernel should do about a server that tries is written down in the
 * milestone and is not settled here.
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

/* The channel, read by both threads and written before the second exists. */
static sys_handle_t chan;

/*
 * The serving thread's stack. Its own memory, because a thread's stack
 * is memory its creator already has: there is no allocator here.
 */
#define STACK_BYTES     4096
static unsigned char serve_stack[STACK_BYTES] __attribute__((aligned(16)));

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

/*
 * The request loop, which is every request after the first one. It owes
 * nobody anything across a receive: each request is answered before the
 * next is taken, which is the shape section 19.3 asks for.
 */
static void
serve(void *arg)
{
    sys_msg_t message, reply;
    sys_thread_id_t peer;
    uint64_t cookie;

    (void)arg;

    for (;;) {
        const ledger_request_t *req;
        ledger_reply_t *rep;
        struct record *r;

        memset(&message, 0, sizeof(message));
        if (sys_msg_recv(chan, &message, &peer, &cookie) != SYS_OK) {
            sys_thread_exit(2);
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

        memset(&reply, 0, sizeof(reply));
        reply.handle = SYS_HANDLE_INVALID;
        reply.inline_len = sizeof(*rep);
        rep = (ledger_reply_t *)reply.inline_data;
        rep->lp_status = SYS_OK;

        /*
         * A request shorter than the header is refused rather than read
         * past. The reply still goes back, because a client that sent
         * nonsense is still waiting for an answer.
         */
        if (message.inline_len < sizeof(*req)) {
            rep->lp_status = SYS_ERR_INVALID_ARG;
            req = NULL;
        } else {
            req = (const ledger_request_t *)message.inline_data;
        }

        if (req != NULL) {
            switch (req->lr_op) {
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
        }

        rep->lp_held = held();
        rep->lp_taken = taken_ever;
        rep->lp_notices = notices;

        if (sys_msg_reply(peer, &reply) != SYS_OK) {
            sys_thread_exit(3);
        }
    }
}

int
main(int argc, char **argv)
{
    sys_msg_t message;
    sys_thread_id_t peer;
    sys_handle_t never, th;
    uint64_t cookie;

    (void)argc;
    (void)argv;

    chan = ns_lookup(SYS_NS_SERVICE);
    never = ns_lookup(SYS_NS_PRIVATE + 0);
    if (chan == SYS_HANDLE_INVALID || never == SYS_HANDLE_INVALID) {
        return 1;
    }

    /*
     * The one request that is never answered. It has to be the first,
     * because this thread is the only receiver until it starts the
     * other one, and that is what makes which request it is a fact
     * rather than a race.
     */
    memset(&message, 0, sizeof(message));
    if (sys_msg_recv(chan, &message, &peer, &cookie) != SYS_OK) {
        return 2;
    }
    if ((message.flags & SYS_MSG_IS_PULSE) != 0) {
        return 3;
    }
    if (record_for(peer) == NULL) {
        return 4;
    }

    if (sys_thread_create(serve, NULL, serve_stack, STACK_BYTES,
      SYS_PRIO_INHERIT, &th) != SYS_OK) {
        return 5;
    }
    if (sys_handle_close(th) != SYS_OK) {
        return 7;
    }

    /*
     * And this thread stops here, still owing that reply. It may not
     * receive again while it does, and it has nothing else to do: the
     * client it is holding is about to be killed, and what the check
     * measures is what the record does afterwards.
     */
    (void)sys_fence_wait(never, 1, SYS_DEADLINE_NEVER);

    return 6;
}
