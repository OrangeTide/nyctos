/* virtio_blk.c : a user-space virtio-mmio block driver.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Holds a grant over the machine's virtio-mmio transport slots, scans them
 * for the block device, and drives it. A block device reached this way is a
 * real one behind the block protocol the RAM store used to answer, so a file
 * read reaches bytes on an emulated disk rather than in a server's array.
 *
 */

#include <os/sys.h>
#include <ns.h>
#include <debug.h>
#include <stddef.h>
#include <sys/virtio.h>

/* One mapped slot: the base address of its register window. */
static uintptr_t slot_base;

static uint32_t
reg_rd(unsigned int off)
{
    return *(volatile uint32_t *)(slot_base + off);
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

int
main(void)
{
    sys_handle_t h_dev;
    uintptr_t region, span;
    uint32_t version;

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

    /*
     * The version the slot reports. Modern virtio-mmio is two; the 'virt'
     * machine offers it, and a legacy-only one would be a different queue
     * layout than this driver writes.
     */
    version = reg_rd(VIRTIO_MMIO_VERSION);
    if (version != 1 && version != 2) {
        DBG_PUTS("virtio_blk: the block slot reports an unknown version");
        return 4;
    }

    return 0;
}
