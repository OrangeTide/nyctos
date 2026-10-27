/* tick.c : reads the system page and reports what it holds.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * It follows the pointer from its startup record to the read-only system page
 * that the kernel maps into every process. It prints the page's version and
 * tick rate, then returns the rate's low 31 bits as its status. It returns 1,
 * 2, or 3 if the page is missing, has the wrong version, or reports a zero
 * rate.
 *
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
