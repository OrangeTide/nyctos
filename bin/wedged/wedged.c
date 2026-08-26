/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * A program that blocks, so that something outside can break it out.
 *
 * Section 10.5 says a supervisor never has to know which wait a wedged
 * thread is sitting in, because section 10.6 closes the list of ways to
 * block and each of them leaves behind the one function that takes a
 * thread off again. This is that claim made checkable: the argument
 * chooses the wait, one cancel arrives from outside, and every case has
 * to come back with the same answer.
 *
 * The reply-blocked case is the one worth the effort. A client between
 * the rendezvous and the reply has lent its buffer away: the pages are
 * mapped in the server and gone from here, and section 6.1 gives them
 * back at a reply that is never going to come. So this writes to its
 * buffer the moment it is released, and if the pages did not come home
 * it dies there rather than reporting success.
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <string.h>
#include <sys/fs.h>

/* Where the reply-blocked case lends from, and the mark it writes after. */
static unsigned char lent[64];

#define MARK    0x5a

/* The roles a parent hands this program, per section 20.4. */
#define NS_FENCE    (SYS_NS_PRIVATE + 0)
#define NS_NOBODY   (SYS_NS_PRIVATE + 1)
#define NS_WEDGED   (SYS_NS_PRIVATE + 2)

static sys_err_t
send_to(sys_handle_t to, int with_buffer)
{
    fs_request_t *req;
    sys_msg_t message, reply;

    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);
    if (with_buffer) {
        message.buf = (uintptr_t)lent;
        message.buf_len = sizeof(lent);
        message.buf_flags = SYS_BUF_WRITE;
    }
    req = (fs_request_t *)message.inline_data;
    req->fr_op = FS_OP_READ;
    req->fr_offset = 0;
    req->fr_length = sizeof(lent);

    return sys_msg_send(to, &message, &reply);
}

static int
same(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

int
main(int argc, char **argv)
{
    sys_msg_t message;
    sys_thread_id_t peer;
    uint64_t cookie;
    sys_handle_t h;
    sys_err_t err;

    if (argc < 2) {
        return 1;
    }

    memset(lent, 0, sizeof(lent));

    if (same(argv[1], "recv")) {
        h = ns_lookup(SYS_NS_SERVICE);
        if (h == SYS_HANDLE_INVALID) {
            return 2;
        }
        err = sys_msg_recv(h, &message, &peer, &cookie);
    } else if (same(argv[1], "fence")) {
        h = ns_lookup(NS_FENCE);
        if (h == SYS_HANDLE_INVALID) {
            return 2;
        }
        err = sys_fence_wait(h, 1, SYS_DEADLINE_NEVER);
    } else if (same(argv[1], "send")) {
        h = ns_lookup(NS_NOBODY);
        if (h == SYS_HANDLE_INVALID) {
            return 2;
        }
        err = send_to(h, 0);
    } else if (same(argv[1], "reply")) {
        h = ns_lookup(NS_WEDGED);
        if (h == SYS_HANDLE_INVALID) {
            return 2;
        }
        err = send_to(h, 1);

        /*
         * Its own memory, the instant it is running again. If the pages
         * are still in the server this writes into a hole and the
         * program stops here rather than returning zero.
         */
        lent[0] = MARK;
        lent[sizeof(lent) - 1] = MARK;
        if (lent[0] != MARK || lent[sizeof(lent) - 1] != MARK) {
            return 3;
        }
    } else {
        return 1;
    }

    return err == SYS_ERR_INTERRUPTED ? 0 : 4;
}
