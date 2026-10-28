/* virtio_blk.c : a user-space virtio-mmio block driver.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Holds a grant over the machine's virtio-mmio transport slots, scans them
 * for the block device, brings it up over one split virtqueue, and drives it.
 * A block device reached this way is a real one behind the block protocol the
 * RAM store used to answer, so a file read reaches bytes on an emulated disk
 * rather than in a server's array.
 *
 * The device reaches the queue and buffers by their addresses as the machine
 * sees them, which are physical. A program's ordinary memory is not mapped at
 * its physical address, so the queue and buffers live in a second grant, a
 * page of memory the boot hands this driver. A grant is mapped at the address
 * it covers, so an address in that page is the physical address the device
 * reads, and the queue needs no translation.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <debug.h>
#include <stddef.h>
#include <string.h>
#include <sys/blk.h>
#include <sys/fs.h>
#include <sys/virtio.h>

/*
 * The driver's DMA page, the device's interrupt, and a handle to wait on it
 * are handed over under these private namespace ids.
 */
#define NS_DMA      (SYS_NS_PRIVATE + 0)
#define NS_IRQ      (SYS_NS_PRIVATE + 1)
#define NS_FENCE    (SYS_NS_PRIVATE + 2)

#define QUEUE_SIZE  8u

/* One mapped slot: the base address of its register window. */
static uintptr_t slot_base;

/*
 * The virtqueue and the buffers one read uses, laid out in the granted DMA
 * page. Modern virtio-mmio takes the three rings at three addresses, so they
 * need no shared alignment, only each its own; placing them in one struct at
 * the page's base gives each its natural alignment, the descriptor table on
 * sixteen and past it the rest.
 */
struct dma_page {
    struct virtq_desc desc[QUEUE_SIZE];
    struct {
        uint16_t    flags;
        uint16_t    idx;
        uint16_t    ring[QUEUE_SIZE];
    } avail;
    struct {
        uint16_t    flags;
        uint16_t    idx;
        struct virtq_used_elem ring[QUEUE_SIZE];
    } used;
    struct virtio_blk_req hdr;
    uint8_t     data[VIRTIO_BLK_SECTOR_BYTES];
    uint8_t     status;
};

static struct dma_page *q;

/* How far the driver has advanced the available ring, and consumed the used. */
static uint16_t avail_sent;
static uint16_t used_seen;

/*
 * The device's interrupt and the timeline it advances. The driver waits on
 * the timeline for each completion rather than spinning the used ring, and
 * irq_seen is the number of completions waited for, which is the number of
 * transfers issued: one interrupt per transfer.
 */
static sys_handle_t h_irq = SYS_HANDLE_INVALID;
static sys_handle_t h_fence = SYS_HANDLE_INVALID;
static uint64_t irq_seen;

/* Append a string to a buffer, returning how many bytes were written. */
static size_t
sapp(char *out, const char *s)
{
    size_t w = 0;

    while (s[w] != '\0') {
        out[w] = s[w];
        w++;
    }

    return w;
}

/* Append v in decimal to a buffer, returning how many digits were written. */
static size_t
udec(char *out, uint64_t v)
{
    char tmp[20];
    size_t n = 0, w = 0;

    do {
        tmp[n++] = (char)('0' + (unsigned int)(v % 10u));
        v /= 10u;
    } while (v != 0);
    while (n > 0) {
        out[w++] = tmp[--n];
    }

    return w;
}

static uint32_t
reg_rd(unsigned int off)
{
    return *(volatile uint32_t *)(slot_base + off);
}

static void
reg_wr(unsigned int off, uint32_t v)
{
    *(volatile uint32_t *)(slot_base + off) = v;
}

/* Write a buffer's address to a pair of low and high registers. */
static void
reg_wr_addr(unsigned int low, unsigned int high, const void *p)
{
    uint64_t a = (uint64_t)(uintptr_t)p;

    reg_wr(low, (uint32_t)a);
    reg_wr(high, (uint32_t)(a >> 32));
}

/*
 * Find the block device among the slots in the granted region. A slot with
 * no device reads a zero device id, and a slot that is not virtio at all
 * reads the wrong magic; both are passed over. The first block device found
 * is the one driven.
 */
static int
find_block_device(uintptr_t region, size_t span)
{
    size_t off;

    for (off = 0; off + VIRTIO_MMIO_CONFIG <= span;
      off += VIRTIO_MMIO_SLOT_STRIDE) {
        slot_base = region + off;
        if (reg_rd(VIRTIO_MMIO_MAGIC) != VIRTIO_MAGIC) {
            continue;
        }
        if (reg_rd(VIRTIO_MMIO_DEVICE_ID) != VIRTIO_ID_BLOCK) {
            continue;
        }
        return 0;
    }

    slot_base = 0;

    return -1;
}

/*
 * Bring the device up: reset it, tell it a driver is present, agree on
 * features, lay the queue out and hand it over, and declare the driver ready.
 * The status register is read back after the feature agreement and after the
 * ready declaration, because the device signals a rejection there rather than
 * in a return value, and a rejection ignored is a device driven as though it
 * agreed to something it did not.
 */
static int
bring_up(void)
{
    uint32_t status;
    uint32_t qmax;

    if (reg_rd(VIRTIO_MMIO_VERSION) != 2) {
        return 5;       /* legacy transport is a different queue layout */
    }

    reg_wr(VIRTIO_MMIO_STATUS, 0);      /* reset */
    status = VIRTIO_STATUS_ACKNOWLEDGE;
    reg_wr(VIRTIO_MMIO_STATUS, status);
    status |= VIRTIO_STATUS_DRIVER;
    reg_wr(VIRTIO_MMIO_STATUS, status);

    /*
     * Accept no optional feature, and the one a modern device requires:
     * VIRTIO_F_VERSION_1, feature bit thirty-two, in the high word. A read of
     * a block device needs none of the block feature bits, so the low word is
     * zero.
     */
    reg_wr(VIRTIO_MMIO_DRIVER_FEAT_SEL, 0);
    reg_wr(VIRTIO_MMIO_DRIVER_FEATURES, 0);
    reg_wr(VIRTIO_MMIO_DRIVER_FEAT_SEL, 1);
    reg_wr(VIRTIO_MMIO_DRIVER_FEATURES, 1);  /* bit 32: VIRTIO_F_VERSION_1 */

    status |= VIRTIO_STATUS_FEATURES_OK;
    reg_wr(VIRTIO_MMIO_STATUS, status);
    if ((reg_rd(VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK) == 0) {
        return 6;       /* the device refused the features offered */
    }

    /* Queue zero, no larger than the device allows or the driver laid out. */
    reg_wr(VIRTIO_MMIO_QUEUE_SEL, 0);
    qmax = reg_rd(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax < QUEUE_SIZE) {
        return 7;
    }
    reg_wr(VIRTIO_MMIO_QUEUE_NUM, QUEUE_SIZE);

    reg_wr_addr(VIRTIO_MMIO_QUEUE_DESC_LOW, VIRTIO_MMIO_QUEUE_DESC_HIGH,
      q->desc);
    reg_wr_addr(VIRTIO_MMIO_QUEUE_DRIVER_LOW, VIRTIO_MMIO_QUEUE_DRIVER_HIGH,
      &q->avail);
    reg_wr_addr(VIRTIO_MMIO_QUEUE_DEVICE_LOW, VIRTIO_MMIO_QUEUE_DEVICE_HIGH,
      &q->used);
    reg_wr(VIRTIO_MMIO_QUEUE_READY, 1);

    status |= VIRTIO_STATUS_DRIVER_OK;
    reg_wr(VIRTIO_MMIO_STATUS, status);

    status = reg_rd(VIRTIO_MMIO_STATUS);
    if ((status & VIRTIO_STATUS_FAILED) != 0 ||
      (status & VIRTIO_STATUS_DRIVER_OK) == 0) {
        return 8;       /* the device did not come ready */
    }

    return 0;
}

/*
 * Read one 512-byte sector into the data buffer. The chain is three
 * descriptors: the header the device reads, the sector it writes, and the
 * status byte it writes last. The head goes in the available ring, the device
 * is notified, and the used ring is polled until it advances. The fences keep
 * the descriptors written before the notify and the status read after the
 * completion, because the device sees the queue through memory and not
 * through the register write.
 */
static int
transfer(uint32_t type, uint64_t sector)
{
    q->hdr.type = type;
    q->hdr.reserved = 0;
    q->hdr.sector = sector;
    q->status = 0xff;

    q->desc[0].addr = (uint64_t)(uintptr_t)&q->hdr;
    q->desc[0].len = (uint32_t)sizeof(q->hdr);
    q->desc[0].flags = VIRTQ_DESC_F_NEXT;
    q->desc[0].next = 1;

    /*
     * The data descriptor's direction is the device's, not the driver's: on a
     * read the device writes the sector into this buffer, and on a write it
     * reads the sector out of it. So the write flag is set for a read and
     * clear for a write, which is the one bit that tells the two apart.
     */
    q->desc[1].addr = (uint64_t)(uintptr_t)q->data;
    q->desc[1].len = (uint32_t)sizeof(q->data);
    q->desc[1].flags = VIRTQ_DESC_F_NEXT |
      (type == VIRTIO_BLK_T_IN ? VIRTQ_DESC_F_WRITE : 0u);
    q->desc[1].next = 2;

    q->desc[2].addr = (uint64_t)(uintptr_t)&q->status;
    q->desc[2].len = 1;
    q->desc[2].flags = VIRTQ_DESC_F_WRITE;
    q->desc[2].next = 0;

    q->avail.ring[avail_sent % QUEUE_SIZE] = 0;      /* head of the chain */
    avail_sent++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    q->avail.idx = avail_sent;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    reg_wr(VIRTIO_MMIO_QUEUE_NOTIFY, 0);

    /*
     * Wait for the device's interrupt rather than spinning on the used ring.
     * The handler masked the source and advanced the timeline to the count of
     * interrupts seen, so the driver waits for the count this transfer brings.
     * It then acknowledges the device, which drops the level-triggered line,
     * and unmasks at the controller so the next completion is delivered.
     */
    irq_seen++;
    if (sys_fence_wait(h_fence, irq_seen, SYS_DEADLINE_NEVER) != SYS_OK) {
        return 10;      /* the wait on the interrupt timeline failed */
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    used_seen = q->used.idx;
    reg_wr(VIRTIO_MMIO_INTERRUPT_ACK, reg_rd(VIRTIO_MMIO_INTERRUPT_STATUS));
    (void)sys_irq_unmask(h_irq);

    /*
     * One interrupt per transfer. The timeline reached irq_seen, the count of
     * transfers issued, so the device must have posted the matching
     * completion into the used ring: used_seen must equal avail_sent. If it
     * lags, a completion the driver counted never arrived, and the wait
     * returned only because a spurious or coalesced interrupt advanced the
     * timeline past it. Catching the drift here makes a missed interrupt
     * visible rather than reading a ring that did not advance as though it
     * had.
     */
    if (used_seen != avail_sent) {
        return 20;      /* the completion count drifted from the transfers */
    }

    if (q->status != VIRTIO_BLK_S_OK) {
        return 9;       /* the device reported the transfer failed */
    }

    return 0;
}

static int
read_sector(uint64_t sector)
{
    return transfer(VIRTIO_BLK_T_IN, sector);
}

static int
write_sector(uint64_t sector)
{
    return transfer(VIRTIO_BLK_T_OUT, sector);
}

/*
 * The sector past the store's bytes, scratch the driver may write without
 * touching a file. The store occupies whole sectors from zero; this is the
 * next one.
 */
#define SCRATCH_SECTOR  (((uint64_t)BLK_BLOCK_COUNT * BLK_BLOCK_BYTES + \
                          VIRTIO_BLK_SECTOR_BYTES - 1) / VIRTIO_BLK_SECTOR_BYTES)

/*
 * Prove the write path before serving: write a pattern to the scratch sector
 * and read it back. The sector is past the store's range, so no file is
 * disturbed and nothing the filesystem reads depends on it. A write that did
 * not take would read back as whatever was there before, which the compare
 * catches.
 */
static int
write_selftest(void)
{
    uint32_t i;

    for (i = 0; i < sizeof(q->data); i++) {
        q->data[i] = (uint8_t)(i * 7u + 1u);
    }
    if (write_sector(SCRATCH_SECTOR) != 0) {
        return 17;
    }
    memset(q->data, 0, sizeof(q->data));
    if (read_sector(SCRATCH_SECTOR) != 0) {
        return 18;
    }
    for (i = 0; i < sizeof(q->data); i++) {
        if (q->data[i] != (uint8_t)(i * 7u + 1u)) {
            return 19;
        }
    }

    return 0;
}

/*
 * Report once, at startup, that each self-test transfer completed on exactly
 * one interrupt. transfer() fails any transfer whose completion count drifts
 * from the transfers issued, so reaching here means the count stayed in step:
 * the device posted one used-ring completion per transfer, and the driver
 * waited for one interrupt per completion. The line names both counts so a
 * boot check can confirm they match. It goes to the always-on debug console,
 * which is separate from the block protocol, so the served path pays nothing
 * for it.
 */
static void
report_in_step(void)
{
    char line[80];
    size_t n = 0;

    n += sapp(&line[n], "blk: one interrupt per transfer: ");
    n += udec(&line[n], avail_sent);
    n += sapp(&line[n], " transfers, ");
    n += udec(&line[n], q->used.idx);
    n += sapp(&line[n], " completions\n");
    (void)sys_debug_write(line, (uint32_t)n);
}

/*
 * A read of whole blocks off the device, or a refusal saying which rule it
 * broke, answering the same block protocol the RAM store answered. The three
 * refusals are the store's, not the device's: an unaligned offset and a
 * non-block length could never be right, and a range off the end of the store
 * is a fact about this disk. The bytes come from the device: the sector that
 * holds the range is read over the ring, and the range is copied out of it.
 */
static sys_err_t
serve_read(void *dst, uint32_t dstlen, uint64_t off, uint32_t len)
{
    uint64_t store_bytes = (uint64_t)BLK_BLOCK_COUNT * BLK_BLOCK_BYTES;
    uint64_t sector;

    if ((off % BLK_BLOCK_BYTES) != 0 || (len % BLK_BLOCK_BYTES) != 0 ||
      len == 0) {
        return SYS_ERR_INVALID_ARG;
    }
    if (off / BLK_BLOCK_BYTES >= BLK_BLOCK_COUNT || off + len > store_bytes) {
        return SYS_ERR_NO_ENT;
    }
    if (dst == NULL || dstlen < len) {
        return SYS_ERR_TRUNCATED;
    }

    /*
     * The whole store fits in the first sector, so a range inside it is
     * inside one sector. A store that outgrew a sector would read each
     * sector the range crosses; this one never crosses.
     */
    sector = off / VIRTIO_BLK_SECTOR_BYTES;
    if ((off % VIRTIO_BLK_SECTOR_BYTES) + len > VIRTIO_BLK_SECTOR_BYTES) {
        return SYS_ERR_INVALID_ARG;
    }
    if (read_sector(sector) != 0) {
        return SYS_ERR_STALE;       /* the device would not read the sector */
    }
    memcpy(dst, &q->data[off % VIRTIO_BLK_SECTOR_BYTES], len);

    return SYS_OK;
}

/*
 * A write of whole blocks to the device, refused by the same three rules a
 * read is. A block is smaller than a sector, so the sector it lives in is read
 * first, the block is placed into it, and the sector is written back: the
 * blocks that share the sector are read and written unchanged, so a write
 * places only the block it names. The bytes to write are the caller's, bounded
 * by how many actually arrived rather than by the length the request claims.
 */
static sys_err_t
serve_write(const void *src, uint32_t have, uint64_t off, uint32_t len)
{
    uint64_t store_bytes = (uint64_t)BLK_BLOCK_COUNT * BLK_BLOCK_BYTES;
    uint64_t sector;

    if ((off % BLK_BLOCK_BYTES) != 0 || (len % BLK_BLOCK_BYTES) != 0 ||
      len == 0) {
        return SYS_ERR_INVALID_ARG;
    }
    if (off / BLK_BLOCK_BYTES >= BLK_BLOCK_COUNT || off + len > store_bytes) {
        return SYS_ERR_NO_ENT;
    }
    if (src == NULL || have < len) {
        return SYS_ERR_TRUNCATED;
    }

    sector = off / VIRTIO_BLK_SECTOR_BYTES;
    if ((off % VIRTIO_BLK_SECTOR_BYTES) + len > VIRTIO_BLK_SECTOR_BYTES) {
        return SYS_ERR_INVALID_ARG;
    }
    if (read_sector(sector) != 0) {
        return SYS_ERR_STALE;       /* the device would not read the sector */
    }
    memcpy(&q->data[off % VIRTIO_BLK_SECTOR_BYTES], src, len);
    if (write_sector(sector) != 0) {
        return SYS_ERR_STALE;       /* the device would not write the sector */
    }

    return SYS_OK;
}

int
main(void)
{
    sys_handle_t h_dev, h_dma, chan;
    sys_msg_t message, reply;
    sys_thread_id_t peer;
    uint64_t cookie;
    uintptr_t region, span, dma, dma_span;
    int err;

    h_dev = ns_lookup(SYS_NS_DEVICE);
    if (h_dev == SYS_HANDLE_INVALID) {
        DBG_PUTS("virtio_blk: spawned without its device");
        return 1;
    }
    if (sys_grant_map(h_dev, &region, &span) != SYS_OK) {
        DBG_PUTS("virtio_blk: the device grant would not map");
        return 2;
    }

    h_dma = ns_lookup((sys_ns_id_t)NS_DMA);
    if (h_dma == SYS_HANDLE_INVALID) {
        DBG_PUTS("virtio_blk: spawned without a DMA page");
        return 11;
    }
    if (sys_grant_map(h_dma, &dma, &dma_span) != SYS_OK) {
        DBG_PUTS("virtio_blk: the DMA grant would not map");
        return 12;
    }
    if (dma_span < sizeof(struct dma_page)) {
        DBG_PUTS("virtio_blk: the DMA page is too small for the queue");
        return 13;
    }
    q = (struct dma_page *)dma;
    memset(q, 0, sizeof(*q));

    h_irq = ns_lookup((sys_ns_id_t)NS_IRQ);
    h_fence = ns_lookup((sys_ns_id_t)NS_FENCE);
    if (h_irq == SYS_HANDLE_INVALID || h_fence == SYS_HANDLE_INVALID) {
        DBG_PUTS("virtio_blk: spawned without its interrupt handles");
        return 16;
    }

    if (find_block_device(region, span) != 0) {
        DBG_PUTS("virtio_blk: no block device in the granted slots");
        return 3;
    }

    err = bring_up();
    if (err != 0) {
        DBG_PUTS("virtio_blk: the block device would not come up");
        return err;
    }

    err = write_selftest();
    if (err != 0) {
        DBG_PUTS("virtio_blk: the write did not read back");
        return err;
    }
    report_in_step();

    chan = ns_lookup(SYS_NS_SERVICE);
    if (chan == SYS_HANDLE_INVALID) {
        DBG_PUTS("virtio_blk: spawned without a channel to serve on");
        return 14;
    }

    for (;;) {
        fs_request_t req;
        fs_reply_t rep;
        struct wire_in in;
        struct wire_out out;
        uint32_t extra = 0;     /* read bytes appended after the reply header */

        if (sys_msg_recv(chan, &message, &peer, &cookie) != SYS_OK) {
            return 15;
        }

        wire_in_init(&in, message.inline_data, message.inline_len);
        fs_request_get(&in, &req);

        memset(&reply, 0, sizeof(reply));
        reply.handle = SYS_HANDLE_INVALID;
        memset(&rep, 0, sizeof(rep));
        rep.fr_length = 0;
        rep.fr_size = (uint64_t)BLK_BLOCK_COUNT * BLK_BLOCK_BYTES;

        switch (req.fr_op) {
        case FS_OP_READ:
            if (message.buf != 0) {
                rep.fr_status = serve_read((void *)message.buf,
                  message.buf_len, req.fr_offset, req.fr_length);
                if (rep.fr_status == SYS_OK) {
                    rep.fr_length = req.fr_length;
                }
            } else {
                /*
                 * No buffer lent, so a small read comes back inline, in the
                 * room the reply leaves after its header. This is what lets a
                 * caller that cannot lend memory, the kernel boot thread among
                 * them, read a block at all.
                 */
                uint32_t room =
                  SYS_IPC_INLINE_BYTES - (uint32_t)sizeof(fs_reply_t);

                rep.fr_status = serve_read(
                  &reply.inline_data[sizeof(fs_reply_t)], room,
                  req.fr_offset, req.fr_length);
                if (rep.fr_status == SYS_OK) {
                    rep.fr_length = req.fr_length;
                    extra = req.fr_length;
                }
            }
            break;

        case FS_OP_WRITE: {
            /*
             * The bytes to write follow the request header inline, and are
             * only as many as the message carried, which the store checks
             * against the length the request claims.
             */
            uint32_t have = message.inline_len > sizeof(fs_request_t)
              ? message.inline_len - (uint32_t)sizeof(fs_request_t) : 0;

            rep.fr_status = serve_write(
              &message.inline_data[sizeof(fs_request_t)], have,
              req.fr_offset, req.fr_length);
            if (rep.fr_status == SYS_OK) {
                rep.fr_length = req.fr_length;
            }
            break;
        }

        default:
            /*
             * A store has no names to walk and nothing to open or close, so
             * every other operation is one this server cannot provide. A
             * server answers every operation, and this is the answer.
             */
            rep.fr_status = SYS_ERR_NOT_SUPPORTED;
            break;
        }

        wire_out_init(&out, reply.inline_data, SYS_IPC_INLINE_BYTES);
        fs_reply_put(&out, &rep);
        reply.inline_len = (uint32_t)sizeof(fs_reply_t) + extra;

        /* The same answer where the kernel can read it, for a chain behind. */
        reply.status = rep.fr_status;

        if (sys_msg_reply(peer, &reply) != SYS_OK) {
            return 16;
        }
    }
}
