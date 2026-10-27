/* console.c : the console service and its UART driver, in one process.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * A receive thread stores bytes from the UART in a 256-byte ring when the
 * interrupt fires. A request thread answers line reads from that ring. Both
 * threads run at the same priority, so neither preempts the other between
 * the blocking calls where they touch the ring.
 *
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
 * What the parent agreed to hand over. The identifiers below
 * SYS_NS_PRIVATE are reserved for things that mean the same
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
 * around it here.
 */
static char ring[RING_BYTES];
static uint32_t ring_head;      /* written by the receive thread */
static uint32_t ring_tail;      /* written by the request thread */
static uint32_t ring_lines;     /* complete lines waiting to be read */

static sys_thread_id_t waiting_peer;
static uint32_t waiting_want;
static int waiting;

/* Returns whether the byte was stored, which decides whether a
 * newline counts as a line: a line whose terminator was dropped is
 * not a line, and counting one would leave a reader waiting for a
 * terminator that is not in the ring. */
static int
ring_put(char c)
{
    uint32_t next = (ring_head + 1) % RING_BYTES;

    if (next == ring_tail) {
        return 0;       /* full: the newest byte is the one that goes */
    }
    ring[ring_head] = c;
    ring_head = next;

    return 1;
}

static void
put(const char *s, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++) {
        /*
         * A newline on a terminal is two characters, and a program
         * that had to know that would be a program written for this
         * console rather than for a console. The driver is where a
         * terminal convention belongs, which is the same reason it is
         * the driver that echoes and erases.
         */
        if (s[i] == '\n') {
            uart_putc('\r');
        }
        uart_putc(s[i]);
    }
}

/*
 * Take up to max bytes of one line out of the ring, stopping after its
 * newline.
 *
 * A line longer than the reply can carry is not an error and is not
 * truncated: what fits is returned, the rest stays where it is, and the
 * line stays counted until its newline has actually gone. Decrementing
 * on the first read instead left the remainder in the ring with nothing
 * saying it was there, so the next read was held and then answered with
 * the tail of the line before it.
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
            ring_lines--;
            break;
        }
    }

    return n;
}

/*
 * Answer a read. Either thread may do this: a reply is addressed by the
 * client's thread identifier, and any thread of a server may send one,
 * which is what lets the thread that has the bytes answer the
 * request the other thread accepted.
 */
static void
reply_line(sys_thread_id_t peer, uint32_t want)
{
    sys_msg_t reply;
    fs_reply_t rep;
    struct wire_out out;
    uint32_t max;

    memset(&reply, 0, sizeof(reply));
    memset(&rep, 0, sizeof(rep));
    rep.fr_status = SYS_OK;
    max = SYS_IPC_INLINE_BYTES - sizeof(fs_reply_t);
    if (want < max) {
        max = want;
    }
    rep.fr_length = take_line((char *)&reply.inline_data[sizeof(fs_reply_t)],
      max);
    reply.inline_len = sizeof(fs_reply_t) + rep.fr_length;

    wire_out_init(&out, reply.inline_data, SYS_IPC_INLINE_BYTES);
    fs_reply_put(&out, &rep);

    /* The same answer where the kernel can read it. */
    reply.status = rep.fr_status;

    (void)sys_msg_reply(peer, &reply);
}
/*
 * Blocked on the interrupt timeline, which is the whole of what a driver
 * gets: the handler advanced a counter and returned, and
 * everything the device needs is done here, at this thread's own priority
 * and preemptible like anything else.
 */
static void
rx_thread(void *arg)
{
    uint64_t seen = 0;
    int after_cr = 0;
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
                /*
                 * A terminal sends CR and a pipe sends LF, so both end
                 * a line, and a terminal that sends CR LF must not end
                 * two. The second half of a pair is dropped rather than
                 * delivered as an empty line.
                 */
                if (c == '\n' && after_cr) {
                    after_cr = 0;
                    continue;
                }
                after_cr = (c == '\r');
                put("\n", 1);
                if (ring_put('\n')) {
                    ring_lines++;
                }
            } else if (c == 0x7f || c == 0x08) {
                after_cr = 0;
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
                after_cr = 0;
                ring_put((char)c);
                uart_putc((char)c);
            }
        }
        /*
         * Drained to empty, then unmasked, in that order. The handler
         * masks a source it cannot finish with, and this
         * is the driver saying it has finished with it.
         */
        (void)sys_irq_unmask(h_irq);
        if (waiting && ring_lines > 0) {
            waiting = 0;
            reply_line(waiting_peer, waiting_want);
        }
    }
}
/*
 * The request loop. A read with nothing to return is not
 * refused and does not block this thread: the request is held, which is
 * what a client blocked in a rendezvous already means, and the receive
 * thread answers it when a line arrives.
 */
int
main(void)
{
    sys_handle_t th;
    sys_msg_t msg, reply;
    struct wire_in in;
    struct wire_out out;
    sys_thread_id_t peer;
    uint64_t cookie;
    fs_request_t req;
    fs_reply_t rep;
    sys_handle_t h_dev;
    uintptr_t regs, span;

    /*
     * The registers first. A driver is a program that
     * holds a grant, and this is where it finds out what it holds: the
     * address is not in this image, and a copy started without the grant
     * stops here rather than reaching for something it was not given.
     */
    h_dev = ns_lookup(SYS_NS_DEVICE);
    if (h_dev == SYS_HANDLE_INVALID) {
        DBG_PUTS("console: spawned without its device");
        return -4;
    }
    if (sys_grant_map(h_dev, &regs, &span) != SYS_OK) {
        DBG_PUTS("console: the device grant would not map");
        return -5;
    }
    if (uart_attach(regs, span) != 0) {
        DBG_PUTS("console: the grant is not the device this drives");
        return -6;
    }

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

        wire_in_init(&in, msg.inline_data, msg.inline_len);
        fs_request_get(&in, &req);

        memset(&reply, 0, sizeof(reply));
        memset(&rep, 0, sizeof(rep));
        rep.fr_status = SYS_ERR_NOT_SUPPORTED;
        reply.inline_len = sizeof(fs_reply_t);

        switch (req.fr_op) {
        case FS_OP_READ:
            if (ring_lines > 0) {
                reply_line(peer, req.fr_length);
                continue;
            }
            /*
             * Held rather than answered. There is one reader here and no
             * queue of them, which is what a console with one shell needs.
             */
            waiting_peer = peer;
            waiting_want = req.fr_length;
            waiting = 1;
            continue;

        case FS_OP_WRITE: {
            /*
             * Bounded by what arrived rather than by what the request
             * claims. The length is a number a program chose, and the
             * bytes behind it are only as many as the message carried.
             */
            uint32_t n = req.fr_length;
            uint32_t have = msg.inline_len > sizeof(fs_request_t)
              ? msg.inline_len - (uint32_t)sizeof(fs_request_t) : 0;

            if (n > have) {
                n = have;
            }
            put((const char *)&msg.inline_data[sizeof(fs_request_t)], n);
            rep.fr_status = SYS_OK;
            rep.fr_length = n;
            break;
        }
            break;

        case FS_OP_CLOSE:
            rep.fr_status = SYS_OK;
            break;

        default:
            /*
             * A server implements every operation, as a
             * rejection where it cannot do better, so that a client
             * discovers the limit where it uses it.
             */
            break;
        }

        wire_out_init(&out, reply.inline_data, SYS_IPC_INLINE_BYTES);
        fs_reply_put(&out, &rep);

        /* The same answer where the kernel can read it. */
        reply.status = rep.fr_status;

        (void)sys_msg_reply(peer, &reply);
    }
}
