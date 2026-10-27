/* virtio_blk.c : a user-space virtio-mmio block driver.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Holds a grant over the machine's virtio-mmio transport slots, scans them
 * for the block device, brings it up over one split virtqueue laid out in the
 * driver's own memory, and drives it. A block device reached this way is a
 * real one behind the block protocol the RAM store used to answer, so a file
 * read reaches bytes on an emulated disk rather than in a server's array.
 *
 * The device reaches the queue and buffers by their addresses as the machine
 * sees them. User memory is identity-mapped, so the address the driver holds
 * is the physical address the device reads, and the queue needs no
 * translation.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <debug.h>
#include <stddef.h>
#include <string.h>
#include <sys/virtio.h>

/* One mapped slot: the base address of its register window. */
static uintptr_t slot_base;

/*
 * The one virtqueue, laid out in the driver's own memory. Modern
 * virtio-mmio takes the three rings at three addresses, so they need no
 * shared alignment, only each its own: the descriptor table on sixteen, the
 * available ring on two, the used ring on four.
 */
#define QUEUE_SIZE  8u

static struct virtq_desc desc[QUEUE_SIZE] __attribute__((aligned(16)));

static struct {
    uint16_t    flags;
    uint16_t    idx;
    uint16_t    ring[QUEUE_SIZE];
} avail __attribute__((aligned(2)));

static struct {
    uint16_t    flags;
    uint16_t    idx;
    struct virtq_used_elem ring[QUEUE_SIZE];
} used __attribute__((aligned(4)));

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

    memset(desc, 0, sizeof(desc));
    memset(&avail, 0, sizeof(avail));
    memset(&used, 0, sizeof(used));
    reg_wr_addr(VIRTIO_MMIO_QUEUE_DESC_LOW, VIRTIO_MMIO_QUEUE_DESC_HIGH, desc);
    reg_wr_addr(VIRTIO_MMIO_QUEUE_DRIVER_LOW, VIRTIO_MMIO_QUEUE_DRIVER_HIGH,
      &avail);
    reg_wr_addr(VIRTIO_MMIO_QUEUE_DEVICE_LOW, VIRTIO_MMIO_QUEUE_DEVICE_HIGH,
      &used);
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

int
main(void)
{
    sys_handle_t h_dev;
    uintptr_t region, span;
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

    if (find_block_device(region, span) != 0) {
        DBG_PUTS("virtio_blk: no block device in the granted slots");
        return 3;
    }

    err = bring_up();
    if (err != 0) {
        DBG_PUTS("virtio_blk: the block device would not come up");
        return err;
    }

    return 0;
}
