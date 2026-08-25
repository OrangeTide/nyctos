/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * A record of what threads blocked on and when, which is the smallest
 * useful thing section 22.2 means by "enough to build a tracer".
 *
 * It exists because the two sides of a rendezvous cannot be caught in the
 * act by looking. Whichever thread would do the looking is by definition
 * running, so it cannot see the state of a system in which something else
 * is running. The states have to be recorded as they are entered and read
 * afterwards, which is what a tracer is for.
 *
 * A fixed ring, overwritten when full, formatted only when something asks.
 * Section 22.1 requires that an unobserved system pay nothing for
 * introspection; what this costs when nothing reads it is two stores and a
 * mask per blocking transition. Formatting happens in the reader.
 *
 * This is not sys_stat_read(). Section 22 puts attributes behind a handle
 * and there are no thread handles yet, so what is here is the kernel's own
 * view, printed by the kernel. Section 22.3's rule that inspection follows
 * the supervision tree arrives with the supervisor that would use it.
 */

#include <sys/config.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/trace.h>

#include <stdbool.h>

static struct trace_event trace_ring[TRACE_EVENTS];
static uint32_t trace_head;      /* next slot to write */
static uint32_t trace_written;   /* events ever written, for overflow */
static bool trace_on;

void
trace_enable(bool on)
{
    trace_on = on;
}

void
trace_reset(void)
{
    trace_head = 0;
    trace_written = 0;
}

/*
 * Called from the blocking paths with interrupts already disabled, which
 * is what makes a bare increment safe here and would not on a second core.
 * Section 11.4 partitions the scheduler per core, so a ring per core is
 * the shape this takes when there is one.
 */
void
trace_wait(const struct thread *t, enum thread_wait wait)
{
    struct trace_event *e;

    if (!trace_on) {
        return;
    }

    e = &trace_ring[trace_head];
    e->tr_thread = t->t_name;
    e->tr_wait = (uint8_t)wait;
    e->tr_prio = t->t_prio;

    trace_head = (trace_head + 1) % TRACE_EVENTS;
    trace_written++;
}

static const char *
wait_name(uint8_t wait)
{
    switch (wait) {
    case THREAD_WAIT_NONE:  return "runs";
    case THREAD_WAIT_SEND:  return "send-blocked";
    case THREAD_WAIT_REPLY: return "reply-blocked";
    case THREAD_WAIT_RECV:  return "recv-blocked";
    case THREAD_WAIT_FENCE: return "fence-blocked";
    default:                return "?";
    }
}

void
trace_report(const char *what)
{
    uint32_t n = trace_written < TRACE_EVENTS ? trace_written : TRACE_EVENTS;
    uint32_t first = trace_written < TRACE_EVENTS ?
      0 : trace_head;
    uint32_t i;

    cnprintf("    %s: %lu transitions%s\n", what,
      (unsigned long)trace_written,
      trace_written > TRACE_EVENTS ? ", oldest lost" : "");

    for (i = 0; i < n; i++) {
        const struct trace_event *e = &trace_ring[(first + i) % TRACE_EVENTS];

        cnprintf("        %-12s prio %-3u %s\n", e->tr_thread,
          (unsigned)e->tr_prio, wait_name(e->tr_wait));
    }
}

/*
 * Whether a wait state was entered at all. The point of a trace here is
 * not the listing, which a person reads, but this, which a test reads: a
 * two-process demonstration where one program runs to completion before
 * the other starts looks identical in the output and exercises no blocking
 * whatsoever.
 */
bool
trace_saw(enum thread_wait wait)
{
    uint32_t n = trace_written < TRACE_EVENTS ? trace_written : TRACE_EVENTS;
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (trace_ring[i].tr_wait == (uint8_t)wait) {
            return true;
        }
    }

    return false;
}

/*
 * The same question about one thread rather than about the system.
 *
 * It exists because a second layer under a server made the system-wide
 * answer stop meaning what it used to. A client sending one message
 * cannot queue behind a busy server, which is the saving section 25.7
 * claims and the thing worth checking; but the server it sends to now
 * sends onward to a block store, and queues there. Asking whether
 * anything at all send-blocked answers yes either way, and stops being
 * a statement about the client.
 *
 * Compared by content and not by pointer. A thread's name is copied
 * into it at creation, so two threads of one image have equal names at
 * different addresses, and matching by address would silently answer
 * about the wrong one.
 */
bool
trace_saw_thread(const char *name, enum thread_wait wait)
{
    uint32_t n = trace_written < TRACE_EVENTS ? trace_written : TRACE_EVENTS;
    uint32_t i;

    for (i = 0; i < n; i++) {
        const char *a = trace_ring[i].tr_thread;
        const char *b = name;

        if (trace_ring[i].tr_wait != (uint8_t)wait || a == NULL) {
            continue;
        }
        while (*a != '\0' && *a == *b) {
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            return true;
        }
    }

    return false;
}
