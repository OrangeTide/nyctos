/* echo.c : prints its arguments, separated by spaces.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Writes each argument after the program name, with a single space between
 * them, then a newline. It is a short program that depends only on argument
 * passing.
 *
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
