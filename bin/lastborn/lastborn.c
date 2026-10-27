/* lastborn.c : sends a thread's entry point and stack to the supervisor.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * The first message carries the address of kid() and the top of a stack in
 * this program's image, and it is never answered. The supervisor makes a
 * thread from those values, and that thread sends its own message on the
 * same channel and waits. Both threads then stay inside a request until the
 * supervisor ends the process.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define STACK_BYTES 4096

static unsigned char stack_kid[STACK_BYTES] __attribute__((aligned(16)));

#define ERR_NS      1
#define ERR_RETURN  3

/* What the first message carries, and all that it carries. */
struct makings {
    uint64_t    entry;
    uint64_t    stack;
};

static sys_handle_t serve;

/*
 * The thread the supervisor makes. One byte of message, which is
 * how the supervisor tells this apart from the first one, and then
 * the same wait: this thread is here to be killed, not to finish.
 */
static void
kid(void *arg)
{
    sys_msg_t request, reply;

    (void)arg;

    memset(&request, 0, sizeof(request));
    memset(&reply, 0, sizeof(reply));
    request.handle = SYS_HANDLE_INVALID;
    request.inline_len = 1;
    request.inline_data[0] = 1;

    (void)sys_msg_send(serve, &request, &reply);
    sys_thread_exit(ERR_RETURN);
}

int
main(void)
{
    sys_msg_t request, reply;
    struct makings m;

    serve = ns_lookup(SYS_NS_SERVICE);
    if (serve == SYS_HANDLE_INVALID) {
        return ERR_NS;
    }

    /*
     * The top of the stack, because a stack grows down and the
     * kernel checks the alignment of what it is given rather than
     * guessing which end was meant. The array is aligned, so its
     * end is too.
     */
    m.entry = (uint64_t)(uintptr_t)kid;
    m.stack = (uint64_t)(uintptr_t)(stack_kid + STACK_BYTES);

    memset(&request, 0, sizeof(request));
    memset(&reply, 0, sizeof(reply));
    request.handle = SYS_HANDLE_INVALID;
    request.inline_len = sizeof(m);
    memcpy(request.inline_data, &m, sizeof(m));

    (void)sys_msg_send(serve, &request, &reply);

    return ERR_RETURN;
}
