/* blk_server.c : a RAM-backed block store, served as a program.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * A block store held in the server's own memory, reached over the block
 * protocol. It sits under the filesystem as a second storage layer, so a
 * client read crosses two process boundaries to reach the bytes, one to
 * the filesystem and one to here. There are no device registers.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <string.h>
#include <sys/blk.h>
#include <sys/fs.h>

static uint8_t disk[BLK_BLOCK_COUNT * BLK_BLOCK_BYTES];

/*
 * The bytes the filesystem serves, which live here now and not there.
 * That is the whole of what the second layer buys: a
 * file server with no copy of its own files has to ask for them.
 */
static const char motd_text[] = "served from outside the kernel\n";
static const char alt_motd_text[] = "and this one from the other one\n";
static const char notes_text[] = "only the second directory has this\n";

/*
 * The filesystem above says how long each of these is, out of the same
 * header. If an edit here changes a length, the build stops rather
 * than the check finding it: a file server serving the wrong count is
 * a short read or a read of whatever followed, and neither announces
 * itself.
 */
_Static_assert(sizeof(motd_text) - 1 == BLK_MOTD_BYTES,
  "motd is not the length blk.h says");
_Static_assert(sizeof(alt_motd_text) - 1 == BLK_ALT_MOTD_BYTES,
  "the second motd is not the length blk.h says");
_Static_assert(sizeof(notes_text) - 1 == BLK_NOTES_BYTES,
  "notes is not the length blk.h says");

/*
 * And each has to fit in the blocks between where it starts and where
 * the next one does, or the store would serve one file the tail of
 * another.
 */
_Static_assert(BLK_MOTD_BYTES <=
  (BLK_ALT_MOTD_BLOCK - BLK_MOTD_BLOCK) * BLK_BLOCK_BYTES,
  "motd runs into the second one");
_Static_assert(BLK_ALT_MOTD_BYTES <=
  (BLK_NOTES_BLOCK - BLK_ALT_MOTD_BLOCK) * BLK_BLOCK_BYTES,
  "the second motd runs into notes");
_Static_assert(BLK_NOTES_BLOCK * BLK_BLOCK_BYTES + BLK_NOTES_BYTES <=
  BLK_BLOCK_COUNT * BLK_BLOCK_BYTES,
  "notes runs off the end of the disk");

static const struct {
    uint32_t    block;
    const char *text;
} contents[] = {
    { BLK_MOTD_BLOCK,     motd_text },
    { BLK_ALT_MOTD_BLOCK, alt_motd_text },
    { BLK_NOTES_BLOCK,    notes_text },
};

#define CONTENT_COUNT   (sizeof(contents) / sizeof(contents[0]))

static void
format(void)
{
    uint32_t i;

    for (i = 0; i < CONTENT_COUNT; i++) {
        memcpy(&disk[contents[i].block * BLK_BLOCK_BYTES],
          contents[i].text, strlen(contents[i].text));
    }
}

/*
 * A read of whole blocks, or a refusal saying which rule it broke.
 *
 * Three refusals rather than one. An unaligned offset, a length that is
 * not a whole number of blocks, and a range that runs off the end of
 * the disk are three different mistakes, and a caller told only "no"
 * has to guess which. The first two are INVALID_ARG because the request
 * could never be right; the third is NO_ENT because there is nothing
 * there, which is a fact about this disk and not about the request.
 */
static sys_err_t
read_blocks(void *dst, uint32_t dstlen, uint64_t off, uint32_t len)
{
    if ((off % BLK_BLOCK_BYTES) != 0 || (len % BLK_BLOCK_BYTES) != 0 ||
      len == 0) {
        return SYS_ERR_INVALID_ARG;
    }

    if (off / BLK_BLOCK_BYTES >= BLK_BLOCK_COUNT ||
      off + len > (uint64_t)BLK_BLOCK_COUNT * BLK_BLOCK_BYTES) {
        return SYS_ERR_NO_ENT;
    }

    if (dst == NULL || dstlen < len) {
        return SYS_ERR_TRUNCATED;
    }

    memcpy(dst, &disk[off], len);

    return SYS_OK;
}

int
main(void)
{
    sys_handle_t chan;
    sys_msg_t message, reply;
    sys_thread_id_t peer;
    uint64_t cookie;

    chan = ns_lookup(SYS_NS_SERVICE);
    if (chan == SYS_HANDLE_INVALID) {
        return 1;
    }

    format();

    for (;;) {
        fs_request_t req;
        fs_reply_t rep;
        struct wire_in in;
        struct wire_out out;

        if (sys_msg_recv(chan, &message, &peer, &cookie) != SYS_OK) {
            return 2;
        }

        wire_in_init(&in, message.inline_data, message.inline_len);
        fs_request_get(&in, &req);

        memset(&reply, 0, sizeof(reply));
        reply.handle = SYS_HANDLE_INVALID;
        memset(&rep, 0, sizeof(rep));
        rep.fr_length = 0;
        rep.fr_size = (uint64_t)BLK_BLOCK_COUNT * BLK_BLOCK_BYTES;

        if (req.fr_op != FS_OP_READ) {
            /*
             * A store has no names to walk and nothing to open or
             * close, so every other operation is one this server
             * cannot provide rather than one it declines to. A server
             * answers every operation, and this is the answer.
             */
            rep.fr_status = SYS_ERR_NOT_SUPPORTED;
        } else {
            rep.fr_status = read_blocks((void *)message.buf,
              message.buf_len, req.fr_offset, req.fr_length);
            if (rep.fr_status == SYS_OK) {
                rep.fr_length = req.fr_length;
            }
        }

        wire_out_init(&out, reply.inline_data, SYS_IPC_INLINE_BYTES);
        fs_reply_put(&out, &rep);
        reply.inline_len = sizeof(fs_reply_t);

        /*
         * The same answer where the kernel can read it.
         * A refusal that lives only in the payload is a successful
         * send as far as the kernel is concerned, and a chain linked
         * behind it carries on.
         */
        reply.status = rep.fr_status;

        if (sys_msg_reply(peer, &reply) != SYS_OK) {
            return 3;
        }
    }
}
