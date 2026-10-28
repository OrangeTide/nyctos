/* virtio.h : virtio-mmio register offsets and the block device identity.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * The register map of one virtio-mmio transport slot, the magic and device
 * identity a driver reads to find the block device among the slots, the
 * status and feature bits used to bring the device up, and the split
 * virtqueue layout the device reads out of the driver's own memory. The
 * register offsets are the virtio specification's; the slot stride is the
 * QEMU 'virt' machine's and so is chosen by the architecture. Included by the
 * virtio block driver and by the kernel init code that grants it the slots.
 *
 */

#ifndef _SYS_VIRTIO_H_
#define _SYS_VIRTIO_H_

#include <stdint.h>

/*
 * The 'virt' machine lays its virtio-mmio slots end to end, one transport
 * each, and a driver handed the whole region steps it by this stride to reach
 * the next slot. The two ports space them differently, so the stride is the
 * architecture's rather than the specification's.
 */
#if defined(__aarch64__)
#define VIRTIO_MMIO_SLOT_STRIDE 0x200u
#elif defined(__riscv)
#define VIRTIO_MMIO_SLOT_STRIDE 0x1000u
#else
#error "virtio-mmio slot stride is unknown for this architecture"
#endif

/* Register offsets within one slot. */
#define VIRTIO_MMIO_MAGIC           0x000u  /* "virt", little-endian */
#define VIRTIO_MMIO_VERSION         0x004u  /* 1 legacy, 2 modern */
#define VIRTIO_MMIO_DEVICE_ID       0x008u  /* 0 empty slot, 2 block */
#define VIRTIO_MMIO_VENDOR_ID       0x00cu
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010u
#define VIRTIO_MMIO_DEVICE_FEAT_SEL 0x014u
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020u
#define VIRTIO_MMIO_DRIVER_FEAT_SEL 0x024u
#define VIRTIO_MMIO_QUEUE_SEL       0x030u
#define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034u
#define VIRTIO_MMIO_QUEUE_NUM       0x038u
#define VIRTIO_MMIO_QUEUE_READY     0x044u
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050u
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060u
#define VIRTIO_MMIO_INTERRUPT_ACK   0x064u

/*
 * The bits of the interrupt-status register. A used-ring update is a queue
 * completion; a configuration change is not. A line that asserts with
 * neither bit set is spurious. The driver tells a completion from the rest
 * by these bits rather than by the fact an interrupt arrived.
 */
#define VIRTIO_MMIO_INT_VRING       0x1u
#define VIRTIO_MMIO_INT_CONFIG      0x2u
#define VIRTIO_MMIO_STATUS          0x070u
#define VIRTIO_MMIO_QUEUE_DESC_LOW  0x080u
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084u
#define VIRTIO_MMIO_QUEUE_DRIVER_LOW 0x090u
#define VIRTIO_MMIO_QUEUE_DRIVER_HIGH 0x094u
#define VIRTIO_MMIO_QUEUE_DEVICE_LOW 0x0a0u
#define VIRTIO_MMIO_QUEUE_DEVICE_HIGH 0x0a4u
#define VIRTIO_MMIO_CONFIG          0x100u

/* The magic every slot carries: the bytes 'v','i','r','t' little-endian. */
#define VIRTIO_MAGIC    0x74726976u

/* The device ids this system knows. A block device answers with two. */
#define VIRTIO_ID_BLOCK 2u

/* Status register bits, written in order to bring a device up. */
#define VIRTIO_STATUS_ACKNOWLEDGE   0x01u
#define VIRTIO_STATUS_DRIVER        0x02u
#define VIRTIO_STATUS_DRIVER_OK     0x04u
#define VIRTIO_STATUS_FEATURES_OK   0x08u
#define VIRTIO_STATUS_FAILED        0x80u

/*
 * One descriptor of a split virtqueue. The address is where the device reads
 * or writes, as the machine sees it, which for an identity-mapped driver is
 * the address the driver itself uses.
 */
struct virtq_desc {
    uint64_t    addr;
    uint32_t    len;
    uint16_t    flags;
    uint16_t    next;
};

#define VIRTQ_DESC_F_NEXT   1u  /* chains to the descriptor in next */
#define VIRTQ_DESC_F_WRITE  2u  /* device writes it, rather than reads */

/* The ring the driver fills with the heads of chains it has made ready. */
struct virtq_avail {
    uint16_t    flags;
    uint16_t    idx;
    uint16_t    ring[];
};

/* One completion the device has written back. */
struct virtq_used_elem {
    uint32_t    id;         /* head descriptor of the chain that completed */
    uint32_t    len;        /* bytes the device wrote into the chain */
};

/* The ring the device fills with the chains it has finished. */
struct virtq_used {
    uint16_t    flags;
    uint16_t    idx;
    struct virtq_used_elem ring[];
};

/*
 * The block request header, which leads the data in a read or write. The
 * sector is a 512-byte unit regardless of the block size the protocol above
 * uses, so a driver turns a byte offset into a sector here.
 */
struct virtio_blk_req {
    uint32_t    type;
    uint32_t    reserved;
    uint64_t    sector;
};

#define VIRTIO_BLK_T_IN     0u  /* read from the device */
#define VIRTIO_BLK_T_OUT    1u  /* write to the device */

#define VIRTIO_BLK_S_OK     0u
#define VIRTIO_BLK_S_IOERR  1u
#define VIRTIO_BLK_S_UNSUPP 2u

/* The sector size the block transport counts in. */
#define VIRTIO_BLK_SECTOR_BYTES 512u

#endif /* _SYS_VIRTIO_H_ */
