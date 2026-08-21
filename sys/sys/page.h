/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Physical page frames, and the first allocator here that is not a pool.
 *
 * Section 8 gives every kernel object a pool: a fixed count of a fixed
 * type, sized at build time, so that the memory a system needs can be read
 * off its configuration. Page tables do not fit that shape. How many a
 * system needs depends on how its address spaces happen to be laid out,
 * and the same frames also become process memory and whatever a pager
 * hands out, so there is no type to count.
 *
 * What makes this acceptable rather than a hole in section 8's argument is
 * that its bound is not a guess. A pool's size is a number somebody chose;
 * this one's is the memory the machine has, which is a fact about the
 * board. Exhaustion is still a configuration error rather than a condition
 * to recover from, and it is still reported rather than waited on.
 *
 * Frames are physical addresses. Nothing here maps anything, which is the
 * port's business, and nothing here knows what a frame will be used for.
 */

#ifndef _SYS_PAGE_H_
#define _SYS_PAGE_H_

#include <sys/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE       4096u
#define PAGE_MASK       (PAGE_SIZE - 1u)

#define PAGE_ALIGN_DOWN(a)  ((uintptr_t)(a) & ~(uintptr_t)PAGE_MASK)
#define PAGE_ALIGN_UP(a)    PAGE_ALIGN_DOWN((uintptr_t)(a) + PAGE_MASK)

/*
 * Hand the allocator the memory it may use: everything from base for len
 * bytes, less whatever the kernel image and the boot archive already
 * occupy, which the caller passes as a region to hold back.
 */
void page_init(uintptr_t base, size_t len, uintptr_t hold, size_t hold_len);

/*
 * One frame, zeroed, or zero if there are none left.
 *
 * Zeroed because the
 * first use of a frame is a page table, and a page table with anything in
 * it is a page table containing somebody else's memory.
 *
 * The lowest free frame, so a frame released is the next one out. That
 * is a policy rather than a promise about which frame a caller gets,
 * and it is stated because it is what makes releasing checkable.
 */
uintptr_t page_alloc(void);

/*
 * Reserve one frame by address, for a caller that needs a particular one:
 * memory a device occupies, or memory an image was linked to sit in.
 * False means somebody has it already.
 */
bool page_take(uintptr_t pa);

/* Give one back. Releasing a frame that is not allocated is a panic: it
 * means two owners believed they had it. */
void page_free(uintptr_t pa);

/* Frames in the map, and the largest number ever allocated at once. */
uint32_t page_count(void);

/* The last frame in the map, for a check that the port's claim is true. */
uintptr_t page_last(void);
uint32_t page_used(void);
uint32_t page_highwater(void);

void page_report(void);

#endif /* _SYS_PAGE_H_ */
