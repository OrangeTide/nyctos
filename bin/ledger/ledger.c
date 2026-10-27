/* ledger.c : a server that keeps one record per client.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Each client that asks takes a slot in a fixed table, and the slot is freed
 * when the owning thread dies or its connection is reaped. Death pulses
 * release the records of a dead owner, and reaped connections are collected
 * before each reply. The first request is never answered, which keeps that
 * client blocked in a rendezvous until it is killed.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <string.h>
#include <sys/ledger.h>

struct record {
    sys_thread_id_t     owner;

    /*
     * The connection the request arrived on, which is what says the
     * client is gone after nobody said anything. A record is keyed on
     * the client and not on this, per stage three: two clients may
     * share a connection, and one of them dying does not end it.
     */
    uint64_t            conn;
    int                 taken;
};

static struct record records[LEDGER_RECORDS];

/* Records ever taken, and death notices ever seen, both for COUNT. */
static uint32_t taken_ever;
static uint32_t notices;

/* Records released because a notice named their client. */
static uint32_t released;

/* Connections collected, and records let go because one was. */
static uint32_t reaped;
static uint32_t drained;

/* Hints that a peer has gone, counted and acted on in no other way. */
static uint32_t hints;

/*
 * Receives that arrived saying a pulse had been dropped. Counted for
 * the same reason the hints are: this server does not need to know,
 * because it collects before every answer whether it was told anything
 * or not, and a check that can see the flag arriving is what says so.
 */
static uint32_t losses;

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
 * Let go of every record taken on a connection whose peers have all
 * gone, per M18 stage four.
 *
 * Every record, and not one: a connection is not a client, and two
 * clients that shared one are both gone when it is. That is the same
 * fact stage three used from the other side, where one of two clients
 * on a connection died and the other kept its record.
 */
static void
release_on(uint64_t conn)
{
    uint32_t i;

    for (i = 0; i < LEDGER_RECORDS; i++) {
        if (records[i].taken && records[i].conn == conn) {
            records[i].taken = 0;
            records[i].owner = 0;
            records[i].conn = 0;
            drained++;
        }
    }
}

/*
 * Collect what has gone, per M18 stage four.
 *
 * Called before every request is answered rather than when a pulse
 * arrives, which is the whole point of the arrangement: the pulse is a
 * hint that may be coalesced or lost, and this reads the state behind
 * it. A server that only drained when told would be as fragile as one
 * that acted on the notice directly, and this one is correct with every
 * hint thrown away.
 */
static void
drain(void)
{
    uint64_t conn;

    while (sys_chan_reap(chan, false, &conn) == SYS_OK) {
        reaped++;
        release_on(conn);
    }
}

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
record_for(sys_thread_id_t who, uint64_t conn)
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
            records[i].conn = conn;
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
        if ((message.flags & SYS_MSG_PULSE_LOST) != 0) {
            losses++;
        }

        if ((message.flags & SYS_MSG_IS_PULSE) != 0) {
            const sys_pulse_t *p = (const sys_pulse_t *)message.inline_data;

            if (p->code == SYS_PULSE_DEAD_OWNER) {
                notices++;
                release_for(p->value);
            }

            /*
             * Counted and nothing else, deliberately. The hint says
             * something is worth collecting and this server collects
             * before every answer anyway, so acting on it here would
             * hide whether it needs the hint at all.
             */
            if (p->code == SYS_PULSE_PEER_GONE) {
                hints++;
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
                if (record_for(peer, cookie) == NULL) {
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
                if (record_for(peer, cookie) == NULL) {
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
                r = record_for(peer, cookie);
                if (r != NULL) {
                    r->taken = 0;
                    r->owner = 0;
                    r->conn = 0;
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

        drain();

        rep->lp_held = held();
        rep->lp_taken = taken_ever;
        rep->lp_notices = notices;
        rep->lp_released = released;
        rep->lp_reaped = reaped;
        rep->lp_drained = drained;
        rep->lp_hints = hints;
        rep->lp_losses = losses;

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
     * This channel keeps a connection whose clients have all gone,
     * rather than freeing it, so that there is something left to
     * collect. Said once and it stays said: what it costs is a
     * connection slot per departed client until this server gets
     * around to asking, and what it buys is a server that is right
     * about who is still there without having been told.
     */
    (void)sys_chan_reap(chan, true, NULL);

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
