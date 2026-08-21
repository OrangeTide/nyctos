/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * A program's own input and output, over the console service of section
 * 19 and the protocol of section 25.
 *
 * Until this stage write() went to the debug console of section 22.4 and
 * read() did not exist, which meant a program's output was ambient
 * authority and an image built without the backdoor could not talk to a
 * person at all. Now both ends are a connection the process was handed at
 * spawn, under SYS_NS_CONSOLE, and a process that was given no console
 * reaches none: section 20.1 has authority be a handle, and this is what
 * that means for the most ordinary thing a program does.
 *
 * The debug macros in <debug.h> keep their own spelling and their own
 * build switch, per section 22.4, because a program's real output and its
 * diagnostics are different categories and only the second is one a build
 * removes wholesale.
 *
 * There are still no file descriptors. The three POSIX numbers all name
 * the same connection, because there is one console and nothing else to
 * open; the descriptor table arrives with the mount table in M5.
 */

#include <os/sys.h>
#include <ns.h>
#include <string.h>
#include <sys/fs.h>
#include <unistd.h>

/*
 * Looked up once and kept. The lookup is a walk over eight words, which is
 * nothing, but a program that prints in a loop should not repeat it and
 * the handle cannot change: nothing in this system replaces a namespace
 * entry after spawn.
 */
static sys_handle_t console = SYS_HANDLE_INVALID;
static int looked_up;

static sys_handle_t
console_handle(void)
{
    if (!looked_up) {
        console = ns_lookup(SYS_NS_CONSOLE);
        looked_up = 1;
    }

    return console;
}

/*
 * The most a request can carry inline. Bulk data belongs in the message
 * buffer, per section 25.5, and a console line is not bulk data; a caller
 * with more to say makes more calls, exactly as the debug console makes
 * it split at its own bound.
 */
#define CONSOLE_CHUNK   (SYS_IPC_INLINE_BYTES - sizeof(fs_request_t))

ssize_t
write(int fd, const void *buf, size_t nbytes)
{
    sys_handle_t conn = console_handle();
    const char *p = buf;
    size_t done = 0;

    if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        return -1;
    }
    if (conn == SYS_HANDLE_INVALID) {
        return -1;
    }

    while (done < nbytes) {
        sys_msg_t request, reply;
        fs_request_t *req;
        fs_reply_t *rep;
        size_t n = nbytes - done;

        if (n > CONSOLE_CHUNK) {
            n = CONSOLE_CHUNK;
        }

        memset(&request, 0, sizeof(request));
        req = (fs_request_t *)request.inline_data;
        req->fr_op = FS_OP_WRITE;
        req->fr_length = (uint32_t)n;
        req->fr_offset = 0;
        memcpy(&request.inline_data[sizeof(*req)], p + done, n);
        request.inline_len = (uint32_t)(sizeof(*req) + n);

        if (sys_msg_send(conn, &request, &reply) != SYS_OK) {
            return done == 0 ? -1 : (ssize_t)done;
        }
        rep = (fs_reply_t *)reply.inline_data;
        if (rep->fr_status != SYS_OK) {
            return done == 0 ? -1 : (ssize_t)done;
        }

        /*
         * A short write is the server's to report and the caller's to
         * carry on from, which is why this loops on what was taken rather
         * than on what was offered.
         */
        if (rep->fr_length == 0) {
            break;
        }
        done += rep->fr_length;
    }

    return (ssize_t)done;
}

/*
 * One read, one reply. The console answers with at most one line and
 * blocks until it has one, which is what makes this look like a read from
 * a terminal without anything here knowing what a line is.
 */
ssize_t
read(int fd, void *buf, size_t nbytes)
{
    sys_handle_t conn = console_handle();
    sys_msg_t request, reply;
    fs_request_t *req;
    fs_reply_t *rep;
    uint32_t n;

    if (fd != STDIN_FILENO) {
        return -1;
    }
    if (conn == SYS_HANDLE_INVALID) {
        return -1;
    }
    if (nbytes == 0) {
        return 0;
    }

    memset(&request, 0, sizeof(request));
    req = (fs_request_t *)request.inline_data;
    req->fr_op = FS_OP_READ;
    req->fr_length = nbytes > CONSOLE_CHUNK ? CONSOLE_CHUNK
      : (uint32_t)nbytes;
    request.inline_len = sizeof(*req);

    if (sys_msg_send(conn, &request, &reply) != SYS_OK) {
        return -1;
    }
    rep = (fs_reply_t *)reply.inline_data;
    if (rep->fr_status != SYS_OK) {
        return -1;
    }

    n = rep->fr_length;
    if (n > nbytes) {
        n = (uint32_t)nbytes;
    }
    memcpy(buf, &reply.inline_data[sizeof(*rep)], n);

    return (ssize_t)n;
}
