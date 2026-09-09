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
    cn->cn_peers = 0;
    cn->cn_gone = false;

    channel_conn_add(ch, cn);

    return cn;
}

void
connection_destroy(struct connection *cn)
{
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
    if (by != connection_owner(cn)) {
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

    if (by != connection_owner(cn) && cn->cn_peers > 0) {
        gone = --cn->cn_peers == 0;
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
        cn->cn_gone = true;
        channel_conn_gone(cn->cn_channel, cn);
    }

    if (doomed && !(cn->cn_gone && channel_reaping(cn->cn_channel))) {
        connection_destroy(cn);
    }
}

bool
connection_gone(const struct connection *cn)
{
    return cn->cn_gone;
}

uint64_t
connection_cookie(const struct connection *cn)
{
    return cn->cn_cookie;
}
