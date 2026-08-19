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
