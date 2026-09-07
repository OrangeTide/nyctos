/*
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Write a word at an address it was told, which is touch_addr's other
 * half.
 *
 * M14 stage three gives a program's text the permissions text should
 * have: read and execute, and not write. Nothing could test that
 * before, because reading is all any program here did to an address it
 * was handed, and a mapping that permits everything and a mapping that
 * permits reading answer a read the same way.
 *
 * The address comes from the command line for touch_addr's reason: an
 * address this knew would be an address the build had to agree with,
 * and the interesting one is this program's own text, which only the
 * loader knows the address of.
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
            d = (uintptr_t)(*s - 'a') + 10u;
        } else if (*s >= 'A' && *s <= 'F') {
            d = (uintptr_t)(*s - 'A') + 10u;
        } else if (*s == 'x' || *s == 'X') {
            continue;
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
    volatile uint32_t *at;
    uintptr_t addr;

    if (argc != 2 || hex(argv[1], &addr) != 0) {
        printf("usage: poke_addr <hex address>\n");
        return 1;
    }

    at = (volatile uint32_t *)addr;

    /*
     * The store that either works or does not, and volatile for
     * touch_addr's reason: a compiler that saw nothing read the value
     * back could otherwise decide the whole program is nothing.
     */
    *at = 0x5eed1234u;

    printf("poke_addr: wrote to 0x%x\n", (unsigned int)addr);

    /*
     * Reaching here at all is the answer, so the status says which
     * address was written rather than what was there. A caller that
     * expected a fault reads the fault instead.
     */
    return 2;
}
