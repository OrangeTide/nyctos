/* strlen.c : returns the length of a string.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * strlen counts the bytes before the terminating NUL in s and returns the
 * count.
 *
 */

#include <string.h>

size_t
strlen(const char *s)
{
    const char *p = s;

    while (*p != '\0') {
        p++;
    }
    return (size_t)(p - s);
}
