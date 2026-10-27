/* fourways.c : a process with a thread in each state a kill must reach.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * The asker is blocked inside a request, the spinner runs without entering
 * the kernel, and the main thread waits to receive. The fourth state, a
 * thread that is ready but never run, is made by the supervisor from the
 * entry point and stack this program sends it.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define STACK_BYTES 4096

static unsigned char stack_ask[STACK_BYTES] __attribute__((aligned(16)));
static unsigned char stack_spin[STACK_BYTES] __attribute__((aligned(16)));
static unsigned char stack_idle[STACK_BYTES] __attribute__((aligned(16)));

/* What the makings of the fourth thread are sent as. */
struct makings {
    uint64_t    entry;
    uint64_t    stack;
};

#define ERR_NS      1
#define ERR_CREATE  2
#define ERR_RETURN  3

static sys_handle_t serve;
static sys_handle_t store;

/*
 * A client whose request a server has taken and will not answer,
 * which is section 10.5's interesting case: the thread is inside a
 * rendezvous rather than waiting to enter one.
 */
static void
asker(void *arg)
{
    sys_msg_t request, reply;

    (void)arg;

    memset(&request, 0, sizeof(request));
    memset(&reply, 0, sizeof(reply));
    request.handle = SYS_HANDLE_INVALID;
    request.inline_len = 1;

    (void)sys_msg_send(store, &request, &reply);
    sys_thread_exit(ERR_RETURN);
}

/* Computes and never enters the kernel again. */
static void
spinner(void *arg)
{
    volatile unsigned long n = 0;

    (void)arg;

    for (;;) {
        n++;
    }
}

/*
 * The thread the supervisor makes, which never runs.
 *
 * Its body is here because the stack and the entry point have to be
 * in this program's image, and only this program knows where those
 * are. It receives on a channel nothing sends to, so that a run
 * where it did reach user mode is a run that does not come back,
 * rather than one that quietly passes.
 */
static void
idler(void *arg)
{
    sys_msg_t message;
    sys_thread_id_t peer;
    uint64_t cookie;

    (void)arg;

    memset(&message, 0, sizeof(message));
    (void)sys_msg_recv(serve, &message, &peer, &cookie);
    sys_thread_exit(ERR_RETURN);
}

int
main(void)
{
    sys_msg_t message;
    sys_thread_id_t peer;
    sys_handle_t th;
    uint64_t cookie;

    serve = ns_lookup(SYS_NS_SERVICE);
    store = ns_lookup(SYS_NS_STORE);
    if (serve == SYS_HANDLE_INVALID || store == SYS_HANDLE_INVALID) {
        return ERR_NS;
    }

    /*
     * The makings of the fourth thread go first, before there is
     * anything else on this channel, and the answer to them is what
     * says the supervisor has them. Only then are the other two
     * made, so that the one request left unanswered on this channel
     * afterwards is the asker's.
     */
    {
        struct makings m;
        sys_msg_t request, reply;

        m.entry = (uint64_t)(uintptr_t)idler;
        m.stack = (uint64_t)(uintptr_t)(stack_idle + STACK_BYTES);

        memset(&request, 0, sizeof(request));
        memset(&reply, 0, sizeof(reply));
        request.handle = SYS_HANDLE_INVALID;
        request.inline_len = sizeof(m);
        memcpy(request.inline_data, &m, sizeof(m));
        if (sys_msg_send(store, &request, &reply) != SYS_OK) {
            return ERR_CREATE;
        }
    }

    /*
     * Each handle is given back as soon as the thread exists,
     * because nothing here joins them. A handle to a thread holds
     * the thread, a thread holds its process, and the table these
     * would sit in is the process's own, so keeping them is a
     * process that names itself and is never collected. Measured
     * before this was written: the four threads ended, the process
     * exited, and the reap never came.
     */
    if (sys_thread_create(asker, NULL, stack_ask, STACK_BYTES,
      SYS_PRIO_INHERIT, &th) != SYS_OK) {
        return ERR_CREATE;
    }
    (void)sys_handle_close(th);

    if (sys_thread_create(spinner, NULL, stack_spin, STACK_BYTES,
      SYS_PRIO_INHERIT, &th) != SYS_OK) {
        return ERR_CREATE;
    }
    (void)sys_handle_close(th);

    memset(&message, 0, sizeof(message));
    (void)sys_msg_recv(serve, &message, &peer, &cookie);

    return ERR_RETURN;
}
