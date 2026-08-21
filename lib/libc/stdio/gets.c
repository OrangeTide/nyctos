/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * A line of input, which is what every program that talks to a person
 * needs and the only shape the console offers.
 *
 * One read is not one line, which is what the first version of this
 * assumed. A read carries at most what fits in a message, and the console
 * answers with as much of a line as that allows, so a line longer than
 * forty-eight bytes arrives in pieces. A caller handed a piece cannot tell
 * it from a line, and a shell handed one runs it as a command: one long
 * line became six, five of them nonsense.
 *
 * So this reads until it sees the end of a line or fills the buffer. The
 * newline is dropped and the result is terminated, because every caller
 * wants a word rather than a line ending. A line too long for the buffer
 * fills it and returns, and the caller can tell because the length is the
 * buffer's: what is left of the line arrives on the next call, which is
 * the only thing a reader with a fixed buffer can offer.
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
