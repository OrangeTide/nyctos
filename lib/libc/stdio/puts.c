/* puts.c : writes a string and a newline to standard output.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * puts writes the string and then a newline to STDOUT_FILENO. It returns the
 * length plus one on success, or -1 if either write fails.
 *
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
