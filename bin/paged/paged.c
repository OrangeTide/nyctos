/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * A program that reads itself.
 *
 * Its text is not in memory when it starts. Section 15.3 has each page
 * arrive when it is touched, as a message this program's own thread
 * sends to its pager without knowing it sent anything, so the first
 * instruction of this function is already the result of one.
 *
 * What it does is add up the bytes of its own image and return the sum.
 * That is a strange thing for a program to want, and it is the point:
 * whoever started it knows what those bytes are, so a checksum that
 * matches means every page arrived and arrived with the right contents
 * in it. A check that only asked whether the program ran would pass on a
 * pager that supplied blank pages.
 *
 * The image is where and how long the command line says. A program has
 * no way to ask where it was loaded, and inventing one for this would be
 * inventing an interface for a test.
 *
 * It also carries a word of initialised writable data, for M14 stage
 * five. Section 15.1 has writable segments copied eagerly and read-only
 * ones supplied on demand, and the loader could not obey that while it
 * could not see where text ended: everything with bytes in it went to
 * the pager, so a paged program with a word of data would have had that
 * word arrive read-only and faulted on writing it. Nothing had any, so
 * nothing found out. This has one, and checks it.
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
