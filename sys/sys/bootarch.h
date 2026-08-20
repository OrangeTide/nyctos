/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The boot archive of section 24.4: a header, a table of fixed-size
 * records, and payloads aligned to the port's region granularity. No
 * compression, because a decompressor implies a RAM budget the compact
 * build does not have.
 *
 * Alignment is the detail worth getting right the first time. Text runs in
 * place on Tier 0 and is mapped on Tier 2, and both want a payload
 * naturally placed. The cpio format the initramfs lineage inherited aligns
 * to two or four bytes, which defeats both.
 *
 * The walk is a loop over fixed-size records and is not a filesystem: it
 * speaks no protocol and resolves no paths. Section 24.4 has root and the
 * romfs server each carry their own copy of it rather than putting a
 * server in the boot path ahead of root. The kernel carries one too, for
 * now, because root does not exist yet.
 *
 * This header is shared with the host tool that writes the archive, so it
 * must stay free of anything the host does not have.
 */

#ifndef _SYS_BOOTARCH_H_
#define _SYS_BOOTARCH_H_

#include <stdint.h>

#define BOOT_MAGIC          0x4243594eu     /* "NYCB", little-endian */
#define BOOT_VERSION        1u
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
 */
struct boot_member {
    char        bm_name[BOOT_NAME_MAX];
    uint64_t    bm_load;
    uint32_t    bm_offset;      /* payload, from the archive base */
    uint32_t    bm_size;
};

_Static_assert(sizeof(struct boot_header) == 16, "boot header is 16 bytes");
_Static_assert(sizeof(struct boot_member) == 32, "boot member is 32 bytes");

#endif /* _SYS_BOOTARCH_H_ */
