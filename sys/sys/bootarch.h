/* bootarch.h : boot archive header, member records, and constants.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Defines BOOT_MAGIC, BOOT_VERSION, BOOT_NAME_MAX, and BOOT_ALIGN, the
 * 4096-byte alignment of each payload. Declares struct boot_header and
 * struct boot_member, the fixed-size records that make up an archive.
 * Included by the kernel boot code and the mkbootarch host tool.
 *
 */

#ifndef _SYS_BOOTARCH_H_
#define _SYS_BOOTARCH_H_

#include <stdint.h>

#define BOOT_MAGIC          0x4243594eu     /* "NYCB", little-endian */
#define BOOT_VERSION        2u
#define BOOT_NAME_MAX       16

/*
 * Payload alignment. A page on both current ports, and the smallest
 * granularity a Tier 2 port would want to map at.
 */
#define BOOT_ALIGN          4096u

struct boot_header {
    uint32_t    bh_magic;
    uint32_t    bh_version;
    uint32_t    bh_count;
    uint32_t    bh_bytes;       /* whole archive, header included */
};

/*
 * One member. bm_load is where the image is linked to run, which for a
 * flat binary whose linker script places .text.start first is also its
 * entry point. Recording it here rather than at spawn keeps the archive
 * the single description of what an image is.
 *
 * bm_rw and bm_mem are the layout, added at version 2. A flat
 * binary is a run of bytes and says nothing about which of them may be
 * written or how much memory the program needs beyond the bytes it
 * carries, so a loader given only bm_size has to map the whole slot,
 * writable and executable, and hope. Both are offsets from bm_load
 * rather than addresses, so they survive an image being placed
 * somewhere other than where it was linked.
 *
 *   [0, bm_rw)      what may be executed and not written
 *   [bm_rw, bm_mem) what may be written and not executed
 *   [0, bm_size)    the bytes the archive carries
 *
 * The bytes cross the boundary when a program has initialised data:
 * below it they are text and rodata, above it they are .data, which is
 * copied into writable memory and is the reason bm_size is not simply
 * where text ends. Everything from bm_size to bm_mem is zeroed, being
 * bss and stack.
 *
 * bm_rw and bm_mem are page-aligned and bm_size is no larger than
 * bm_mem, which the linker script arranges and the kernel checks rather
 * than assumes.
 */
struct boot_member {
    char        bm_name[BOOT_NAME_MAX];
    uint64_t    bm_load;
    uint32_t    bm_offset;      /* payload, from the archive base */
    uint32_t    bm_size;
    uint32_t    bm_rw;          /* where writable memory begins */
    uint32_t    bm_mem;         /* where the program's memory ends */
};

_Static_assert(sizeof(struct boot_header) == 16, "boot header is 16 bytes");
_Static_assert(sizeof(struct boot_member) == 40, "boot member is 40 bytes");

#endif /* _SYS_BOOTARCH_H_ */
