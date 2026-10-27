/* paged.c : a program that checksums its own image.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Its text pages are supplied by a pager on first touch, so the code that
 * computes the sum is itself faulted in. The base address and length come
 * from the command line, and the program returns the sum of those bytes,
 * masked to 31 bits. The image also holds one word of initialised writable
 * data, which the program uses.
 *
 */

#include <os/sys.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Three steps, each on a page of its own.
 *
 * The alignment is the point. A program whose text fits in one page
 * demonstrates one fault, which is a fault rather than demand paging;
 * spread across four pages, control walks off the end of what is present
 * three times and the pages arrive in the order the program needs them.
 * The padding between them is part of the image and is summed with
 * everything else.
 */
#define ON_ITS_OWN_PAGE __attribute__((aligned(4096), noinline))

static uint32_t ON_ITS_OWN_PAGE
step_one(uint32_t v)
{
    return v * 3u + 1u;
}

static uint32_t ON_ITS_OWN_PAGE
step_two(uint32_t v)
{
    return v ^ 0x9e3779b9u;
}

static uint32_t ON_ITS_OWN_PAGE
step_three(uint32_t v)
{
    return (v >> 7) | (v << 25);
}

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

/*
 * A word the linker puts in .data, so it is inside the image's bytes
 * and above where writable memory begins. Read before the checksum and
 * written after it, because the checksum reads this memory too and a
 * word changed before it would make the sum disagree with the archive.
 */
#define SEED    0x5eed1234u

static volatile uint32_t seed = SEED;

int
main(int argc, char **argv)
{
    const volatile unsigned char *at;
    uintptr_t base, len, i;
    uint32_t sum = 0;

    if (argc != 3 || hex(argv[1], &base) != 0 || hex(argv[2], &len) != 0) {
        return -1;
    }

    /*
     * Walk through the pages before reading them, so that the text
     * arrives because the program is running in it rather than
     * because something is reading it as data. The argument comes from
     * the command line so that the calls cannot be folded away, which is
     * what happened to the first version of this: three functions on
     * three pages, and a compiler that noticed nobody wanted the answer.
     */
    if (step_three(step_two(step_one((uint32_t)len))) == 0) {
        return -2;
    }

    /* The word the archive carried, before anything has touched it. */
    if (seed != SEED) {
        return -3;
    }

    at = (const volatile unsigned char *)base;
    for (i = 0; i < len; i++) {
        sum += at[i];
    }

    /*
     * And writable, which is the half a pager could get wrong: a page
     * supplied to a fault is mapped read-only by section 15.1, so a
     * data page that arrived that way would fault here rather than a
     * page earlier.
     */
    seed = ~SEED;
    if (seed != ~SEED) {
        return -4;
    }

    /*
     * Positive, because a status is a signed number and a sum of bytes
     * is not. The top bit goes rather than the low ones, so two images
     * that differ anywhere still differ here.
     */
    return (int)(sum & 0x7fffffffu);
}
