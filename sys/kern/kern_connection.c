/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <sys/channel.h>
#include <sys/connection.h>
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
