/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Synchronous message passing, per section 12.
 *
 * Strict send, receive, reply: a send blocks until a server receives the
 * message and replies to it. There is no queue of messages, because the
 * blocked senders are the queue.
 *
 * That queue is ordered by sender priority and not by arrival. FIFO here
 * would be a priority inversion no amount of donation could repair, since
 * the inversion would be in the queue rather than in the execution.
 *
 * A reply is addressed by the client's thread id and needs no transaction
 * identifier, because section 5 has a thread blocked on at most one thing:
 * a blocked sender is uniquely named by its id and there is nothing to
 * disambiguate. The generation in the id closes the recycling hazard, so a
 * reply to a client that died is refused rather than delivered to whatever
 * now occupies the slot.
 */

#ifndef _SYS_CHANNEL_H_
#define _SYS_CHANNEL_H_

struct connection;

#include <sys/config.h>
#include <sys/op.h>
#include <sys/config.h>
#include <sys/pool.h>

#include <stdbool.h>
#include <stdint.h>

struct proc;
struct thread;

struct channel {
    sys_id_t        c_id;
    const char     *c_name;
    struct proc    *c_owner;

    /* Blocked senders, highest priority first. */
    struct thread  *c_senders;

    /* Threads blocked in receive, in no particular order. */
    struct thread  *c_receivers;

    /*
     * Threads that took a message from this channel and have not
     * replied yet, newest first, linked through t_serve_next.
     *
     * Section 12.3 wants a link from a blocked sender to whoever is
     * serving it, and this is that link. Without it a client queued on
     * a busy server can raise nothing, because there is no way from
     * the channel to the thread that is inside a request on it.
     */
    struct thread  *c_serving;

    /*
     * The connections minted on this channel, and whether their
     * departed peers are kept for the server to collect, per M18 stage
     * four. Off by default: keeping every connection anybody ever made
     * would spend the pool on servers that were never going to ask.
     */
    struct connection *c_conns;
    bool            c_reaping;

    /*
     * How many senders this channel will hold, and whether anybody
     * said. Per M18 stage six: a queue nobody bounded grows until the
     * machine runs out of threads, because every queued sender is one.
     * Zero is a depth like any other and refuses everything, which is
     * why the bound needs a flag of its own rather than a reserved
     * value.
     */
    uint32_t        c_depth;
    bool            c_bounded;

    /*
     * Pulses waiting to be collected, per section 13.1, oldest first.
     *
     * One array with a reservation rather than two queues, so that
     * order between a system pulse and the user pulses around it is
     * kept: a server that is told a client died wants that in the
     * sequence it happened in, not ahead of or behind everything.
     * What the reservation buys is that the user region cannot grow
     * into the last SYS_PULSE_RESERVED slots.
     */
    sys_pulse_t     c_pulses[SYS_PULSE_SLOTS];
    uint32_t        c_pulse_head;   /* next to collect */
    uint32_t        c_pulse_count;
    uint32_t        c_pulse_users;  /* how many of those are user pulses */
    bool            c_pulse_lost;
};

extern uint64_t channel_rendezvous;

/*
 * Sends that gave up at their deadline: one count for a client still on
 * the queue, which no server had seen, and one for a client whose
 * message a server was holding. Section 12.1 has the two differ in what
 * the server learns, so they are counted apart.
 */
extern uint64_t channel_gave_up_queued;
extern uint64_t channel_gave_up_working;

/*
 * Clients released because the one thread that could have answered them
 * ended, per section 10.5. Not the same as a client released by the
 * death of a whole server, which is section 12.5 and says so with a
 * different error.
 */
extern uint64_t channel_stale_clients;

/* Pulses dropped for want of a slot, per M18 stage five. */
extern uint64_t channel_pulses_lost;

/*
 * Senders a bounded queue would not hold, per M18 stage six. Counted
 * apart by which of the two it was, because the client cannot tell them
 * apart and the boot should be able to: one arrived at a full queue and
 * was turned away, and one was already waiting when somebody who
 * outranked it arrived.
 */
extern uint64_t channel_senders_refused;
extern uint64_t channel_senders_displaced;


/*
 * Whether any channel still names this process as its owner. A channel
 * keeps that pointer and takes no reference for it, so reaping an owner
 * would leave it naming a freed slot. See the implementation.
 */
bool channel_owned_by(const struct proc *p);

/*
 * Undo what a channel operation was holding on behalf of a thread that
 * has been cancelled out of it: the buffer it lent, and the expectation
 * of a reply. See the implementation.
 */
void channel_thread_cancelled(struct thread *t);

/*
 * Leave a pulse for whoever receives on this channel, per section 13.1.
 * Never blocks and never fails: a full user region costs an older user
 * pulse, and the reserved slots keep a system pulse from being the one
 * that is lost.
 */
uint64_t channel_chain_raised(uint64_t *deepest);

void channel_pulse(struct channel *ch, uint16_t code, uint32_t value);

/*
 * A process that owned channels has terminated, so anything blocked
 * sending to one of them is released with SYS_ERR_OWNER_DEAD rather
 * than left waiting for a server that is gone. Section 12.5.
 */
void channel_owner_died(struct proc *p);

/*
 * Say who may receive on this channel, and so whose death leaves its
 * clients with nobody to answer them. Called when a handle carrying
 * SYS_RIGHT_RECV is put into a table.
 */
void channel_set_owner(struct channel *ch, struct proc *p);
struct proc *channel_owner(const struct channel *ch);

struct channel *channel_create(const char *name, struct proc *owner);

/*
 * Send, and block until a reply. The message is copied into the receiving
 * thread rather than shared, so neither side holds a pointer into the
 * other; see section 12.4 on why the inline payload exists at all.
 */
/*
 * Send and wait for the reply. The deadline is absolute, on the
 * monotonic timebase, and SYS_DEADLINE_NEVER is what a caller with
 * nothing to say about time passes. A deadline already past makes this
 * a poll: it completes if a server is waiting and returns
 * SYS_ERR_TIMEOUT if none is.
 */
sys_err_t channel_send(struct channel *ch, uint64_t cookie,
  const struct sys_msg *req, struct sys_msg *reply, uint64_t deadline);

/* Receive, filling out_peer with the id to reply to. */
sys_err_t channel_recv(struct channel *ch, struct sys_msg *out,
  sys_thread_id_t *out_peer, uint64_t *out_cookie);

sys_err_t channel_reply(sys_thread_id_t peer, const struct sys_msg *reply);

/* Take a sender or receiver off a channel, for a wake from elsewhere. */
void channel_unwait_thread(void *obj, struct thread *t);

/*
 * Count a send that gave up at its deadline, apart by which state it
 * was in. Called from the deadline path only: the unwait hook is
 * reached by cancellation too.
 */
void channel_gave_up(const struct thread *t);

/*
 * A thread is ending: take it off any channel's list of threads inside
 * a request. A server that exits without replying is not serving.
 */
void channel_thread_exited(struct thread *t);

/*
 * Collect a connection on this channel whose peers have all gone,
 * writing the cookie it was minted with. SYS_ERR_NO_ENT when there is
 * none. Enabling is what makes departed peers be kept at all.
 */
sys_err_t channel_reap(struct channel *ch, bool enable, uint64_t *out);

/* How many senders this channel will hold, per M18 stage six. */
void channel_listen(struct channel *ch, uint32_t depth);

/* Link and unlink a connection, which the connection code calls. */
void channel_conn_add(struct channel *ch, struct connection *cn);
void channel_conn_gone(struct channel *ch, struct connection *cn);
void channel_conn_remove(struct channel *ch, struct connection *cn);
bool channel_reaping(const struct channel *ch);

#endif /* _SYS_CHANNEL_H_ */
