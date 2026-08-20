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

#include <sys/config.h>
#include <sys/op.h>
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
};

extern uint64_t channel_rendezvous;

struct channel *channel_create(const char *name, struct proc *owner);

/*
 * Send, and block until a reply. The message is copied into the receiving
 * thread rather than shared, so neither side holds a pointer into the
 * other; see section 12.4 on why the inline payload exists at all.
 */
sys_err_t channel_send(struct channel *ch, uint64_t cookie,
  const struct sys_msg *req, struct sys_msg *reply);

/* Receive, filling out_peer with the id to reply to. */
sys_err_t channel_recv(struct channel *ch, struct sys_msg *out,
  sys_thread_id_t *out_peer, uint64_t *out_cookie);

sys_err_t channel_reply(sys_thread_id_t peer, const struct sys_msg *reply);

/* Take a sender or receiver off a channel, for a wake from elsewhere. */
void channel_unwait_thread(void *obj, struct thread *t);

#endif /* _SYS_CHANNEL_H_ */
