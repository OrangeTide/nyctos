/* granted.c : reads granted memory after its handle is closed.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * The program looks up the device handle, maps the region the grant covers,
 * writes a marker word there, and reads it back. It closes the handle and
 * reads the same address again, and returns 0 when that read matches. Other
 * nonzero statuses name the step that failed, and a fault on the second read
 * ends the process without a status.
 *
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
