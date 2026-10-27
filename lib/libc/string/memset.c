/* memset.c : fills a block of memory with one byte value.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * memset stores c, converted to unsigned char, into the first len bytes of b,
 * one byte at a time, and returns b.
 *
 */

#include <string.h>

void *
memset(void *b, int c, size_t len)
{
    unsigned char *p = b;
    size_t i;

    for (i = 0; i < len; i++) {
        p[i] = (unsigned char)c;
    }
    return b;
}
