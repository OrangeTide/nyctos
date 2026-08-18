/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The ramdisk filesystem driver. It parses the TFS image (sys/sys/tfs.h)
 * QEMU places in RAM at a fixed physical address via '-device loader'
 * (see sys/kern/kern_exec.c) and answers lookups by linear scan of its
 * flat directory table. There is no subdirectory structure to walk: a
 * name like "bin/motd" is compared whole against each entry, the same
 * way QNX's IFS and Plan 9's boot floppy resolve their boot-time root.
 */

#include <sys/tfs.h>
#include <sys/vfs.h>

static const struct tfs_header *rf_hdr;
static const struct tfs_dirent *rf_dirents;
static const char *rf_base;
static int rf_valid;

static int
streq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int
eqmagic(const char *a, const char *b, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

void
ramfs_init(const void *image)
{
    const struct tfs_header *hdr = (const struct tfs_header *)image;

    if (!eqmagic(hdr->th_magic, TFS_MAGIC, TFS_MAGIC_LEN)) {
        rf_valid = 0;
        return;
    }

    rf_hdr = hdr;
    rf_dirents = (const struct tfs_dirent *)((const char *)image + sizeof(*hdr));
    rf_base = (const char *)image;
    rf_valid = 1;
}

int
ramfs_lookup(const char *path, struct vnode *vp)
{
    uint32_t i;

    if (!rf_valid) {
        return -1;
    }

    for (i = 0; i < rf_hdr->th_nfiles; i++) {
        if (streq(rf_dirents[i].td_name, path)) {
            vp->v_data = rf_base + rf_dirents[i].td_offset;
            vp->v_size = rf_dirents[i].td_size;
            return 0;
        }
    }
    return -1;
}
