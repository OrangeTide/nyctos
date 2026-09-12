/*
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Two requests in one submission, neither of them answered, for M21
 * stage three.
 *
 * The stage reads the kill mark before an entry runs rather than
 * after it. What that has to be shown on is a thread that is at an
 * entry boundary with the mark set, and the way to put one there is
 * to have it blocked inside the entry before the boundary and let
 * the kill release it.
 *
 * So the first request goes to a supervisor that will never answer
 * it, and this waits. The kill cancels that wait, the first entry
 * comes back interrupted, and the second entry is the one that must
 * never begin. Neither request has a deadline: what ends this
 * program is the kill, and a deadline would give it another way out
 * and make the check pass for the wrong reason.
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
