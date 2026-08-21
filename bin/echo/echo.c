/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Print the arguments. It exists to need argument passing and nothing
 * else, which is what makes it the first thing worth typing at the shell.
 */

#include <stdio.h>

int
main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        printf("%s%s", argv[i], i + 1 < argc ? " " : "");
    }
    printf("\n");

    return 0;
}
