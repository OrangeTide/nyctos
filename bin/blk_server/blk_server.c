/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * A block store, as an ordinary program.
 *
 * It is a RAM disk, which is the honest thing to build here: section 19
 * says a driver is a process holding a grant over its registers and an
 * interrupt handle, and neither of those is what this milestone is
 * about. What matters for section 18 is that there is a second layer
 * under the filesystem and that a request crosses two boundaries to
 * reach the bytes. A driver with real registers would cross the same
 * two.
 *
 * It serves reads in whole blocks and refuses anything else. That is
 * what makes it a block store rather than a second byte server: the
 * layer above has to do the block arithmetic, which is the work a
 * filesystem exists to do and the reason the two layers are two.
 *
 * It never exits, for the reason every server here never exits: an exit
 * nobody asked about halts the machine.
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
 * That is the whole of what the second layer buys this milestone: a
 * file server with no copy of its own files has to ask for them.
 */
static const struct {
    uint32_t    block;
    const char *text;
} contents[] = {
    { BLK_MOTD_BLOCK,     "served from outside the kernel\n" },
    { BLK_ALT_MOTD_BLOCK, "and this one from the other one\n" },
    { BLK_NOTES_BLOCK,    "only the second directory has this\n" },
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
        const fs_request_t *req;
        fs_reply_t *rep;

        if (sys_msg_recv(chan, &message, &peer, &cookie) != SYS_OK) {
            return 2;
        }

        req = (const fs_request_t *)message.inline_data;

        memset(&reply, 0, sizeof(reply));
        reply.handle = SYS_HANDLE_INVALID;
        reply.inline_len = sizeof(*rep);
        rep = (fs_reply_t *)reply.inline_data;
        rep->fr_length = 0;
        rep->fr_size = (uint64_t)BLK_BLOCK_COUNT * BLK_BLOCK_BYTES;

        if (req->fr_op != FS_OP_READ) {
            /*
             * A store has no names to walk and nothing to open or
             * close, so every other operation is one this server
             * cannot provide rather than one it declines to. Section
             * 25.2's rule is that a server answers every operation,
             * and this is the answer.
             */
            rep->fr_status = SYS_ERR_NOT_SUPPORTED;
        } else {
            rep->fr_status = read_blocks((void *)message.buf,
              message.buf_len, req->fr_offset, req->fr_length);
            if (rep->fr_status == SYS_OK) {
                rep->fr_length = req->fr_length;
            }
        }

        if (sys_msg_reply(peer, &reply) != SYS_OK) {
            return 3;
        }
    }
}
