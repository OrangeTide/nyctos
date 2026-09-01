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

    return cn;
}

void
connection_destroy(struct connection *cn)
{
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
 * A connection is named by handles and by nothing else, so a handle
 * going is the whole of its lifetime.
 *
 * Nothing freed one before this. The kernel destroyed a connection only
 * when the handle it was being put into could not be made, so every
 * connection anybody ever held leaked its slot when the handle closed,
 * and the pool was a count of connections ever made rather than
 * connections open at once. Section 25.2 says that pool is a build-time
 * bound on every file open at once, which it could not be.
 */
void
connection_ref(struct connection *cn)
{
    intr_state_t s = objref_enter();

    cn->cn_refs++;

    objref_leave(s);
}

void
connection_unref(struct connection *cn)
{
    intr_state_t s = objref_enter();
    bool doomed;

    if (cn->cn_refs == 0) {
        objref_leave(s);
        panic("connection: unreferenced more often than referenced");
    }

    doomed = --cn->cn_refs == 0;

    objref_leave(s);

    if (doomed) {
        connection_destroy(cn);
    }
}
