/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 *
 * PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * A program that is given memory it did not have, and then has it taken
 * away.
 *
 * Section 7: a grant is authority over a region, held as a handle. This
 * program is handed one at spawn, asks where it landed, writes a word
 * into it, and reads it back. None of that memory is in this program's
 * image: it is a page the kernel allocated and described, which is what
 * a driver's registers are except that this one is ordinary memory and
 * can be poked without consequences.
 *
 * Then it closes the handle and reads the same address again. The handle
 * was the authority, so at a tier that enforces anything the read is the
 * last instruction this program executes. At Tier 0 there is nothing to
 * enforce and the read succeeds, which is section 6.1's bargain again:
 * the grant made the permission explicit rather than creating it.
 *
 *   0  the read after closing worked, which is Tier 0
 *   the program does not report the other case, because at Tier 2 there
 *   is no instruction after the read to report with. Whoever started it
 *   reads the fault.
 */

#include <os/sys.h>
#include <ns.h>
#include <stddef.h>
#include <stdint.h>

#define MARK    0x5eed1234u

int
main(void)
{
    volatile uint32_t *at;
    sys_handle_t h;
    uintptr_t base, len;

    h = ns_lookup(SYS_NS_DEVICE);
    if (h == SYS_HANDLE_INVALID) {
        return 2;
    }
    if (sys_grant_map(h, &base, &len) != SYS_OK) {
        return 3;
    }
    if (len < sizeof(*at)) {
        return 4;
    }

    at = (volatile uint32_t *)base;

    /* Memory this program was given, which it did not have before. */
    *at = MARK;
    if (*at != MARK) {
        return 5;
    }

    if (sys_handle_close(h) != SYS_OK) {
        return 6;
    }

    /*
     * The same address, without the handle that permitted it. What
     * happens next is the whole of what a grant means.
     */
    if (*at != MARK) {
        return 7;
    }

    return 0;
}
