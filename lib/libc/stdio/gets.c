/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * A line of input, which is what every program that talks to a person
 * needs and the only shape the console offers.
 *
 * There is no buffering here and none is needed: the console service
 * answers a read with at most one line and blocks until it has one, so a
 * line is exactly what one read returns. A file would need buffering and a
 * file is not something a program can open yet.
 *
 * The newline is dropped and the result is terminated, because every
 * caller wants a word rather than a line ending. A line longer than the
 * buffer is returned in pieces, which is the same thing read() does and
 * for the same reason.
 */

#include <stddef.h>
#include <stdio.h>
#include <unistd.h>

int
readline(char *buf, int size)
{
    ssize_t n;

    if (buf == NULL || size < 2) {
        return -1;
    }

    n = read(STDIN_FILENO, buf, (size_t)size - 1);
    if (n < 0) {
        return -1;
    }

    if (n > 0 && buf[n - 1] == '\n') {
        n--;
    }
    buf[n] = '\0';

    return (int)n;
}
