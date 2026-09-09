/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <sys/channel.h>
#include <sys/connection.h>
#include <sys/objref.h>
#include <sys/object.h>
#include <sys/systm.h>

struct connection *
connection_create(struct channel *ch, uint64_t cookie)
{
    struct connection *cn;

    cn = pool_alloc(connection_pool(), NULL);
    if (cn == NULL) {
        return NULL;
    }

    cn->cn_id = pool_id_of(connection_pool(), cn);
    cn->cn_channel = ch;
    cn->cn_cookie = cookie;
    cn->cn_refs = 0;
    cn->cn_peers = 0;
    cn->cn_gone = false;
    cn->cn_freeing = false;

    /*
     * Who the server is, taken now and not looked up later. A channel's
     * owner is whoever last received a handle to it and can change
     * while this connection exists, and the count below has to add and
     * subtract on the same answer: a reference taken while one process
     * owned the channel and dropped after another does would leave the
     * count saying a client is still here, or saying one has gone while
     * it is still holding.
     *
     * An identifier rather than a pointer, because the process may be
     * reaped and its slot taken by another, and a pointer would then
     * name the wrong one.
     */
    cn->cn_server = (ch != NULL && ch->c_owner != NULL) ?
      ch->c_owner->p_id : 0;

    channel_conn_add(ch, cn);

    return cn;
}

/*
 * Give the slot back, once however many callers reach here.
 *
 * Two of them can. A connection kept back for a server to collect is
 * freed by whichever comes first, the server collecting it or its last
 * reference going, and at Tier 2 those run on different cores: the
 * losing one would free a slot the pool had already given to somebody
 * else. So the decision is made once, under the same lock the
 * reference count is under, and the second caller finds it made.
 */
void
connection_destroy(struct connection *cn)
{
    intr_state_t s = objref_enter();
    bool mine = !cn->cn_freeing;

    cn->cn_freeing = true;

    objref_leave(s);

    if (!mine) {
        return;
    }

    channel_conn_remove(cn->cn_channel, cn);
    (void)pool_free(connection_pool(), cn->cn_id);
}

/*
 * The process that serves this connection. A connection names a channel
 * and a channel has an owner, so the pager chain of section 15.4 is a
 * walk through two indirections and no new field.
 */
struct proc *
connection_owner(const struct connection *cn)
{
    if (cn == NULL || cn->cn_channel == NULL) {
        return NULL;
    }

    return cn->cn_channel->c_owner;
}

/* Whether a reference held by this process is a client's. */
static bool
is_peer(const struct connection *cn, const struct proc *by)
{
    return by == NULL || by->p_id != cn->cn_server;
}

/*
 * Whether nothing names this connection any more. Read under the lock
 * the count is kept under, for a caller outside this file that has to
 * decide whether a connection it is holding onto can go.
 */
bool
connection_unheld(const struct connection *cn)
{
    intr_state_t s = objref_enter();
    bool none = cn->cn_refs == 0;

    objref_leave(s);

    return none;
}

/*
 * A reference naming this connection was taken, or dropped, and by
 * whom. Nothing freed a connection before this pair existed in some
 * form: the kernel destroyed one only when the handle it was going
 * into could not be made, so every connection anybody held leaked its
 * slot, and the pool counted connections ever made rather than
 * connections open at once. Section 25.2 says that pool is a
 * build-time bound on every file open at once, which it could not be.
 *
 * Who matters because a connection has two sides and only one of them
 * is a peer. The process that owns the channel is the server: a handle
 * in its table is how it holds onto a client of its own, not how it is
 * one. Everything else is a peer, and a connection whose peers have all
 * closed is one nobody is going to send on again.
 *
 * That is the level state section 12.5 was missing. The pulse says
 * something happened and may be coalesced or lost; this is what it
 * happened to, and it keeps saying so until the server collects it.
 */
void
connection_retain(struct connection *cn, const struct proc *by)
{
    intr_state_t s = objref_enter();

    cn->cn_refs++;
    if (is_peer(cn, by)) {
        cn->cn_peers++;
    }

    objref_leave(s);
}

void
connection_release(struct connection *cn, const struct proc *by)
{
    intr_state_t s = objref_enter();
    bool gone = false;
    bool doomed;

    if (cn->cn_refs == 0) {
        objref_leave(s);
        panic("connection: unreferenced more often than referenced");
    }

    if (is_peer(cn, by) && cn->cn_peers > 0) {
        gone = --cn->cn_peers == 0;
        if (gone) {
            cn->cn_gone = true;
        }
    }

    doomed = --cn->cn_refs == 0;

    objref_leave(s);

    /*
     * Told first, and destroyed after, because the two are different
     * questions. A channel that collects its departed peers keeps this
     * connection whatever its handle count says, and one that does not
     * frees it exactly as it always did.
     */
    if (gone) {
        channel_conn_gone(cn->cn_channel, cn);
    }

    if (doomed && !(cn->cn_gone && channel_reaping(cn->cn_channel))) {
        connection_destroy(cn);
    }
}
