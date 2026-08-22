/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * A program that describes memory it does not own, and lives.
 *
 * Section 23.2 says the kernel validates what a program hands it before
 * an operation executes. A buffer descriptor is the case that matters:
 * what crosses the boundary is a number, and if nothing checks it the
 * kernel reads whatever it points at on the caller's behalf. That is not
 * a Tier 2 question. At Tier 0 the read succeeds and hands one program
 * another program's memory; at Tier 2 the kernel faults trying to map a
 * page it was never given. Both are wrong in the same way, which is why
 * the refusal is in the operation and not in the tier.
 *
 * The exit status is the report, because a program with no console
 * handle has nowhere to say anything. Zero is both refusals happening;
 * anything else names the step it stopped at.
 *
 * The reply is the point. A refused descriptor is an error returned to a
 * program that is still running, not a dead process, so this exits zero
 * after being told no twice.
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>

#include <string.h>
#include <sys/fs.h>

static char buf[256];

/*
 * Ask for a file's bytes into a named address. Everything but the buffer
 * is what any client would send.
 */
static sys_err_t
read_into(sys_handle_t target, uintptr_t addr, uint32_t len)
{
    fs_request_t *req;
    sys_msg_t message;
    sys_msg_t reply;

    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);
    message.buf = addr;
    message.buf_len = len;
    message.buf_flags = SYS_BUF_WRITE;

    req = (fs_request_t *)message.inline_data;
    req->fr_op = FS_OP_READ;
    req->fr_offset = 0;
    req->fr_length = len;

    return sys_msg_send(target, &message, &reply);
}

int
main(void)
{
    fs_request_t *req;
    sys_msg_t message;
    sys_msg_t reply;
    sys_handle_t dir, file;
    uintptr_t neighbour;

    dir = ns_lookup(SYS_NS_ROOT);
    if (dir == SYS_HANDLE_INVALID) {
        return 1;
    }

    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);
    req = (fs_request_t *)message.inline_data;
    req->fr_op = FS_OP_WALK;
    memcpy(req->fr_name, "motd", sizeof("motd"));
    if (sys_msg_send(dir, &message, &reply) != SYS_OK) {
        return 2;
    }
    file = reply.handle;

    /* This program's own memory, which is the ordinary case. */
    if (read_into(file, (uintptr_t)buf, sizeof(buf)) != SYS_OK) {
        return 3;
    }

    /*
     * The next slot up, which belongs to whatever the loader put there
     * and never to this program. One slot is the granularity the loader
     * works in, so an address a slot away is another program's memory
     * whichever slot this one landed in.
     */
    neighbour = ((uintptr_t)buf & ~((uintptr_t)SYS_USER_SLOT_BYTES - 1)) +
      SYS_USER_SLOT_BYTES;
    if (read_into(file, neighbour, sizeof(buf)) != SYS_ERR_FAULT) {
        return 4;
    }

    /* And an address that straddles the end of this program's slot. */
    if (read_into(file, (uintptr_t)buf, SYS_USER_SLOT_BYTES) !=
      SYS_ERR_FAULT) {
        return 5;
    }

    return 0;
}
