/* patient.c : a client that lends a buffer, gives up, and reuses it.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * It lends a 64-word buffer to a server that never answers, using a send
 * with a 50 millisecond deadline. Once the send times out, it checks that
 * the clock has reached the deadline, then writes a pattern to the buffer
 * and reads it back. The program exits with 0 only if the buffer's pages
 * returned and the program could use them again.
 *
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
     * And it is not before the instant this asked for. The check
     * outside measures around the spawn as well as the send, so its
     * floor would still be met by a program that never waited at all
     * if a load ever took as long as a deadline. This is the same
     * question asked where the answer is exact: one absolute instant,
     * compared with the clock, with no interval to get wrong.
     */
    if (sys_uptime() < deadline) {
        return 5;
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
