/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Opening a file by name.
 *
 * This is where the two halves of section 20 meet. The resolver turns a
 * path into a handle the process already holds and the part of the path
 * below it; this turns that into a message. Nothing here knows what a
 * namespace is and nothing in the resolver knows what a file is, which
 * is the division the section asks for: naming is the library's and the
 * protocol is section 25's.
 *
 * The protocol is unchanged and was always shaped this way. A walk has
 * always carried a handle and a relative name, because a message to a
 * server could never carry anything else; what changed is that the name
 * now comes out of a resolver rather than out of the caller's hand.
 *
 * A program may still build these messages itself, and three of them do.
 * What this buys is that the ones which only want the bytes no longer
 * have to.
 */

#include <os/sys.h>
#include <ns.h>
#include <file.h>

#include <stddef.h>
#include <string.h>
#include <sys/fs.h>

/*
 * One request on a connection, with the reply left where the caller can
 * read the protocol's own status out of it.
 */
static sys_err_t
request(sys_handle_t target, uint32_t op, const char *name, uint64_t off,
  uint32_t len, sys_msg_t *reply)
{
    fs_request_t *req;
    sys_msg_t message;
    sys_err_t err;

    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.inline_len = sizeof(*req);

    req = (fs_request_t *)message.inline_data;
    req->fr_op = op;
    req->fr_offset = off;
    req->fr_length = len;
    if (name != NULL) {
        memcpy(req->fr_name, name, strlen(name) + 1);
    }

    err = sys_msg_send(target, &message, reply);
    if (err != SYS_OK) {
        return err;
    }

    return ((const fs_reply_t *)reply->inline_data)->fr_status;
}

sys_err_t
file_open(const char *path, sys_handle_t *out)
{
    char rest[NS_PATH_MAX];
    sys_handle_t dir;
    sys_msg_t reply;
    sys_err_t err;

    if (out == NULL) {
        return SYS_ERR_INVALID_ARG;
    }

    err = ns_resolve(path, &dir, rest, sizeof(rest));
    if (err != SYS_OK) {
        return err;
    }

    /*
     * A path that names a mount point and nothing below it names the
     * directory the process already holds, and there is nothing to walk
     * to. Opening one is not a file operation, so it is refused here
     * rather than sent to a server that would have to say the same.
     */
    if (rest[0] == '\0') {
        return SYS_ERR_WRONG_TYPE;
    }

    /*
     * The protocol's name is shorter than a path the resolver will hold,
     * which is a real bound and not a library one: a name too long for
     * the message cannot be sent, and truncating it would open a
     * different file.
     */
    if (strlen(rest) + 1 > FS_NAME_MAX) {
        return SYS_ERR_INVALID_ARG;
    }

    err = request(dir, FS_OP_WALK, rest, 0, 0, &reply);
    if (err != SYS_OK) {
        return err;
    }

    *out = reply.handle;

    return SYS_OK;
}

sys_err_t
file_read(sys_handle_t file, void *buf, uint32_t len, uint64_t off,
  uint32_t *got)
{
    const fs_reply_t *rep;
    sys_msg_t reply;
    uint32_t max = SYS_IPC_INLINE_BYTES - sizeof(fs_reply_t);
    sys_err_t err;

    if (buf == NULL || got == NULL) {
        return SYS_ERR_INVALID_ARG;
    }
    if (len > max) {
        len = max;
    }

    err = request(file, FS_OP_READ, NULL, off, len, &reply);
    if (err != SYS_OK) {
        return err;
    }

    rep = (const fs_reply_t *)reply.inline_data;
    *got = rep->fr_length > len ? len : rep->fr_length;
    memcpy(buf, &reply.inline_data[sizeof(*rep)], *got);

    return SYS_OK;
}

sys_err_t
file_close(sys_handle_t file)
{
    sys_msg_t reply;
    sys_err_t err;

    /*
     * The close is what returns the server's per-connection record to
     * its pool, per section 25.2. Destroying the handle locally would
     * tell the server nothing, so both happen and in that order.
     */
    err = request(file, FS_OP_CLOSE, NULL, 0, 0, &reply);
    (void)sys_handle_close(file);

    return err;
}
