/* file.c : opens, reads, and closes files on a mounted file server.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * file_open resolves the path in the namespace and sends a walk request to
 * each candidate mount until one succeeds. file_read and file_close send
 * read and close requests over the returned handle, and file_close also
 * closes that handle. file_read is limited to what fits in one inline reply,
 * and message layouts come from the fs protocol in sys/fs.h.
 *
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
    fs_request_t req;
    fs_reply_t rep;
    struct wire_out out;
    struct wire_in in;
    sys_msg_t message;
    sys_err_t err;

    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;

    memset(&req, 0, sizeof(req));
    req.fr_op = op;
    req.fr_offset = off;
    req.fr_length = len;
    /*
     * Bounded here rather than by the caller. The field is the
     * protocol's, so a copy
     * into it that trusts whoever called is a copy that is correct
     * only while every caller stays correct.
     */
    if (name != NULL) {
        size_t n = strlen(name);

        if (n + 1 > FS_NAME_MAX) {
            return SYS_ERR_INVALID_ARG;
        }
        memcpy(req.fr_name, name, n + 1);
    }

    wire_out_init(&out, message.inline_data, SYS_IPC_INLINE_BYTES);
    fs_request_put(&out, &req);
    message.inline_len = sizeof(fs_request_t);

    err = sys_msg_send(target, &message, reply);
    if (err != SYS_OK) {
        return err;
    }

    wire_in_init(&in, reply->inline_data, reply->inline_len);
    fs_reply_get(&in, &rep);
    if (!in.ok) {
        return SYS_ERR_TRUNCATED;
    }

    return rep.fr_status;
}

sys_err_t
file_open(const char *path, sys_handle_t *out)
{
    char rest[NS_PATH_MAX];
    sys_handle_t dir;
    sys_msg_t reply;
    unsigned int nth;
    sys_err_t stumbled = SYS_ERR_NO_ENT;
    sys_err_t err = SYS_ERR_NO_ENT;

    if (out == NULL) {
        return SYS_ERR_INVALID_ARG;
    }

    /*
     * Every handle mounted at the name, in the order the parent wrote
     * them, until one answers. That is a union directory and this loop is
     * all of it: it needs no kernel involvement, and
     * what it needs instead is a caller willing to ask twice.
     *
     * What comes back is one failure rather than one per handle. Which
     * of them did not have the file is this loop's business and not the
     * caller's, and a program told that a name was missing from three
     * directories has learned about the namespace rather than about the
     * name.
     */
    for (nth = 0; ; nth++) {
        err = ns_resolve_at(path, nth, &dir, rest, sizeof(rest));
        if (err != SYS_OK) {
            /*
             * Out of candidates. What comes back is the first thing
             * that went wrong other than a directory not having the
             * file, because those are different questions: a name
             * that is in none of three directories is missing, and a
             * name that is in none of two because the third could
             * not be asked is a namespace with something wrong in
             * it. Reporting the second as the first tells a program
             * its file does not exist when its mount is broken.
             */
            return nth == 0 ? err : stumbled;
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
         * The protocol's name is shorter than a path the resolver will
         * hold, which is a real bound and not a library one: a name too
         * long for the message cannot be sent, and truncating it would
         * open a different file.
         */
        if (strlen(rest) + 1 > FS_NAME_MAX) {
            return SYS_ERR_INVALID_ARG;
        }

        err = request(dir, FS_OP_WALK, rest, 0, 0, &reply);
        if (err == SYS_OK) {
            break;
        }
        if (err != SYS_ERR_NO_ENT && stumbled == SYS_ERR_NO_ENT) {
            stumbled = err;
        }
    }

    /*
     * A walk that says it worked and hands back nothing is a server this
     * library cannot use. Reported here rather than left to fail on the
     * first read, where it would look like the file going away rather
     * than never having been opened.
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
    fs_reply_t rep;
    struct wire_in in;
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

    wire_in_init(&in, reply.inline_data, reply.inline_len);
    fs_reply_get(&in, &rep);
    if (!in.ok) {
        return SYS_ERR_TRUNCATED;
    }
    *got = rep.fr_length > len ? len : rep.fr_length;
    memcpy(buf, &reply.inline_data[sizeof(fs_reply_t)], *got);

    return SYS_OK;
}

sys_err_t
file_close(sys_handle_t file)
{
    sys_msg_t reply;
    sys_err_t err;

    /*
     * The close is what returns the server's per-connection record to
     * its pool. Destroying the handle locally would
     * tell the server nothing, so both happen and in that order.
     */
    err = request(file, FS_OP_CLOSE, NULL, 0, 0, &reply);
    (void)sys_handle_close(file);

    return err;
}
