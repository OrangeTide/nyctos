/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Send, receive, reply. See sys/sys/channel.h for the shape and section 12
 * for the reasoning.
 */

#include <sys/channel.h>
#include <sys/intr.h>
#include <sys/object.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/timeline.h>

struct channel *
channel_create(const char *name, struct proc *owner)
{
    struct channel *ch;

    ch = pool_alloc(channel_pool(), NULL);
    if (ch == NULL) {
        return NULL;
    }

    ch->c_id = pool_id_of(channel_pool(), ch);
    ch->c_name = name;
    ch->c_owner = owner;
    ch->c_senders = NULL;
    ch->c_receivers = NULL;

    return ch;
}

/*
 * Insert into the sender queue by effective priority, highest first, and
 * FIFO within a priority. This ordering is the whole reason the queue
 * exists as a sorted structure rather than a list.
 */
static void
sender_insert(struct channel *ch, struct thread *t)
{
    struct thread **link = &ch->c_senders;

    while (*link != NULL && (*link)->t_prio >= t->t_prio) {
        link = &(*link)->t_snext;
    }

    t->t_snext = *link;
    *link = t;
}

static struct thread *
list_pop(struct thread **head)
{
    struct thread *t = *head;

    if (t != NULL) {
        *head = t->t_snext;
        t->t_snext = NULL;
    }

    return t;
}

static bool
list_remove(struct thread **head, struct thread *t)
{
    while (*head != NULL && *head != t) {
        head = &(*head)->t_snext;
    }
    if (*head != t) {
        return false;
    }

    *head = t->t_snext;
    t->t_snext = NULL;

    return true;
}

void
channel_unwait_thread(void *obj, struct thread *t)
{
    struct channel *ch = obj;

    if (!list_remove(&ch->c_senders, t)) {
        (void)list_remove(&ch->c_receivers, t);
    }
    t->t_blocked_on = NULL;
}

/*
 * Raise a thread to at least prio, and follow it to whatever it is itself
 * blocked sending to. Section 12.3 makes donation transitive so that a
 * chain of servers all run at the priority of whoever is ultimately
 * waiting; the bound is what keeps this walk finite with interrupts off.
 */
static sys_err_t
donate(struct thread *to, uint8_t prio)
{
    unsigned int depth;

    for (depth = 0; to != NULL; depth++) {
        if (depth >= SYS_PRIO_CHAIN_MAX) {
            return SYS_ERR_DEADLOCK;
        }
        if (to->t_prio >= prio) {
            return SYS_OK;
        }

        sched_reprioritise(to, prio);

        /* Follow the chain only where this thread is itself waiting. */
        to = to->t_donor;
    }

    return SYS_OK;
}

/* Recompute a thread's priority now that it owes nobody anything. */
static void
undonate(struct thread *t)
{
    t->t_donor = NULL;
    if (t->t_prio != t->t_base_prio) {
        sched_reprioritise(t, t->t_base_prio);
    }
}

static void
msg_copy(struct sys_msg *dst, const struct sys_msg *src)
{
    memcpy(dst, src, sizeof(*dst));
}

sys_err_t
channel_send(struct channel *ch, const struct sys_msg *req,
  struct sys_msg *reply)
{
    struct thread *self = thread_current();
    struct thread *server;
    intr_state_t s;
    sys_err_t err;

    s = md_intr_disable();

    self->t_msg = (struct sys_msg *)req;
    self->t_reply = reply;
    self->t_replied = false;
    self->t_waitres = SYS_ERR_INTERRUPTED;

    server = list_pop(&ch->c_receivers);
    if (server != NULL) {
        /*
         * A server was already waiting, so the rendezvous completes now.
         * It takes the message and this thread stays blocked for the
         * reply, which is what makes the blocked sender the queue slot.
         */
        msg_copy(server->t_msg, req);
        server->t_client = thread_id(self);
        server->t_donor = self;
        server->t_blocked_on = NULL;
        server->t_waitres = SYS_OK;

        err = donate(server, self->t_prio);
        if (err != SYS_OK) {
            md_intr_restore(s);
            return err;
        }

        sched_ready(server);
    } else {
        sender_insert(ch, self);
    }

    self->t_blocked_on = ch;
    self->t_unwait = channel_unwait_thread;
    sched_block();

    err = self->t_waitres;

    self->t_msg = NULL;
    self->t_reply = NULL;

    md_intr_restore(s);

    return err;
}

sys_err_t
channel_recv(struct channel *ch, struct sys_msg *out,
  sys_thread_id_t *out_peer)
{
    struct thread *self = thread_current();
    struct thread *client;
    intr_state_t s;

    s = md_intr_disable();

    client = list_pop(&ch->c_senders);
    if (client != NULL) {
        msg_copy(out, client->t_msg);
        self->t_client = thread_id(client);
        self->t_donor = client;

        (void)donate(self, client->t_prio);

        *out_peer = self->t_client;
        md_intr_restore(s);

        return SYS_OK;
    }

    self->t_msg = out;
    self->t_snext = ch->c_receivers;
    ch->c_receivers = self;

    self->t_blocked_on = ch;
    self->t_unwait = channel_unwait_thread;
    sched_block();

    *out_peer = self->t_client;

    md_intr_restore(s);

    return self->t_waitres;
}

/*
 * Reply, and stop running at the client's priority.
 *
 * A reply to a client that has died is refused rather than delivered,
 * because the id carries a generation and the slot it names is no longer
 * the thread that sent the request.
 */
sys_err_t
channel_reply(sys_thread_id_t peer, const struct sys_msg *reply)
{
    struct thread *self = thread_current();
    struct thread *client;
    intr_state_t s;

    s = md_intr_disable();

    client = thread_by_id(peer);
    if (client == NULL || client->t_reply == NULL) {
        md_intr_restore(s);
        return SYS_ERR_STALE;
    }

    msg_copy(client->t_reply, reply);
    client->t_replied = true;
    client->t_waitres = SYS_OK;
    client->t_blocked_on = NULL;

    if (self->t_donor == client) {
        undonate(self);
    }
    self->t_client = 0;

    sched_ready(client);

    md_intr_restore(s);

    return SYS_OK;
}
