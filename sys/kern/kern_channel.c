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
 * Raise a thread to at least prio.
 *
 * This is one step, and section 12.3 asks for a transitive chain. It gets
 * one anyway, by a different route: a server sends onward at its own
 * effective priority, which is already the donated value, so each hop
 * carries the donation forward without anything walking a chain. A client
 * at 25 reaching a server at 10 that calls a server at 5 leaves all three
 * running at 25.
 *
 * What that does not give is the bound. SYS_PRIO_CHAIN_MAX and the
 * SYS_ERR_DEADLOCK the section attaches to it presuppose a walk, and a
 * walk is only needed to raise a thread that is already blocked onward.
 * Nothing here can be: donation happens at the rendezvous, when the target
 * has just stopped being blocked. So the bound is unenforced rather than
 * enforced trivially, and a cycle of servers would deadlock undetected.
 * That is a real gap between section 12.3 and this, and it wants either a
 * link from a blocked sender to whoever is serving it, or the section
 * conceding that per-send donation is the whole mechanism.
 *
 * The earlier version of this walked from the target back to the thread
 * that had just donated to it, which by construction already outranked the
 * value being donated, so the loop always stopped on its first test. It
 * looked like a bounded chain walk and was a no-op.
 */
static void
donate(struct thread *to, uint8_t prio)
{
    if (to->t_prio < prio) {
        sched_reprioritise(to, prio);
    }
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
channel_send(struct channel *ch, uint64_t cookie, const struct sys_msg *req,
  struct sys_msg *reply)
{
    struct thread *self = thread_current();
    struct thread *server;
    intr_state_t s;
    sys_err_t err;

    s = md_intr_disable();

    self->t_cookie = cookie;
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
        server->t_cookie = cookie;
        server->t_donor = self;
        server->t_blocked_on = NULL;
        server->t_waitres = SYS_OK;

        donate(server, self->t_prio);
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
  sys_thread_id_t *out_peer, uint64_t *out_cookie)
{
    struct thread *self = thread_current();
    struct thread *client;
    intr_state_t s;

    s = md_intr_disable();

    client = list_pop(&ch->c_senders);
    if (client != NULL) {
        msg_copy(out, client->t_msg);
        self->t_client = thread_id(client);
        self->t_cookie = client->t_cookie;
        self->t_donor = client;

        donate(self, client->t_prio);

        *out_peer = self->t_client;
        *out_cookie = self->t_cookie;
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
    *out_cookie = self->t_cookie;

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
    sys_err_t err;

    s = md_intr_disable();

    client = thread_by_id(peer);
    if (client == NULL || client->t_reply == NULL) {
        md_intr_restore(s);
        return SYS_ERR_STALE;
    }

    /*
     * Section 7.4: a handle travels by move. The value changes across the
     * boundary because a handle means something only in the table holding
     * it, and the sender slot is invalidated rather than merely unused, so
     * presenting the old value afterwards is refused rather than reaching
     * the object.
     */
    msg_copy(client->t_reply, reply);
    client->t_reply->handle = SYS_ID_INVALID;

    err = SYS_OK;
    if (reply->handle != SYS_ID_INVALID) {
        err = handle_move(&self->t_proc->p_handles,
          &client->t_proc->p_handles, reply->handle,
          &client->t_reply->handle);
    }

    /*
     * The client is released whether or not the handle made it across.
     * Returning early on a failed transfer would leave it blocked on a
     * reply that has already been sent, which is a worse outcome than
     * telling both sides the transfer failed.
     */
    client->t_replied = true;
    client->t_waitres = err;
    client->t_blocked_on = NULL;

    if (self->t_donor == client) {
        undonate(self);
    }
    self->t_client = 0;

    sched_ready(client);

    md_intr_restore(s);

    return err;
}
