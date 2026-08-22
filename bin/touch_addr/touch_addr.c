/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Read a word at an address it was told, and say what was there.
 *
 * The address comes from the command line because the point is one
 * program behaving two ways: the same image, the same instruction, an
 * address that is somebody else's memory. At Tier 0 there is one address
 * space and it reads whatever is there. At Tier 2 the hardware refuses
 * and section 16.2 ends the thread.
 *
 * That is section 6.1's argument in two runs, and it is the reason this
 * program takes the address rather than knowing one: an address it knew
 * would be an address the build had to agree with, and the interesting
 * ones belong to whoever the loader put there.
 */

#include <os/sys.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

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
    volatile const uint32_t *at;
    uintptr_t addr;
    uint32_t word;

    if (argc != 2 || hex(argv[1], &addr) != 0) {
        printf("usage: touch_addr <hex address>\n");
        return 1;
    }

    at = (volatile const uint32_t *)addr;

    /*
     * The load that either works or does not. Volatile so that it
     * happens: a compiler that noticed the value was only printed could
     * otherwise sink it past the print, and the whole program is this
     * one instruction.
     */
    word = *at;

    printf("touch_addr: 0x%x at 0x%x\n", (unsigned int)word,
      (unsigned int)addr);

    /*
     * The word itself is the exit status. There is no console for
     * a program started by the kernel's own checks, so the print
     * above reaches a person running this from a shell and nothing
     * else; a caller that wants to know what was there reads the
     * status, and a caller that wanted to know whether the read
     * happened at all can compare it with the same address.
     */
    return (int)word;
}
