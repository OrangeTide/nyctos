/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * A client of a server that goes away, per section 12.5.
 *
 * It sends to a channel whose owning process is about to terminate and
 * expects to be failed rather than told: a client is not at a receive
 * point, so there is nothing it could collect, and the two sides of a
 * rendezvous therefore need different mechanisms. Neither depends on a
 * deadline, which is what makes this usable during a cohort restart
 * where a supervisor is deliberately destroying servers with clients
 * waiting on them.
 *
 * A separate program from `wedged` and not another case in it, because
 * at Tier 0 every image is loaded at the address it was linked for and
 * two live copies of one image would be one program written over
 * another. The check needs the client and the server it loses to be
 * running at the same time.
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <string.h>
#include <sys/fs.h>

int
main(void)
{
    fs_request_t *req;
    sys_msg_t message, reply;
    sys_handle_t to;

    to = ns_lookup(SYS_NS_PRIVATE + 1);
    if (to == SYS_HANDLE_INVALID) {
        return 1;
    }

    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);
    req = (fs_request_t *)message.inline_data;
    req->fr_op = FS_OP_READ;

    return sys_msg_send(to, &message, &reply) == SYS_ERR_OWNER_DEAD ? 0 : 2;
}
