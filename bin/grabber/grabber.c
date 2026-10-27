/* grabber.c : writes to a buffer after it has been moved in a message.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * The main thread sends a page-aligned buffer as a move. A second thread
 * waits on a timeline that the server signals after it takes the message and
 * before it replies, then writes to the buffer. The program reports 0 if the
 * write succeeded and 1 if the second thread faulted at the buffer, and any
 * other value means neither outcome was seen.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * The buffer, alone on its page. A move takes whole pages, so anything
 * sharing one with the buffer goes with it, and a stack or a flag that
 * vanished mid-call would be this program failing rather than the
 * contract being kept.
 */
#define PAGE_BYTES  4096

static char buf[PAGE_BYTES] __attribute__((aligned(PAGE_BYTES)));

/* The same size the other threaded program uses; see bin/threads. */
static unsigned char stack[4096] __attribute__((aligned(16)));

static sys_handle_t gate;
static volatile int wrote;

static void
toucher(void *arg)
{
    (void)arg;

    if (sys_fence_wait(gate, 1, SYS_DEADLINE_NEVER) != SYS_OK) {
        sys_thread_exit(-1);
    }

    /*
     * The violation. On a tier that enforces the contract this is the
     * last instruction this thread executes.
     */
    buf[0] = 'x';
    wrote = 1;

    sys_thread_exit(0);
}

int
main(void)
{
    sys_handle_t service, second;
    sys_msg_t message;
    sys_msg_t reply;
    sys_fault_t fault;
    uint32_t state;
    int32_t code;

    service = ns_lookup(SYS_NS_SERVICE);
    gate = ns_lookup(SYS_NS_PRIVATE);
    if (service == SYS_HANDLE_INVALID || gate == SYS_HANDLE_INVALID) {
        return 2;
    }

    /*
     * At this thread's own priority. Where it runs in the order does not
     * matter now that it waits for the window rather than hunting for
     * it: it blocks on the timeline wherever it is scheduled.
     */
    if (sys_thread_create(toucher, NULL, stack,
      sizeof(stack), SYS_PRIO_INHERIT, &second) != SYS_OK) {
        return 3;
    }

    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.buf = (uintptr_t)buf;
    message.buf_len = sizeof(buf);
    message.buf_flags = SYS_BUF_WRITE;

    if (sys_msg_send(service, &message, &reply) != SYS_OK) {
        return 4;
    }

    /*
     * The buffer is back, and so is the question of what happened to the
     * thread that touched it while it was away. This is the one place a
     * thread's own status is readable: the handle came from creating it,
     * and nothing hands out a handle to somebody else's thread.
     */
    if (sys_thread_status(second, &code, &state) != SYS_OK) {
        return 5;
    }
    if (state != SYS_STATE_EXITED) {
        return 6;
    }
    if (sys_thread_fault(second, &fault) != SYS_OK) {
        return 7;
    }

    if (fault.kind == SYS_FAULT_NONE) {
        return (wrote && code == 0) ? 0 : 8;
    }
    if (fault.kind != SYS_FAULT_ACCESS) {
        return 9;
    }
    if (fault.addr != (uintptr_t)buf) {
        return 10;
    }

    return 1;
}
