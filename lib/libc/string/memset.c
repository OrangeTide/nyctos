/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
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
