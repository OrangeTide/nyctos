/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The memory primitives a freestanding kernel has to supply for itself.
 * The compiler is entitled to emit calls to these for structure assignment
 * and array initialisation even under -fno-builtin, so they must exist even
 * where no kernel source calls them by name.
 *
 * These are the obvious byte-at-a-time versions. A port that cares can
 * replace them; nothing here is on a path that has been measured.
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
