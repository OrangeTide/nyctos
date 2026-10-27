/* gets.c : reads one line from standard input into a buffer.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * readline reads one line from standard input into a buffer by calling read
 * on STDIN_FILENO until it sees a newline or the buffer is full. The newline
 * is removed and the result is NUL-terminated. The function returns the number
 * of bytes stored, or -1 on error or invalid arguments, and a line longer than
 * the buffer is returned in pieces.
 *
 */

#include <stddef.h>
#include <stdio.h>
#include <unistd.h>

int
readline(char *buf, int size)
{
    int total = 0;

    if (buf == NULL || size < 2) {
        return -1;
    }

    while (total < size - 1) {
        ssize_t n = read(STDIN_FILENO, buf + total, (size_t)(size - 1 - total));

        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            break;
        }

        total += (int)n;
        if (buf[total - 1] == '\n') {
            total--;
            break;
        }
    }

    buf[total] = '\0';

    return total;
}
