/* memcpy.c : copies a block of memory byte by byte.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * memcpy copies len bytes from src to dst, one byte at a time, and returns
 * dst. The behavior is undefined if the regions overlap, as with the standard
 * memcpy.
 *
 */

#include <string.h>

void *
memcpy(void *dst, const void *src, size_t len)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    size_t i;

    for (i = 0; i < len; i++) {
        d[i] = s[i];
    }
    return dst;
}
