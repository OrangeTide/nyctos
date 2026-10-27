/* cat.c : prints the contents of a file.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Opens the path in its first argument and writes the file to standard
 * output. The path is resolved through the namespace the process was
 * started with, so the program holds no root and knows no absolute names.
 * Each failure prints a message and returns a distinct status: 1 for usage,
 * 2 for a mount point, 3 for a missing file, 4 for a failed read, and 5 for
 * a path that cannot be reached.
 *
 */

#include <file.h>
#include <stdio.h>
#include <stddef.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
    char buf[64];
    sys_handle_t file;
    uint64_t off = 0;
    sys_err_t err;

    if (argc < 2) {
        printf("usage: cat path\n");
        return 1;
    }

    /*
     * The two ways a path can fail to be a file are different and a
     * person needs to be told which. A path that lands exactly on a
     * mount point names a handle this process already holds and has
     * nothing below it to read; a path that lands anywhere else and
     * finds nothing names nothing at all.
     */
    err = file_open(argv[1], &file);
    if (err == SYS_ERR_WRONG_TYPE) {
        printf("cat: %s: that is a mount point\n", argv[1]);
        return 2;
    }
    if (err == SYS_ERR_NO_ENT) {
        printf("cat: %s: no such file\n", argv[1]);
        return 3;
    }
    if (err != SYS_OK) {
        /*
         * Something other than the file not being there. With a
         * union that is usually a half of it that could not be
         * asked, and telling a person their file is missing when
         * their namespace is broken sends them looking in the
         * wrong place.
         */
        printf("cat: %s: cannot be reached\n", argv[1]);
        return 5;
    }

    for (;;) {
        uint32_t got;

        if (file_read(file, buf, sizeof(buf), off, &got) != SYS_OK) {
            printf("cat: %s: read failed\n", argv[1]);
            return 4;
        }
        if (got == 0) {
            break;
        }

        (void)write(STDOUT_FILENO, buf, got);
        off += got;
    }

    (void)file_close(file);

    return 0;
}
