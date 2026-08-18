/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#include <sys/object.h>
#include <sys/systm.h>

void
kernel_main(void)
{
    cnputs("Nyctos/" ARCH_NAME " kernel booting\n");

    object_init();
    object_check();
    object_report();

    /*
     * Stage 1 ends here. Later stages put a scheduler, then message
     * passing, then a driver process behind this point. Until one of them
     * exists there is nothing to hand control to, so the machine stops
     * rather than spinning.
     */
    cnputs("stage 1 complete\n");
    machine_halt(0);
}
