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
 * The deadline is this program's own, as of M16 stage four. Section
 * 14.3 has every blocking call take an absolute instant, and section
 * 6.2 is what makes one affordable: the counter, read with no system
 * call, and the frequency from the page mapped into every process. It
 * arrived as an argument until then, because the kernel that started
 * this was the only thing here that could read a clock.
 *
 * How long it waits is written here and nowhere else, and the check
 * outside knows the same number as the floor it asserts. A shorter
 * wait here and the check sees a program back before its floor. A
 * longer one it does not see: the holder never answers, so this waits
 * out whatever it asked for and the boot merely takes longer.
 */

#include <os/sys.h>
#include <ns.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PATTERN 0x5eed1234u

/* The same fifty milliseconds the check outside calls DEADLINE_MS. */
#define WAIT_MS 50u

static volatile uint32_t buf[64];

int
main(void)
{
    sys_op_send_args_t args;
    sys_handle_t to;
    sys_op_t send;
    uint64_t deadline;
    uint32_t i;

    /*
     * Asked for before anything else, because a program with no page
     * has no frequency and would get SYS_DEADLINE_NOW: a send that
     * returns the right error for the wrong reason, having waited for
     * nothing at all.
     */
    if (sys_uptime_hz() == 0) {
        return 1;
    }
    deadline = sys_deadline_in_ms(WAIT_MS);

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
