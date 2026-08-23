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
    /*
     * Bounded here rather than by the caller. The field is the
     * protocol's and its size is section 25's business, so a copy
     * into it that trusts whoever called is a copy that is correct
     * only while every caller stays correct.
     */
    if (name != NULL) {
        size_t n = strlen(name);

        if (n + 1 > FS_NAME_MAX) {
            return SYS_ERR_INVALID_ARG;
        }
        memcpy(req->fr_name, name, n + 1);
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
     * A path that names a mount point exactly names a handle this
     * process already holds, and there is nothing to walk to.
     *
     * Refused rather than handed back, and the reason is ownership
     * rather than type. The handle is the process's own namespace
     * entry: returning it would give a caller something that closing
     * would close the mount, and a program that opened its own root
     * twice would lose it. What a mount point is for is resolving
     * paths below it.
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

    /*
     * A walk that says it worked and hands back nothing is a server
     * this library cannot use. Reported here rather than left to
     * fail on the first read, where it would look like the file
     * going away rather than never having been opened.
     */
    if (reply.handle == SYS_HANDLE_INVALID) {
        return SYS_ERR_STALE;
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
