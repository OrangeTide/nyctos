/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * TFS: the on-disk format of the initramfs image. This is the ABI between
 * tools/mkfsimg, which builds the image at build time, and sys/kern/ramfs.c,
 * which parses it at boot; both include this header verbatim, the same way
 * sys/sys/syscall.h is the ABI between the kernel and libc.
 *
 * The format is deliberately the simplest thing that works, patterned
 * after the boot-image filesystems this problem has been solved with
 * before: QNX's IFS and Plan 9's boot floppy are both a flat table of
 * named files with no subdirectories, and cpio/romfs show the same
 * header-then-data shape. There is no subdirectory structure; a path like
 * "bin/motd" is one opaque name compared whole, not walked component by
 * component. That is enough for a boot-time root and is the seam where a
 * real directory tree goes if one is ever needed.
 *
 * Layout:
 *
 *   struct tfs_header
 *   struct tfs_dirent[th_nfiles]
 *   file data, each entry's bytes starting at its td_offset
 *
 * Every field is naturally aligned with no compiler-inserted padding on
 * any ABI this project targets, so the struct can be laid down and read
 * back as raw bytes. This assumes the image is built on a little-endian
 * host for little-endian targets, which holds for every architecture and
 * host this project currently builds on; a byte-order-independent format
 * is a straightforward follow-up if that ever changes.
 */

#ifndef _SYS_TFS_H_
#define _SYS_TFS_H_

#include <stdint.h>

#define TFS_MAGIC "TFS1"
#define TFS_MAGIC_LEN 4
#define TFS_NAME_MAX 55
#define TFS_ALIGN 16

struct tfs_header {
    char     th_magic[TFS_MAGIC_LEN];
    uint32_t th_nfiles;
};

struct tfs_dirent {
    char     td_name[TFS_NAME_MAX + 1];
    uint32_t td_offset;
    uint32_t td_size;
};

#endif /* _SYS_TFS_H_ */
