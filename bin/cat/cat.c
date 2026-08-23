/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Print a file.
 *
 * The name is a path, resolved against the namespace this process was
 * given, per section 20.3. What that means here is what it should mean:
 * nothing. This program holds no root, knows no absolute names, and
 * cannot say where `/motd` lives; it hands a string to the library and
 * gets back a handle to something it was already entitled to reach.
 *
 * It used to speak the protocol of section 25 itself, in about fifty
 * lines of message building, because there was no way to name a file
 * except by sending a walk to the one directory it had been handed. That
 * is the number the milestone is measured against: if naming made
 * programs longer, the complexity moved into the library rather than
 * went away.
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
    if (err != SYS_OK) {
        printf("cat: %s: no such file\n", argv[1]);
        return 3;
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
