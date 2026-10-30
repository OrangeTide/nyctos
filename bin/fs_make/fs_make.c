/* fs_make.c : makes a file through the C library and writes it.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * It reaches the file by path through the namespace it was started with,
 * holding no root of its own. The file is opened if it is already there and
 * made if it is not, so the program leaves the same file whichever boot it
 * is. It writes a pattern that reaches past one block, shortens the file,
 * and grows it back, so a reader finds a known length and known bytes. Each
 * failure returns a distinct status.
 *
 */

#include <file.h>

#include <stddef.h>
#include <sys/fs.h>

/* The bytes a reader will check for, 24 of them, more than one block holds. */
static const unsigned char pattern[24] = {
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
    0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
    0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
};

int
main(void)
{
    sys_handle_t file;
    uint32_t put;
    sys_err_t err;

    /* Open it, or make it if this is the first boot to reach it. */
    err = file_open("/made", &file);
    if (err == SYS_ERR_NO_ENT) {
        err = file_create("/made", &file);
    }
    if (err != SYS_OK) {
        return 2;
    }

    /* A write past one block grows the file into the next. */
    if (file_write(file, pattern, sizeof(pattern), 0, &put) != SYS_OK ||
      put != sizeof(pattern)) {
        (void)file_close(file);
        return 3;
    }

    /* Shorten it, then grow it back, leaving a known length and bytes. */
    if (file_truncate(file, 8) != SYS_OK) {
        (void)file_close(file);
        return 4;
    }
    if (file_write(file, pattern, sizeof(pattern), 0, &put) != SYS_OK ||
      put != sizeof(pattern)) {
        (void)file_close(file);
        return 5;
    }

    (void)file_close(file);

    return 0;
}
