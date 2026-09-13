/*
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * A process with a thread in each of the states a kill has to reach,
 * for M21 stage four.
 *
 * Section 10.5 says process termination unblocks every thread of the
 * target exactly as if each had been cancelled individually. Four
 * different things have to happen for that to be true, and until now
 * each was checked on a process that had one thread in one of them.
 * A supervisor ending a driver cohort is not in that position: it
 * ends a process whose threads are wherever they happened to be.
 *
 * The four, and how each is arranged here:
 *
 *   blocked receiving      this thread, once the other three exist
 *   inside a request       the asker, whose message the supervisor
 *                          takes and never answers
 *   running                the spinner, which never enters the kernel
 *                          again and so can only be reached by being
 *                          made to
 *   ready and never run    not made here at all. This program sends
 *                          the supervisor an entry point and a stack
 *                          instead, and the supervisor makes that
 *                          thread itself with dispatch deferred and
 *                          the kill written in the same region.
 *
 * The last of those was made here once, created behind a thread that
 * never yields, and the claim was that it therefore sits on the queue
 * for the life of the process. It does not. A thread preempted by
 * something that outranks it goes to the back of its own level, so
 * the first time the supervisor wakes to look, the spinner lands
 * behind the thread that was never supposed to run, and the next
 * time the supervisor sleeps that thread runs. It was measured at
 * about one run in twenty five. A program at the lowest priority
 * cannot arrange for anything below it, so the arrangement moved to
 * the side that can: the supervisor, which can hold off dispatch
 * altogether.
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
