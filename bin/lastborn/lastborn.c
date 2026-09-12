/*
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * A process that hands out the makings of one more thread, for M21
 * stage five.
 *
 * The stage asks what happens when a thread is made in a process
 * that has already been killed. Answering it needs the same request
 * made twice, once while the process is alive and once after the
 * mark, so that the only difference between the two answers is the
 * mark. The supervisor cannot invent the request: a thread needs an
 * entry point and a stack in the program's own image, and only the
 * program knows where those are.
 *
 * So this sends them. The first message carries the address of the
 * function below and the top of a stack for it, and is never
 * answered, which leaves this thread inside a rendezvous and the
 * process alive for as long as the supervisor wants it. The thread
 * the supervisor then makes runs `kid`, which says so on the same
 * channel and waits in the same way.
 *
 * After that there are two threads, both inside a request, and the
 * kill has something to reach.
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
