/* strncmp.c : compares at most n bytes of two strings.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Bytes are compared as unsigned characters. The result is the difference
 * between the first mismatched pair, or 0 if the bytes match through a
 * terminating NUL or through n bytes.
 *
 */

#include <string.h>

int
strncmp(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        }
        if (a[i] == '\0') {
            break;
        }
    }

    return 0;
}
