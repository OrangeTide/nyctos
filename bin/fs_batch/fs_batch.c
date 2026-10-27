/* fs_batch.c : reads a file as one compound request.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * It walks to a file and reads it in a single message, using the compound
 * request of the filesystem protocol. The server performs the walk and then
 * the read, and sends one reply. fs_client makes the same read as three
 * separate requests.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <debug.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/fs.h>

static char buf[256];

int
main(void)
{
    fs_batch_t hdr;
    fs_entry_t walk_e, read_e;
    fs_batch_reply_t rep;
    struct wire_out out;
    struct wire_in in;
    sys_msg_t message, reply;
    sys_handle_t dir;

    dir = ns_lookup(SYS_NS_ROOT);
    if (dir == SYS_HANDLE_INVALID) {
        return 1;
    }

    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.buf = (uintptr_t)buf;
    message.buf_len = sizeof(buf);
    message.buf_flags = SYS_BUF_WRITE;

    memset(&hdr, 0, sizeof(hdr));
    hdr.fb_op = FS_OP_BATCH;
    hdr.fb_count = 2;

    memset(&walk_e, 0, sizeof(walk_e));
    walk_e.fe_op = FS_OP_WALK;
    walk_e.fe_namelen = (uint16_t)strlen("motd");

    memset(&read_e, 0, sizeof(read_e));
    read_e.fe_op = FS_OP_READ;
    read_e.fe_length = sizeof(buf);

    wire_out_init(&out, message.inline_data, SYS_IPC_INLINE_BYTES);
    fs_batch_put(&out, &hdr);
    fs_entry_put(&out, &walk_e, "motd");
    fs_entry_put(&out, &read_e, NULL);
    message.inline_len = out.len;

    if (sys_msg_send(dir, &message, &reply) != SYS_OK) {
        return 2;
    }

    wire_in_init(&in, reply.inline_data, reply.inline_len);
    fs_batch_reply_get(&in, &rep);
    DBG_PUTS("fs_batch: compound replied");
    if (!in.ok || rep.fb_status != SYS_OK) {
        return 3;
    }
    if (rep.fb_completed != hdr.fb_count) {
        return 4;
    }

    size_t len = rep.fb_length < sizeof(buf) ? rep.fb_length : sizeof(buf);

    puts("fs_batch read:");
    (void)write(STDOUT_FILENO, buf, len);

    return (int)rep.fb_length;
}
