/* malloc.c : a heap allocator over the process's growable heap.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * malloc carves blocks from a free list and grows the heap when the list
 * cannot satisfy a request. free returns a block to the list, where the
 * next request of a fitting size reuses it rather than growing again.
 * Each block carries a header with its usable size, so free needs only
 * the pointer the caller was given.
 *
 * The heap grows through sys_heap_grow, which raises the top and reports
 * the old top. A grow takes the whole page-rounded region the kernel
 * added as one free block, so later requests are served from it without
 * another grow until it is spent. Blocks never overlap: a split hands out
 * the front of a free block and keeps the remainder, and the regions a
 * grow returns are disjoint because each begins at the previous top.
 *
 * Not synchronized. A process with more than one thread calling this at
 * once would corrupt the list, and nothing here guards against it yet.
 */

#include <os/sys.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/*
 * The alignment every block is handed out at, which is the widest a
 * target needs. The header is a multiple of it, so a payload one header
 * past an aligned block is aligned too.
 */
#define ALIGN   16u

struct block {
    size_t          size;       /* usable bytes after the header */
    struct block   *next;       /* the next free block, while on the list */
};

#define HDR     ((size_t)((sizeof(struct block) + ALIGN - 1) & ~(ALIGN - 1)))

static struct block *free_list;

static size_t
round_up(size_t n)
{
    return (n + (ALIGN - 1)) & ~(size_t)(ALIGN - 1);
}

/*
 * Take the whole region a grow added as one free block. The request is
 * what the caller needs; the kernel rounds it up to a page, so the top it
 * reports back is at or beyond the request, and the difference is free
 * memory the block covers rather than loses.
 */
static int
heap_grow(size_t want)
{
    struct block *b;
    uintptr_t base, top;
    size_t have;

    if (sys_heap_grow(want, &base) != SYS_OK) {
        return -1;
    }
    if (sys_heap_grow(0, &top) != SYS_OK) {
        return -1;
    }

    have = (size_t)(top - base);
    if (have < HDR + ALIGN) {
        return -1;
    }

    b = (struct block *)base;
    b->size = have - HDR;
    b->next = free_list;
    free_list = b;

    return 0;
}

void *
malloc(size_t n)
{
    struct block *b, **link;
    size_t need;

    if (n == 0) {
        return NULL;
    }

    need = round_up(n);

    for (;;) {
        for (link = &free_list; (b = *link) != NULL; link = &b->next) {
            if (b->size < need) {
                continue;
            }

            /*
             * Keep the remainder as its own block when what is left over
             * holds a header and a least-sized payload. Otherwise the
             * whole block goes, its few spare bytes unused until it is
             * freed.
             */
            if (b->size >= need + HDR + ALIGN) {
                struct block *rest;

                rest = (struct block *)((uint8_t *)b + HDR + need);
                rest->size = b->size - need - HDR;
                rest->next = b->next;
                b->size = need;
                *link = rest;
            } else {
                *link = b->next;
            }

            return (uint8_t *)b + HDR;
        }

        if (heap_grow(HDR + need) != 0) {
            return NULL;
        }
    }
}

void
free(void *p)
{
    struct block *b;

    if (p == NULL) {
        return;
    }

    b = (struct block *)((uint8_t *)p - HDR);
    b->next = free_list;
    free_list = b;
}
