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
 * the timeline rather than spinning the used ring. irq_seen is the number of
 * interrupts waited for, which is no longer the number of transfers: a wake
 * that is not a completion is passed over and counted, so interrupts are
 * transfers plus the ones passed over. completions is the number of wakes the
 * driver took for completions, one per transfer; passed_over is the number it
 * told apart as a configuration change or a spurious line and did not.
 */
static sys_handle_t h_irq = SYS_HANDLE_INVALID;
static sys_handle_t h_fence = SYS_HANDLE_INVALID;
static uint64_t irq_seen;
static uint64_t completions;
static uint64_t passed_over;

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
     * Wait for the device's interrupt rather than spinning on the used ring,
     * and keep waiting until the wake is this transfer's completion. The
     * handler masks the source and advances the timeline on every interrupt,
     * so a wake may be a completion, a configuration change, or a spurious
     * line. The completion is the used ring reaching the count of transfers
     * issued, flagged by the queue bit in the interrupt-status register; the
     * rest carry no queue bit and leave the ring where it was. Each wake is
     * acknowledged at the device and the source re-armed, so a non-completion
     * is passed over and the next interrupt delivered.
     */
    for (;;) {
        uint32_t status;

        irq_seen++;
        if (sys_fence_wait(h_fence, irq_seen, SYS_DEADLINE_NEVER) != SYS_OK) {
            return 10;      /* the wait on the interrupt timeline failed */
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        status = reg_rd(VIRTIO_MMIO_INTERRUPT_STATUS);
        used_seen = q->used.idx;
        reg_wr(VIRTIO_MMIO_INTERRUPT_ACK, status);
        (void)sys_irq_unmask(h_irq);

        if (used_seen == avail_sent) {
            /*
             * The ring reached this transfer, so the device posted its
             * completion. A completion carries the queue bit; its absence
             * would mean the ring moved without the device saying so.
             */
            if ((status & VIRTIO_MMIO_INT_VRING) == 0) {
                return 20;      /* the ring advanced with no queue bit */
            }
            completions++;
            break;
        }
        passed_over++;          /* a configuration change or a spurious line */
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
 * Pass over any interrupt that is pending before the first transfer. The
 * device has been brought up but not notified, so it has completed nothing:
 * the used ring is empty and the interrupt-status register carries no queue
 * bit. Whatever is pending here is therefore not a completion, and taking it
 * for one would read a ring that holds nothing. Each is acknowledged and the
 * source re-armed. The wait is bounded because there may be none: a port that
 * cannot forge a pending line hands the driver nothing to pass over, and the
 * wait then returns empty rather than blocking for a completion that only a
 * transfer would bring.
 */
static void
pass_over_pending(void)
{
    for (;;) {
        uint32_t status;

        if (sys_fence_wait(h_fence, irq_seen + 1,
          sys_deadline_in_ms(100)) != SYS_OK) {
            break;      /* nothing pending within the window */
        }
        irq_seen++;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        status = reg_rd(VIRTIO_MMIO_INTERRUPT_STATUS);
        reg_wr(VIRTIO_MMIO_INTERRUPT_ACK, status);
        (void)sys_irq_unmask(h_irq);
        passed_over++;
    }
}

/*
 * Report once, at startup, how the self-test's interrupts were told apart:
 * the transfers issued, the wakes taken for completions, and the wakes passed
 * over as a configuration change or a spurious line. The counts must stay in
 * step: one completion per transfer, and the device's used ring at the same
 * count. A completion count that drifted from the transfers would mean a wake
 * was taken for a completion that no transfer drew, so this refuses to serve
 * rather than report a count it cannot stand behind. The line goes to the
 * always-on debug console, separate from the block protocol, so a boot check
 * can read it.
 */
static int
report_in_step(void)
{
    char line[96];
    size_t n = 0;

    if (completions != avail_sent || (uint16_t)q->used.idx != avail_sent) {
        static const char e[] = "blk: the interrupt count drifted\n";

        (void)sys_debug_write(e, (uint32_t)sizeof(e) - 1);
        return 21;
    }

    n += sapp(&line[n], "blk: interrupts told apart: ");
    n += udec(&line[n], avail_sent);
    n += sapp(&line[n], " transfers, ");
    n += udec(&line[n], completions);
    n += sapp(&line[n], " completions, ");
    n += udec(&line[n], passed_over);
    n += sapp(&line[n], " passed over\n");
    (void)sys_debug_write(line, (uint32_t)n);

    return 0;
}

/*
 * The indirection. A logical block, the block the protocol names, resolves
 * through this map to a physical block, the block on the device that holds its
 * bytes. The map starts as identity, so logical block i lives at physical
 * block i, and it is rebuilt that way each boot rather than read from the disk.
 * Every read and write goes through it, so a logical block is a name looked up
 * here and not an offset computed from the request.
 */
/*
 * The physical arena is larger than the logical store: the first
 * BLK_BLOCK_COUNT physical blocks hold the logical blocks at boot, and the
 * rest are free, to be handed out when a shared block is forked. The arena
 * fits in the sectors the disk already carries, so growing it took no change
 * to the image.
 */
#define STORE_PHYS_BLOCKS   32u      /* BLK_BLOCK_COUNT logical plus free */

static uint16_t block_map[BLK_BLOCK_COUNT];

/*
 * References to each physical block. A block mapped by one logical block has
 * one reference; a block a snapshot also names has more. A write to a block
 * with more than one reference forks rather than overwriting, so no other
 * holder of the old bytes sees the write. A free block has no references.
 */
static uint8_t refc[STORE_PHYS_BLOCKS];

static void
map_init(void)
{
    uint32_t i;

    for (i = 0; i < BLK_BLOCK_COUNT; i++) {
        block_map[i] = (uint16_t)i;
        refc[i] = 1;
    }
    for (i = BLK_BLOCK_COUNT; i < STORE_PHYS_BLOCKS; i++) {
        refc[i] = 0;
    }
}

/*
 * The device byte offset where a physical block's bytes begin. Blocks are
 * laid out end to end from zero, so physical block p begins at p blocks in.
 */
static uint64_t
phys_block_offset(uint16_t pblk)
{
    return (uint64_t)pblk * BLK_BLOCK_BYTES;
}

/*
 * Take a free physical block, giving it its first reference, or fail when the
 * arena holds none. The caller owns the one reference the block now carries.
 */
static int
blk_alloc(uint16_t *out)
{
    uint16_t p;

    for (p = 0; p < STORE_PHYS_BLOCKS; p++) {
        if (refc[p] == 0) {
            refc[p] = 1;
            *out = p;
            return 0;
        }
    }

    return -1;
}

/*
 * Copy one physical block's bytes to another. Each block lives inside a
 * sector, so the source sector is read and the byte range saved, then the
 * destination sector is read, the range placed, and the sector written back,
 * leaving the other blocks in the destination's sector unchanged.
 */
static int
copy_block(uint16_t from, uint16_t to)
{
    uint8_t tmp[BLK_BLOCK_BYTES];
    uint64_t fb = phys_block_offset(from);
    uint64_t tb = phys_block_offset(to);

    if (read_sector(fb / VIRTIO_BLK_SECTOR_BYTES) != 0) {
        return -1;
    }
    memcpy(tmp, &q->data[fb % VIRTIO_BLK_SECTOR_BYTES], BLK_BLOCK_BYTES);
    if (read_sector(tb / VIRTIO_BLK_SECTOR_BYTES) != 0) {
        return -1;
    }
    memcpy(&q->data[tb % VIRTIO_BLK_SECTOR_BYTES], tmp, BLK_BLOCK_BYTES);
    if (write_sector(tb / VIRTIO_BLK_SECTOR_BYTES) != 0) {
        return -1;
    }

    return 0;
}

/* Whether two byte ranges of the same length are equal. */
static int
bytes_equal(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    size_t i;

    for (i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return 0;
        }
    }

    return 1;
}

/*
 * A read of whole blocks off the device, or a refusal saying which rule it
 * broke, answering the same block protocol the RAM store answered. The three
 * refusals are the store's, not the device's: an unaligned offset and a
 * non-block length could never be right, and a range off the end of the store
 * is a fact about this disk. Each block the range covers is resolved through
 * the map to its physical block, whose sector is read over the ring and whose
 * bytes are copied out. A block is smaller than a sector and sector-aligned by
 * its size, so one block never crosses a sector boundary.
 */
static sys_err_t
serve_read(void *dst, uint32_t dstlen, uint64_t off, uint32_t len)
{
    uint64_t store_bytes = (uint64_t)BLK_BLOCK_COUNT * BLK_BLOCK_BYTES;
    uint32_t done;

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

    for (done = 0; done < len; done += BLK_BLOCK_BYTES) {
        uint16_t pblk = block_map[(off + done) / BLK_BLOCK_BYTES];
        uint64_t pbyte = phys_block_offset(pblk);

        if (read_sector(pbyte / VIRTIO_BLK_SECTOR_BYTES) != 0) {
            return SYS_ERR_STALE;   /* the device would not read the sector */
        }
        memcpy((uint8_t *)dst + done,
          &q->data[pbyte % VIRTIO_BLK_SECTOR_BYTES], BLK_BLOCK_BYTES);
    }

    return SYS_OK;
}

/*
 * A write of whole blocks to the device, refused by the same three rules a
 * read is. Each block the range covers is resolved through the map to its
 * physical block. A block only this logical block names is written in place:
 * its sector is read, the block placed into it, and the sector written back,
 * so the blocks sharing the sector are unchanged and the write places only the
 * block it names. A block another holder also names is forked first: a free
 * block takes a copy of the old bytes and the map is pointed at it, so the
 * write lands on the new block and the old bytes stay readable through the
 * other holder. The bytes to write are the caller's, bounded by how many
 * actually arrived rather than by the length the request claims.
 */
static sys_err_t
serve_write(const void *src, uint32_t have, uint64_t off, uint32_t len)
{
    uint64_t store_bytes = (uint64_t)BLK_BLOCK_COUNT * BLK_BLOCK_BYTES;
    uint32_t done;

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

    for (done = 0; done < len; done += BLK_BLOCK_BYTES) {
        uint32_t lblk = (off + done) / BLK_BLOCK_BYTES;
        uint16_t pblk = block_map[lblk];
        uint64_t pbyte;
        uint64_t sector;

        if (refc[pblk] > 1) {
            uint16_t newp;

            if (blk_alloc(&newp) != 0) {
                return SYS_ERR_NO_MEMORY;   /* no free block to fork into */
            }
            if (copy_block(pblk, newp) != 0) {
                return SYS_ERR_STALE;
            }
            refc[pblk]--;
            block_map[lblk] = newp;
            pblk = newp;
        }

        pbyte = phys_block_offset(pblk);
        sector = pbyte / VIRTIO_BLK_SECTOR_BYTES;
        if (read_sector(sector) != 0) {
            return SYS_ERR_STALE;   /* the device would not read the sector */
        }
        memcpy(&q->data[pbyte % VIRTIO_BLK_SECTOR_BYTES],
          (const uint8_t *)src + done, BLK_BLOCK_BYTES);
        if (write_sector(sector) != 0) {
            return SYS_ERR_STALE;   /* the device would not write the sector */
        }
    }

    return SYS_OK;
}

/*
 * The block a write tests against: an unused block, so a file is never
 * disturbed, the way the write self-test keeps to a scratch sector.
 */
#define FORK_TEST_BLOCK     (BLK_BLOCK_COUNT - 1u)

/*
 * Prove the fork before serving: a shared block, written, forks rather than
 * overwriting. The old bytes are laid down while the block is owned, so that
 * write lands in place. A second reference is added, as a snapshot would add
 * one, and the block is written again: this one must fork. The live view then
 * reads the new bytes, while the old physical block still holds the old bytes,
 * untouched. The map and the pool are returned to where they started, so the
 * store serves from the same state a boot without this test would. A fork that
 * did not happen, a write that changed the old block, leaves the old bytes
 * reading as the new ones, which the compare catches.
 */
static int
fork_selftest(void)
{
    uint16_t lblk = FORK_TEST_BLOCK;
    uint16_t p_old = block_map[lblk];
    uint64_t off = (uint64_t)lblk * BLK_BLOCK_BYTES;
    uint8_t a[BLK_BLOCK_BYTES], b[BLK_BLOCK_BYTES], got[BLK_BLOCK_BYTES];
    uint16_t p_new;
    uint32_t i;

    for (i = 0; i < BLK_BLOCK_BYTES; i++) {
        a[i] = (uint8_t)(i * 3u + 5u);
        b[i] = (uint8_t)(i * 11u + 2u);
    }

    if (serve_write(a, sizeof(a), off, BLK_BLOCK_BYTES) != SYS_OK) {
        return 22;
    }

    refc[p_old]++;      /* a second holder, as a snapshot would be */

    if (serve_write(b, sizeof(b), off, BLK_BLOCK_BYTES) != SYS_OK) {
        return 23;
    }
    p_new = block_map[lblk];
    if (p_new == p_old) {
        return 24;      /* it wrote in place rather than forking */
    }

    if (serve_read(got, sizeof(got), off, BLK_BLOCK_BYTES) != SYS_OK) {
        return 25;
    }
    if (!bytes_equal(got, b, BLK_BLOCK_BYTES)) {
        return 26;      /* the live view does not read the new bytes */
    }

    if (read_sector(phys_block_offset(p_old) / VIRTIO_BLK_SECTOR_BYTES) != 0) {
        return 27;
    }
    if (!bytes_equal(&q->data[phys_block_offset(p_old) %
      VIRTIO_BLK_SECTOR_BYTES], a, BLK_BLOCK_BYTES)) {
        return 28;      /* the old block no longer holds the old bytes */
    }

    refc[p_new] = 0;            /* free the fork */
    block_map[lblk] = p_old;    /* back to identity */
    refc[p_old] = 1;

    return 0;
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

    pass_over_pending();

    err = write_selftest();
    if (err != 0) {
        DBG_PUTS("virtio_blk: the write did not read back");
        return err;
    }
    err = report_in_step();
    if (err != 0) {
        return err;
    }

    map_init();

    err = fork_selftest();
    if (err != 0) {
        DBG_PUTS("virtio_blk: a shared block did not fork on write");
        return err;
    }

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
