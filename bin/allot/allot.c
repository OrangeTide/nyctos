/* allot.c : exercises malloc and free over the growable heap.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * A freed block is handed back out without the heap growing again, a set
 * of live blocks never overlap, and a block keeps its bytes while later
 * allocations come and go. The run allocates and frees enough to grow the
 * heap past its first reservation, so reuse and growth are both covered.
 * A nonzero return says which of those failed.
 */

#include <os/sys.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>

#define ALIGN   16u
#define NLIVE   24

static int
aligned(void *p)
{
    return ((uintptr_t)p & (ALIGN - 1)) == 0;
}

static int
overlaps(uintptr_t a, size_t an, uintptr_t b, size_t bn)
{
    return a < b + bn && b < a + an;
}

int
main(int argc, char **argv)
{
    void *live[NLIVE];
    size_t sz[NLIVE];
    unsigned int i, j;

    (void)argc;
    (void)argv;

    /*
     * A freed block is reused. The first allocation grows the heap, so
     * the top rises; freeing it and asking again must return the same
     * block and leave the top where it was. A top that rose again is an
     * allocator that grew instead of reusing.
     */
    {
        void *a, *b;
        uintptr_t ta, tb;

        a = malloc(100);
        if (a == NULL || !aligned(a)) {
            return -1;
        }
        if (sys_heap_grow(0, &ta) != SYS_OK) {
            return -2;
        }
        free(a);
        b = malloc(100);
        if (b == NULL) {
            return -3;
        }
        if (sys_heap_grow(0, &tb) != SYS_OK) {
            return -2;
        }
        if (tb != ta) {
            return -4;
        }
        if (b != a) {
            return -5;
        }
        free(b);
    }

    /*
     * A spread of live blocks, each stamped with a byte of its own. The
     * sizes vary so the blocks do not all fall on the same boundary, and
     * together they outgrow the heap's first page.
     */
    for (i = 0; i < NLIVE; i++) {
        sz[i] = (size_t)(16 + ((i * 53) % 480));
        live[i] = malloc(sz[i]);
        if (live[i] == NULL || !aligned(live[i])) {
            return -10;
        }
        for (j = 0; j < sz[i]; j++) {
            ((volatile uint8_t *)live[i])[j] = (uint8_t)(0x40 + i);
        }
    }

    /*
     * Free half and allocate replacements. This hands freed blocks back
     * out and grows the heap for what the freed ones cannot cover, all
     * while the odd-numbered blocks stay live underneath.
     */
    for (i = 0; i < NLIVE; i += 2) {
        free(live[i]);
        sz[i] = (size_t)(16 + (((i + 7) * 29) % 400));
        live[i] = malloc(sz[i]);
        if (live[i] == NULL || !aligned(live[i])) {
            return -20;
        }
        for (j = 0; j < sz[i]; j++) {
            ((volatile uint8_t *)live[i])[j] = (uint8_t)(0x80 + i);
        }
    }

    /*
     * No two live blocks share an address. An allocator that handed out
     * an overlapping block has two live pointers into one range, which
     * this finds directly rather than waiting for a write to corrupt one.
     */
    for (i = 0; i < NLIVE; i++) {
        for (j = i + 1; j < NLIVE; j++) {
            if (overlaps((uintptr_t)live[i], sz[i],
              (uintptr_t)live[j], sz[j])) {
                return -30;
            }
        }
    }

    /*
     * And every live block still reads back its own byte, so nothing a
     * later allocation did reached into one that was still in use.
     */
    for (i = 0; i < NLIVE; i++) {
        uint8_t want = (uint8_t)((i % 2 == 0 ? 0x80u : 0x40u) + i);

        for (j = 0; j < sz[i]; j++) {
            if (((volatile uint8_t *)live[i])[j] != want) {
                return -40;
            }
        }
    }

    return 0;
}
