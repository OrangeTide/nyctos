/* anon.c : writes a page above its image and reads it back.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * The region above the system page is anonymous memory the loader leaves
 * unmapped. A store there faults, and the pager answers the fault with a
 * zeroed writable page. The address comes from the command line, because the
 * kernel is what knows where a slot's anonymous region begins on each tier.
 * The program writes a pattern across the page and reads it back, returning
 * zero when every word read back what was written.
 *
 */

#include <os/sys.h>
#include <stddef.h>
#include <stdint.h>

/* The system page size. A literal for the same reason paged.c uses one. */
#define PAGE_BYTES 4096u

static int
hex(const char *s, uintptr_t *out)
{
    uintptr_t v = 0;

    if (*s == '\0') {
        return -1;
    }
    for (; *s != '\0'; s++) {
        uintptr_t d;

        if (*s >= '0' && *s <= '9') {
            d = (uintptr_t)(*s - '0');
        } else if (*s >= 'a' && *s <= 'f') {
            d = (uintptr_t)(*s - 'a') + 10;
        } else if (*s >= 'A' && *s <= 'F') {
            d = (uintptr_t)(*s - 'A') + 10;
        } else {
            return -1;
        }
        v = (v << 4) | d;
    }

    *out = v;

    return 0;
}

int
main(int argc, char **argv)
{
    volatile uint32_t *page;
    uintptr_t at;
    unsigned int words = PAGE_BYTES / (unsigned int)sizeof(uint32_t);
    unsigned int i;

    if (argc != 2 || hex(argv[1], &at) != 0) {
        return -1;
    }

    page = (volatile uint32_t *)at;

    /*
     * The store that faults. A word per index rather than one value, so
     * a store that landed on the wrong word, and a page that came back
     * still zeroed, both fail the read-back below.
     */
    for (i = 0; i < words; i++) {
        page[i] = 0x100u + i;
    }

    for (i = 0; i < words; i++) {
        if (page[i] != 0x100u + i) {
            return -2;
        }
    }

    return 0;
}
