/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The console: a driver and the service in front of it, as one process
 * with two threads.
 *
 * Section 19.3 gives a driver that must stay responsive two threads and no
 * shared state: a request thread blocked in receive, and an interrupt
 * thread blocked on the timeline the interrupt advances. Both halves are
 * here. What is not here is the second process the roadmap first described,
 * because splitting the device from the service would cost a rendezvous per
 * line and buy nothing until there is a second character device to put
 * behind one service.
 *
 * The two threads do share state, which is where this departs from the
 * wording of section 19.3: the bytes have to cross from the thread that
 * receives them to the thread that answers for them. What makes that safe
 * is not a lock, because there is nothing to lock with. It is that both
 * threads run at the same priority, so neither preempts the other, and
 * every place either one touches the shared ring sits between two blocking
 * calls. Multicore is what breaks that argument, and multicore is M8.
 *
 * Input is a ring of bytes rather than a line buffer. A person types one
 * line at a time and a pipe delivers a whole session at once, and the ring
 * is what makes those the same thing to a reader. A full ring drops the
 * newest byte, which is the honest failure for a device with no flow
 * control and nowhere to push back to.
 */

#include <os/sys.h>
#include <debug.h>
#include <ns.h>
#include <stddef.h>
#include <string.h>
#include <sys/fs.h>

#include "uart.h"

#define RING_BYTES      256
#define STACK_BYTES     4096

/*
 * What the parent agreed to hand over. Section 20.4 reserves the
 * identifiers below SYS_NS_PRIVATE for things that mean the same
 * everywhere; an interrupt does not, so these two are a convention
 * between this program and whatever launched it, which is what the
 * private range is for.
 */
#define NS_IRQ          (SYS_NS_PRIVATE + 0)
#define NS_IRQ_FENCE    (SYS_NS_PRIVATE + 1)

static unsigned char rx_stack[STACK_BYTES] __attribute__((aligned(16)));

static sys_handle_t h_chan;
static sys_handle_t h_irq;
static sys_handle_t h_fence;

/*
 * The shared state, and all of it. head and tail are written by different
 * threads; see the note at the top of this file for why that needs nothing
 * around it here and will need something in M8.
 */
static char ring[RING_BYTES];
static uint32_t ring_head;      /* written by the receive thread */
static uint32_t ring_tail;      /* written by the request thread */
static uint32_t ring_lines;     /* complete lines waiting to be read */

static sys_thread_id_t waiting_peer;
static int waiting;

static void
ring_put(char c)
{
    uint32_t next = (ring_head + 1) % RING_BYTES;

    if (next == ring_tail) {
        return;         /* full: the newest byte is the one that goes */
    }
    ring[ring_head] = c;
    ring_head = next;
}

static void
put(const char *s, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++) {
        uart_putc(s[i]);
    }
}

/*
 * Take one line out of the ring, up to and including its newline, and put
 * it in the reply. The caller has already established that there is one.
 */
static uint32_t
take_line(char *out, uint32_t max)
{
    uint32_t n = 0;

    while (ring_tail != ring_head && n < max) {
        char c = ring[ring_tail];

        ring_tail = (ring_tail + 1) % RING_BYTES;
        out[n++] = c;
        if (c == '\n') {
            break;
        }
    }

    ring_lines--;

    return n;
}

/*
 * Answer a read. Either thread may do this: section 12.2 addresses a reply
 * by the client's thread identifier and says any thread of a server may
 * send one, which is what lets the thread that has the bytes answer the
 * request the other thread accepted.
 */
static void
reply_line(sys_thread_id_t peer)
{
    sys_msg_t reply;
    fs_reply_t *rep;

    memset(&reply, 0, sizeof(reply));
    rep = (fs_reply_t *)reply.inline_data;
    rep->fr_status = SYS_OK;
    rep->fr_length = take_line((char *)&reply.inline_data[sizeof(*rep)],
      SYS_IPC_INLINE_BYTES - sizeof(*rep));
    reply.inline_len = sizeof(*rep) + rep->fr_length;

    (void)sys_msg_reply(peer, &reply);
}

/*
 * Blocked on the interrupt timeline, which is the whole of what section
 * 13.2 gives a driver: the handler advanced a counter and returned, and
 * everything the device needs is done here, at this thread's own priority
 * and preemptible like anything else.
 */
static void
rx_thread(void *arg)
{
    uint64_t seen = 0;

    (void)arg;

    uart_rx_enable();

    for (;;) {
        if (sys_fence_wait(h_fence, seen + 1, SYS_DEADLINE_NEVER) != SYS_OK) {
            return;
        }

        /*
         * One more than the last value asked for, rather than the value
         * the timeline actually reached, because there is no operation
         * that reads one. A burst that advanced it three times costs two
         * extra passes around this loop, each finding an empty device and
         * asking for the next value, which is cheaper than an operation
         * to avoid them.
         */
        seen++;

        for (;;) {
            int c = uart_getc();

            if (c < 0) {
                break;
            }

            if (c == '\r' || c == '\n') {
                put("\r\n", 2);
                ring_put('\n');
                ring_lines++;
            } else if (c == 0x7f || c == 0x08) {
                /*
                 * Erase, which needs the ring to give a byte back. It can
                 * only take back what has not been read, and a newline
                 * ends what erasing can reach.
                 */
                if (ring_head != ring_tail) {
                    uint32_t prev = (ring_head + RING_BYTES - 1) % RING_BYTES;

                    if (ring[prev] != '\n') {
                        ring_head = prev;
                        put("\b \b", 3);
                    }
                }
            } else if (c >= 0x20 && c < 0x7f) {
                ring_put((char)c);
                uart_putc((char)c);
            }
        }

        /*
         * Drained to empty, then unmasked, in that order: section 13.2
         * has the handler mask a source it cannot finish with, and this
         * is the driver saying it has finished with it.
         */
        (void)sys_irq_unmask(h_irq);

        if (waiting && ring_lines > 0) {
            waiting = 0;
            reply_line(waiting_peer);
        }
    }
}

/*
 * The request loop, per section 12.6. A read with nothing to return is not
 * refused and does not block this thread: the request is held, which is
 * what a client blocked in a rendezvous already means, and the receive
 * thread answers it when a line arrives.
 */
int
main(void)
{
    sys_handle_t th;
    sys_msg_t msg, reply;
    sys_thread_id_t peer;
    uint64_t cookie;
    fs_request_t *req;
    fs_reply_t *rep;

    h_chan = ns_lookup(SYS_NS_SERVICE);
    h_irq = ns_lookup((sys_ns_id_t)NS_IRQ);
    h_fence = ns_lookup((sys_ns_id_t)NS_IRQ_FENCE);
    if (h_chan == SYS_HANDLE_INVALID || h_irq == SYS_HANDLE_INVALID ||
      h_fence == SYS_HANDLE_INVALID) {
        DBG_PUTS("console: spawned without a channel or an interrupt");
        return -1;
    }

    if (sys_thread_create(rx_thread, NULL, rx_stack, STACK_BYTES,
      SYS_PRIO_INHERIT, &th) != SYS_OK) {
        DBG_PUTS("console: no second thread");
        return -2;
    }

    for (;;) {
        memset(&msg, 0, sizeof(msg));
        if (sys_msg_recv(h_chan, &msg, &peer, &cookie) != SYS_OK) {
            return -3;
        }

        req = (fs_request_t *)msg.inline_data;

        memset(&reply, 0, sizeof(reply));
        rep = (fs_reply_t *)reply.inline_data;
        rep->fr_status = SYS_ERR_NOT_SUPPORTED;
        reply.inline_len = sizeof(*rep);

        switch (req->fr_op) {
        case FS_OP_READ:
            if (ring_lines > 0) {
                reply_line(peer);
                continue;
            }
            /*
             * Held rather than answered. There is one reader here and no
             * queue of them, which is what a console with one shell needs
             * and is the whole of what this milestone claims.
             */
            waiting_peer = peer;
            waiting = 1;
            continue;

        case FS_OP_WRITE:
            put((const char *)&msg.inline_data[sizeof(*req)],
              req->fr_length);
            rep->fr_status = SYS_OK;
            rep->fr_length = req->fr_length;
            break;

        case FS_OP_CLOSE:
            rep->fr_status = SYS_OK;
            break;

        default:
            /*
             * Section 25.3 has a server implement every operation, as a
             * rejection where it cannot do better, so that a client
             * discovers the limit where it uses it.
             */
            break;
        }

        (void)sys_msg_reply(peer, &reply);
    }
}
