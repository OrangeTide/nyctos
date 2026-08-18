/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
puts(const char *s)
{
    size_t len = strlen(s);

    if (write(STDOUT_FILENO, s, len) < 0) {
        return -1;
    }
    if (write(STDOUT_FILENO, "\n", 1) < 0) {
        return -1;
    }
    return (int)len + 1;
}
