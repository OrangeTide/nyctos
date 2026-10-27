/* connection.h : client connections to a channel's server.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares struct connection, which carries a server-defined cookie that
 * the kernel delivers with every message sent on it. Also declares the
 * functions to create, destroy, retain, release, and query a connection.
 * The kernel never interprets the cookie. Included by the kernel channel,
 * connection, operation, pager, process, and object code.
 *
 */

#ifndef _SYS_CONNECTION_H_
#define _SYS_CONNECTION_H_

#include <sys/op.h>
#include <sys/pool.h>

#include <stdbool.h>
#include <stdint.h>

struct channel;
struct proc;

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
     * the last one closes: a connection is named by nothing else, so a
     * handle going is the whole of its lifetime.
     *
     * Unless the channel behind it collects its departed peers, in which
     * case the last close is not the end: the
     * connection is kept, marked gone, and returned to its pool when
     * the server collects it.
     */
    uint32_t         cn_refs;

    /*
     * Handles held by anything but the process that owns the channel.
     * That is what a peer is: the server minting a handle to a
     * connection of its own is not a client of it, and a connection
     * whose peers have all gone is one nobody is going to send on
     * again.
     */
    uint32_t         cn_peers;

    /*
     * The process that owned the channel when this connection was
     * made, which is who the count above is counted against. Taken
     * once, because a channel's owner can change and a count that
     * added on one answer and subtracted on another would be wrong in
     * whichever direction the change went.
     */
    sys_id_t         cn_server;

    /* Whether every peer handle has closed, having once existed. */
    bool             cn_gone;

    /* Whether somebody is already giving this slot back. */
    bool             cn_freeing;

    /* The channel's list of connections, for collecting them. */
    struct connection *cn_next;
};

struct connection *connection_create(struct channel *ch, uint64_t cookie);

/*
 * The process that serves this connection, which is the one that owns
 * the channel behind it. The pager chain is walked through
 * this: a pager is a process, and what names one here is a connection.
 */
struct proc *connection_owner(const struct connection *cn);
void connection_destroy(struct connection *cn);

/*
 * A reference naming this connection was taken, or dropped, and by
 * whom. A connection is named by handles and by nothing else, so the
 * last drop is the whole of its lifetime, unless the channel behind it
 * collects its departed peers. Who is holding it matters because a
 * handle in the server's own table is not a client. See the
 * definitions.
 */
void connection_retain(struct connection *cn, const struct proc *by);
void connection_release(struct connection *cn, const struct proc *by);

/* Whether nothing names this connection any more. */
bool connection_unheld(const struct connection *cn);

#endif /* _SYS_CONNECTION_H_ */
