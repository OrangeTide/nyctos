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
 *   ready and never run    the idler, created last and behind a
 *                          thread that never yields, so it sits on
 *                          the queue for the life of the process
 *
 * The order matters and is the whole construction. Threads run at
 * one priority and this kernel does not slice time, so a queue at
 * one priority runs in the order it was filled and each entry runs
 * until it blocks. The asker blocks, the spinner does not, and the
 * idler is behind the spinner.
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
 * Never runs. Created last, and the kill is written before the
 * scheduler gets to it, which is the state stage two added a
 * reading for.
 *
 * It receives on a channel nothing sends to rather than computing,
 * so that a run where the trampoline's reading is taken out says
 * where the thread was caught instead. Measured both ways: with
 * the reading gone this thread still ends, because it makes an
 * entry and the reading at the entry boundary catches it there.
 * The report says as much, one fewer killed before ever running
 * and one more stopped at an entry boundary. A body that computed
 * would also still end, reached by whatever interrupt happened
 * along, and the report would show only the first of those two.
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

    if (sys_thread_create(idler, NULL, stack_idle, STACK_BYTES,
      SYS_PRIO_INHERIT, &th) != SYS_OK) {
        return ERR_CREATE;
    }
    (void)sys_handle_close(th);

    memset(&message, 0, sizeof(message));
    (void)sys_msg_recv(serve, &message, &peer, &cookie);

    return ERR_RETURN;
}
