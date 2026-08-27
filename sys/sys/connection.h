/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Connections: what a client holds onto a server.
 *
 * Section 12.1 separates creating a channel from connecting to it, and
 * section 25.2 makes a connection the thing that names a file or a
 * directory. The server holds the state binding a connection to a node,
 * and the kernel carries an opaque word on its behalf without ever
 * looking at it.
 *
 * This is what makes handle-relative naming real rather than conventional.
 * The authority is a kernel object, so a client cannot forge a reference
 * to a node it was never given, and handing a directory to a child is an
 * ordinary handle move rather than a string it could have guessed.
 *
 * The object list in section 26 has no type for this, which looks like an
 * omission: a connection has to be a handle for any of the above to hold.
 */

#ifndef _SYS_CONNECTION_H_
#define _SYS_CONNECTION_H_

#include <sys/op.h>
#include <sys/pool.h>

#include <stdint.h>

struct channel;

struct connection {
    sys_id_t         cn_id;
    struct channel  *cn_channel;

    /*
     * Server-side state, opaque here. The kernel delivers it with every
     * message on this connection and attaches no meaning to it.
     */
    uint64_t         cn_cookie;

    /*
     * Handles naming this connection. It goes back to its pool when
     * the last one closes, which is the rule stage one gave processes
     * and threads and did not give this: a connection is named by
     * nothing else, so a handle going is the whole of its lifetime.
     */
    uint32_t         cn_refs;
};

struct connection *connection_create(struct channel *ch, uint64_t cookie);

/*
 * The process that serves this connection, which is the one that owns
 * the channel behind it. Section 15.4 walks the pager chain through
 * this: a pager is a process, and what names one here is a connection.
 */
struct proc *connection_owner(const struct connection *cn);
void connection_destroy(struct connection *cn);

/*
 * A handle naming this connection was made, or destroyed. The second
 * one returns it to its pool if that was the last.
 */
void connection_ref(struct connection *cn);
void connection_unref(struct connection *cn);

#endif /* _SYS_CONNECTION_H_ */
