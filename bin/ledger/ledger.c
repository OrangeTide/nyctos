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

/* Records released because a notice named their client. */
static uint32_t released;

/*
 * Release the record belonging to the client the notice names, per M18
 * stage three.
 *
 * The notice carries `SYS_ID_TAG32` of the dead thread, which is the
 * index and twelve bits of generation, per section 10.5. That is what
 * this server has to match on, so it keeps the same 32 bits beside each
 * record rather than the whole identifier: matching on what arrives is
 * the point, and a server that stored something else would be answering
 * a question it was not asked.
 *
 * Exactly one record, and only if it is that client's. Releasing
 * whichever record was found first would pass a check that counted and
 * fail every client whose record it took.
 */
static void
release_for(uint32_t tag)
{
    uint32_t i;

    for (i = 0; i < LEDGER_RECORDS; i++) {
        if (records[i].taken && SYS_ID_TAG32(records[i].owner) == tag) {
            records[i].taken = 0;
            records[i].owner = 0;
            released++;
            return;
        }
    }
}

/* The channel, read by both threads and written before the second exists. */
static sys_handle_t chan;

/*
 * A stack for each serving thread. Its own memory, because a thread's
 * stack is memory its creator already has: there is no allocator here.
 *
 * One per hold, plus the one the first replacement runs on. A held
 * request costs a thread, because the thread holding it may not receive
 * again, so this array is the bound on how many requests this server
 * can be holding at once.
 */
#define STACK_BYTES     4096
#define SERVE_STACKS    4u
static unsigned char serve_stack[SERVE_STACKS][STACK_BYTES]
  __attribute__((aligned(16)));
static uint32_t serve_next;

/* The timeline a holding thread stops on, which nobody signals. */
static sys_handle_t hold_never;

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
static void serve(void *arg);

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
                release_for(p->value);
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

            case LEDGER_OP_HOLD: {
                sys_handle_t th;
                uint32_t mine;

                /*
                 * The record is taken and this client is not answered.
                 * This thread cannot receive again while it owes a
                 * reply, per section 19.3, so it starts a replacement
                 * and stops: a held request costs a thread, and the
                 * stack array is the bound on how many there can be.
                 *
                 * The stack is claimed before the replacement starts,
                 * because the replacement runs at this thread's own
                 * priority and could take a hold of its own: an index
                 * incremented afterwards is an index the replacement
                 * can read before it changes, and two threads on one
                 * stack.
                 */
                if (serve_next >= SERVE_STACKS) {
                    rep->lp_status = SYS_ERR_NO_MEMORY;
                    break;
                }
                mine = serve_next++;

                if (sys_thread_create(serve, NULL, serve_stack[mine],
                  STACK_BYTES, SYS_PRIO_INHERIT, &th) != SYS_OK) {
                    serve_next--;
                    rep->lp_status = SYS_ERR_NO_MEMORY;
                    break;
                }
                if (sys_handle_close(th) != SYS_OK) {
                    sys_thread_exit(4);
                }

                /*
                 * And the record last, so that a client told this
                 * failed has nothing held for it. Taking it first left
                 * a record allocated to a client that was refused,
                 * which is the leak this server exists to measure,
                 * arrived at from the other end.
                 */
                if (record_for(peer) == NULL) {
                    rep->lp_status = SYS_ERR_NO_MEMORY;
                    break;
                }

                /*
                 * And it stops here, still owing that reply. The handle
                 * went first: a process that keeps a thread handle can
                 * never be reaped, which stage one's review measured.
                 */
                (void)sys_fence_wait(hold_never, 1, SYS_DEADLINE_NEVER);
                sys_thread_exit(5);
            }

            case LEDGER_OP_DROP:
                r = record_for(peer);
                if (r != NULL) {
                    r->taken = 0;
                    r->owner = 0;
                }
                break;

            case LEDGER_OP_COUNT:
                break;

            case LEDGER_OP_HELD_FOR: {
                uint32_t i;

                rep->lp_status = SYS_ERR_NO_ENT;
                for (i = 0; i < LEDGER_RECORDS; i++) {
                    if (records[i].taken &&
                      SYS_ID_TAG32(records[i].owner) == req->lr_tag) {
                        rep->lp_status = SYS_OK;
                        break;
                    }
                }
                break;
            }

            default:
                rep->lp_status = SYS_ERR_INVALID_ARG;
                break;
            }
        }

        rep->lp_held = held();
        rep->lp_taken = taken_ever;
        rep->lp_notices = notices;
        rep->lp_released = released;

        if (sys_msg_reply(peer, &reply) != SYS_OK) {
            sys_thread_exit(3);
        }
    }
}

int
main(int argc, char **argv)
{
    sys_handle_t never;

    (void)argc;
    (void)argv;

    chan = ns_lookup(SYS_NS_SERVICE);
    never = ns_lookup(SYS_NS_PRIVATE + 0);
    if (chan == SYS_HANDLE_INVALID || never == SYS_HANDLE_INVALID) {
        return 1;
    }
    hold_never = never;

    /*
     * The first thread is a serving thread like any other. Holding a
     * request is a request code now rather than an arrangement about
     * which message arrived first, so nothing here is special and a
     * check does not have to reason about ordering to get a client
     * left inside a rendezvous.
     */
    serve(NULL);

    return 6;
}
