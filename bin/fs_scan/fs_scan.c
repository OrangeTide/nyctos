/* fs_scan.c : reads a file's size and bytes through the C library.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * A second program from the one that wrote the file: it opens the file by
 * path, asks its size with a stat, and reads it to the end. The size the
 * stat reports must be the number of bytes the file actually holds, so a
 * stat that reported a stale size would disagree with the read. The bytes
 * must also be the ones the writer wrote. Each failure returns a distinct
 * status, and zero means the two agreed.
 *
 */

#include <file.h>

#include <stddef.h>
#include <sys/fs.h>

/* The bytes the writer left, checked here against what the read returns. */
static const unsigned char pattern[24] = {
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
};

int
main(void)
{
    unsigned char buf[64];
    sys_handle_t file;
    uint64_t size;
    uint32_t kind, total = 0, i;

    if (file_open("/made", &file) != SYS_OK) {
        return 2;
    }

    if (file_stat(file, &size, &kind) != SYS_OK) {
        (void)file_close(file);
        return 3;
    }
    if (kind != FS_KIND_FILE) {
        (void)file_close(file);
        return 4;
    }
    if (size == 0 || size > sizeof(buf)) {
        (void)file_close(file);
        return 5;
    }

    /* Read to the end, however many bytes the file really holds. */
    for (;;) {
        uint32_t got;

        if (file_read(file, buf + total, sizeof(buf) - total, total,
          &got) != SYS_OK) {
            (void)file_close(file);
            return 6;
        }
        if (got == 0) {
            break;
        }
        total += got;
        if (total >= sizeof(buf)) {
            break;
        }
    }
    (void)file_close(file);

    /* The size the stat reported must be the bytes the file holds. */
    if ((uint64_t)total != size) {
        return 7;
    }

    /* And those bytes must be the ones the writer wrote. */
    for (i = 0; i < total; i++) {
        if (buf[i] != pattern[i]) {
            return 8;
        }
    }

    return 0;
}
