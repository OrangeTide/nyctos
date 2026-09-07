/*
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * A client that lends a buffer, gives up, and then uses it again.
 *
 * M15 stage four is about what happens to memory a client lent to a
 * server it stopped waiting for. Section 6.1 gives the server the right
 * to the buffer for the length of the call, and at Tier 2 that is a
 * real mapping: the pages leave this program's address space and come
 * back when the reply does. A client that gives up instead has to get
 * them back the same way, with the permissions they had, or it runs on
 * afterwards with a hole where its memory was.
 *
 * The deadline arrives as an argument because a program has no way to
 * read the clock. Section 14.3 has every blocking call take an absolute
 * deadline and section 6.2 says what makes that affordable, a shared
 * page a program reads without a syscall, and that page does not exist.
 * So the kernel that starts this computes the instant and passes it,
 * which is what `paged` does with the bounds of its own image and is
 * the same admission: an interface for a test would be worse.
 */

#include <os/sys.h>
#include <ns.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PATTERN 0x5eed1234u

static volatile uint32_t buf[64];

static int
hex(const char *s, uint64_t *out)
{
    uint64_t v = 0;

    if (*s == '\0') {
        return -1;
    }
    for (; *s != '\0'; s++) {
        uint64_t d;

        if (*s >= '0' && *s <= '9') {
            d = (uint64_t)(*s - '0');
        } else if (*s >= 'a' && *s <= 'f') {
            d = (uint64_t)(*s - 'a') + 10u;
        } else if (*s >= 'A' && *s <= 'F') {
            d = (uint64_t)(*s - 'A') + 10u;
        } else if (*s == 'x' || *s == 'X') {
            continue;
        } else {
            return -1;
        }
        v = (v << 4) | d;
    }

    *out = v;

    return 0;
}

int
main(int argc, char **argv)
{
    sys_op_send_args_t args;
    sys_handle_t to;
    sys_op_t send;
    uint64_t deadline;
    uint32_t i;

    if (argc != 2 || hex(argv[1], &deadline) != 0) {
        return 1;
    }

    to = ns_lookup(SYS_NS_SERVICE);
    if (to == SYS_HANDLE_INVALID) {
        return 2;
    }

    for (i = 0; i < 64; i++) {
        buf[i] = 0;
    }

    memset(&args, 0, sizeof(args));
    args.request.handle = SYS_HANDLE_INVALID;
    args.request.inline_len = 0;
    args.request.buf = (uintptr_t)buf;
    args.request.buf_len = sizeof(buf);
    args.request.buf_flags = SYS_BUF_WRITE;
    args.deadline = deadline;

    memset(&send, 0, sizeof(send));
    send.opcode = SYS_OP_IPC_SEND;
    send.target = to;
    send.flags = SYS_OP_DEADLINE;
    send.arg_ptr = (uintptr_t)&args;
    send.arg_len = sizeof(args);

    if (sys_op_submit(&send, 1) != SYS_ERR_TIMEOUT) {
        return 3;
    }

    /*
     * And the memory is this program's again. Writing it is the half
     * that matters: a page handed to a server comes back read-only if
     * whatever put it back asked the wrong process what it should be,
     * and this program would fault here rather than return.
     */
    for (i = 0; i < 64; i++) {
        buf[i] = PATTERN + i;
    }
    for (i = 0; i < 64; i++) {
        if (buf[i] != PATTERN + i) {
            return 4;
        }
    }

    printf("patient: wrote and read %u words of its own buffer\n", i);

    return 0;
}
