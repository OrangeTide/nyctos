/*
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Read the system page, and say what was in it.
 *
 * Section 6.2 has a page mapped read-only into every process, found
 * through the process's own description of itself rather than at an
 * address a program assumes. This program follows that pointer and
 * returns what it finds, which is how the check outside compares it
 * with what the kernel put there.
 *
 * A program that never looks at its startup record cannot reach the
 * page at all, which is the point of the pointer being there rather
 * than the address being fixed.
 */

#include <os/sys.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/syspage.h>

int
main(void)
{
    const volatile sys_syspage_t *sp = sys_syspage();

    if (sp == NULL) {
        return 1;
    }

    if (sp->version != SYS_SYSPAGE_VERSION) {
        return 2;
    }
    if (sp->hz == 0) {
        return 3;
    }

    printf("tick: version %u, %u ticks a second\n",
      (unsigned int)sp->version, (unsigned int)sp->hz);

    /*
     * The frequency is the answer, low bits first, because a status is
     * a signed number and this is what the check compares.
     */
    return (int)(sp->hz & 0x7fffffffu);
}
