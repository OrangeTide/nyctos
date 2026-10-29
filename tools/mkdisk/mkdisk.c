/* mkdisk.c : build-host tool that writes the block device's disk image.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Writes a raw disk image holding the three files the block store serves, each
 * at the block offset blk.h names, so a virtio block device backed by this
 * image answers a block read with the same bytes the RAM store answered.
 * Usage:
 *
 *   mkdisk out.img
 *
 * It runs on the build host with the host compiler and stdio, and shares only
 * sys/blk.h with the system, so the image and the servers agree on one
 * description of the layout.
 *
 */

#include <sys/blk.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The same three strings the block store formats, placed where blk.h says.
 * Their lengths are checked against the header so an edit here that changes a
 * length stops the build rather than serving a short file or the tail of the
 * next one.
 */
static const char motd_text[] = "served from outside the kernel\n";
static const char alt_motd_text[] = "and this one from the other one\n";
static const char notes_text[] = "only the second directory has this\n";

_Static_assert(sizeof(motd_text) - 1 == BLK_MOTD_BYTES,
  "motd is not the length blk.h says");
_Static_assert(sizeof(alt_motd_text) - 1 == BLK_ALT_MOTD_BYTES,
  "the second motd is not the length blk.h says");
_Static_assert(sizeof(notes_text) - 1 == BLK_NOTES_BYTES,
  "notes is not the length blk.h says");

static const struct {
    unsigned int block;
    const char  *text;
} contents[] = {
    { BLK_MOTD_BLOCK,     motd_text },
    { BLK_ALT_MOTD_BLOCK, alt_motd_text },
    { BLK_NOTES_BLOCK,    notes_text },
};

#define CONTENT_COUNT   (sizeof(contents) / sizeof(contents[0]))

/*
 * The image is a whole number of virtio sectors, because the block transport
 * counts in 512-byte sectors regardless of the sixteen-byte block the protocol
 * above uses, and a backing file shorter than one sector is a device of zero
 * sectors. The store's bytes fit the first sector; the second is scratch, past
 * the store's range and so invisible to the filesystem, where the driver
 * writes and reads a sector back to prove its write path without disturbing a
 * file. The third holds the store's metadata, left zeroed here: the store reads
 * an all-zero sector as a fresh image with no metadata yet and writes its own.
 */
#define SECTOR_BYTES    512u
#define STORE_SECTORS   (((BLK_BLOCK_COUNT * BLK_BLOCK_BYTES + SECTOR_BYTES - \
                          1u) / SECTOR_BYTES))
#define DISK_BYTES      ((STORE_SECTORS + 2u) * SECTOR_BYTES)

int
main(int argc, char **argv)
{
    unsigned char disk[DISK_BYTES];
    FILE *out;
    unsigned int i;

    if (argc != 2) {
        fprintf(stderr, "usage: %s out.img\n", argv[0]);
        return 2;
    }

    memset(disk, 0, sizeof(disk));
    for (i = 0; i < CONTENT_COUNT; i++) {
        memcpy(&disk[contents[i].block * BLK_BLOCK_BYTES], contents[i].text,
          strlen(contents[i].text));
    }

    out = fopen(argv[1], "wb");
    if (out == NULL) {
        fprintf(stderr, "mkdisk: cannot open %s\n", argv[1]);
        return 1;
    }
    if (fwrite(disk, 1, sizeof(disk), out) != sizeof(disk)) {
        fprintf(stderr, "mkdisk: short write to %s\n", argv[1]);
        fclose(out);
        return 1;
    }
    if (fclose(out) != 0) {
        fprintf(stderr, "mkdisk: cannot close %s\n", argv[1]);
        return 1;
    }

    return 0;
}
