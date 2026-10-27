/* subr_mem.c : the freestanding memset and memcpy.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Byte-at-a-time implementations of memset and memcpy. The compiler
 * can emit calls to them even under -fno-builtin, so they exist even
 * where no kernel code calls them by name. A port may replace them.
 *
 */

#include <sys/systm.h>

void *
memset(void *dst, int c, size_t len)
{
    unsigned char *p = dst;
    size_t i;

    for (i = 0; i < len; i++)
        p[i] = (unsigned char)c;

    return dst;
}

void *
memcpy(void *dst, const void *src, size_t len)
{
    const unsigned char *s = src;
    unsigned char *d = dst;
    size_t i;

    for (i = 0; i < len; i++)
        d[i] = s[i];

    return dst;
}
