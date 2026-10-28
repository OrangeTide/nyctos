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
#define BOOT_VERSION        3u
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
 * One member. The payload is the program's ELF, debug-stripped, and the
 * loader places it by walking its PT_LOAD headers. The fields below are
 * what the archive tool reads out of those headers so that the loader, the
 * report, the memory reservation, and the overlap check do not each parse
 * the ELF again.
 *
 * bm_load is the lowest virtual address of any loadable segment, where the
 * image is linked to run and, the linker placing .text.start first, also
 * its entry point. bm_offset and bm_elfsize locate the ELF payload in the
 * archive.
 *
 * bm_rw and bm_mem are the memory layout, as offsets from bm_load so they
 * survive an image placed somewhere other than where it was linked:
 *
 *   [0, bm_rw)      what may be executed and not written
 *   [bm_rw, bm_mem) what may be written and not executed
 *   [0, bm_size)    the image bytes present by virtual address
 *
 * bm_rw is where the first writable segment begins, bm_mem is where the
 * writable segment's memory ends, and bm_size is the extent of the bytes
 * the segments carry by file size. The bytes cross bm_rw when a program
 * has initialised data: below it they are text and rodata, above it they
 * are .data. Everything from bm_size to bm_mem is zeroed at run time,
 * being bss, the stack, and the startup record page.
 *
 * bm_rw and bm_mem are page-aligned and bm_size is no larger than bm_mem,
 * which the linker script arranges and the kernel checks rather than
 * assumes. bm_size and bm_mem are the image extent by virtual address,
 * which is not bm_elfsize: the ELF carries a symbol table and headers
 * beyond the loadable segments.
 */
struct boot_member {
    char        bm_name[BOOT_NAME_MAX];
    uint64_t    bm_load;
    uint32_t    bm_offset;      /* ELF payload, from the archive base */
    uint32_t    bm_size;        /* image extent by virtual address */
    uint32_t    bm_rw;          /* where writable memory begins */
    uint32_t    bm_mem;         /* where the program's memory ends */
    uint32_t    bm_elfsize;     /* bytes of the ELF payload in the archive */
};

_Static_assert(sizeof(struct boot_header) == 16, "boot header is 16 bytes");
_Static_assert(sizeof(struct boot_member) == 48, "boot member is 48 bytes");

#endif /* _SYS_BOOTARCH_H_ */
