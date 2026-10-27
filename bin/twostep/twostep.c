/* twostep.c : submits two requests in one batch, neither answered.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * The first request goes to a server that never replies, so the batch blocks
 * inside its first entry. A kill delivered from outside cancels that wait and
 * interrupts the entry, and the second entry must not begin. The program exits
 * with 5 if the batch runs to its end. Neither request has a deadline, so the
 * kill is the only way out.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Returned when the batch ran to its end, which must not happen. */
#define RAN_ON 5

int
main(void)
{
    sys_handle_t chan;
    sys_op_send_args_t first, second;
    sys_op_t ops[2];

    chan = ns_lookup(SYS_NS_SERVICE);
    if (chan == SYS_HANDLE_INVALID) {
        return 1;
    }

    memset(&first, 0, sizeof(first));
    first.request.handle = SYS_HANDLE_INVALID;
    first.request.inline_len = 1;

    memset(&second, 0, sizeof(second));
    second.request.handle = SYS_HANDLE_INVALID;
    second.request.inline_len = 1;

    memset(ops, 0, sizeof(ops));
    ops[0].opcode = SYS_OP_IPC_SEND;
    ops[0].target = chan;
    ops[0].arg_ptr = (uintptr_t)&first;
    ops[0].arg_len = sizeof(first);

    ops[1].opcode = SYS_OP_IPC_SEND;
    ops[1].target = chan;
    ops[1].arg_ptr = (uintptr_t)&second;
    ops[1].arg_len = sizeof(second);

    (void)sys_op_submit(ops, 2);

    return RAN_ON;
}
