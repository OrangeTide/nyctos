/* anon.c : raises its heap top and writes the range it claimed.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * The region above a program's image is unmapped at load, and the heap
 * top starts at its base, so a store there faults and the fault is
 * refused until the program raises the top. This program raises it by a
 * couple of pages, writes a pattern across the whole range it claimed,
 * and reads it back, returning zero when every word read back what was
 * written. At Tier 2 the pager answers each store with a fresh writable
 * page; at Tier 0 the slot is flat and the store lands with no fault.
 */

#include <os/sys.h>
#include <stddef.h>
#include <stdint.h>

/*
 * How far to raise the top. A page multiple so the claimed range is a
 * whole number of pages and the loop writes all of it.
 */
#define GROW_BYTES  (2u * 4096u)

int
main(int argc, char **argv)
{
    volatile uint32_t *range;
    uintptr_t base;
    unsigned int words = GROW_BYTES / (unsigned int)sizeof(uint32_t);
    unsigned int i;

    (void)argc;
    (void)argv;

    /*
     * The old top comes back as the base of the new range. Before this
     * call an address there was neither reachable nor writable.
     */
    if (sys_heap_grow(GROW_BYTES, &base) != SYS_OK) {
        return -1;
    }

    range = (volatile uint32_t *)base;

    /*
     * The stores that fault. A word per index rather than one value, so
     * a store that landed on the wrong word, and a page that came back
     * still zeroed, both fail the read-back below.
     */
    for (i = 0; i < words; i++) {
        range[i] = 0x100u + i;
    }

    for (i = 0; i < words; i++) {
        if (range[i] != 0x100u + i) {
            return -2;
        }
    }

    return 0;
}
