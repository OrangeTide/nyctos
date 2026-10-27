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

/* Append an entry and its name, and return the new payload length. */
static uint32_t
put(uint8_t *p, uint32_t off, uint8_t op, const char *name, uint64_t offset,
  uint32_t length)
{
    uint32_t namelen = name != NULL ? (uint32_t)strlen(name) : 0;
    fs_entry_t *e = (fs_entry_t *)&p[off];

    memset(e, 0, sizeof(*e));
    e->fe_op = op;
    e->fe_namelen = (uint16_t)namelen;
    e->fe_offset = offset;
    e->fe_length = length;

    off += (uint32_t)sizeof(*e);
    if (namelen != 0) {
        memcpy(&p[off], name, namelen);
        memset(&p[off + namelen], 0, FS_ENTRY_PAD(namelen) - namelen);
        off += FS_ENTRY_PAD(namelen);
    }

    return off;
}

int
main(void)
{
    const fs_batch_reply_t *rep;
    sys_msg_t message, reply;
    sys_handle_t dir;
    fs_batch_t *hdr;
    uint32_t off;

    dir = ns_lookup(SYS_NS_ROOT);
    if (dir == SYS_HANDLE_INVALID) {
        return 1;
    }

    memset(&message, 0, sizeof(message));
    message.handle = SYS_HANDLE_INVALID;
    message.buf = (uintptr_t)buf;
    message.buf_len = sizeof(buf);
    message.buf_flags = SYS_BUF_WRITE;

    hdr = (fs_batch_t *)message.inline_data;
    hdr->fb_op = FS_OP_BATCH;
    hdr->fb_count = 2;

    off = put(message.inline_data, sizeof(*hdr), FS_OP_WALK, "motd", 0, 0);
    off = put(message.inline_data, off, FS_OP_READ, NULL, 0, sizeof(buf));
    message.inline_len = off;

    if (sys_msg_send(dir, &message, &reply) != SYS_OK) {
        return 2;
    }

    rep = (const fs_batch_reply_t *)reply.inline_data;
    DBG_PUTS("fs_batch: compound replied");
    if (rep->fb_status != SYS_OK) {
        return 3;
    }
    if (rep->fb_completed != hdr->fb_count) {
        return 4;
    }

    size_t len = rep->fb_length < sizeof(buf) ? rep->fb_length : sizeof(buf);

    puts("fs_batch read:");
    (void)write(STDOUT_FILENO, buf, len);

    return (int)rep->fb_length;
}
