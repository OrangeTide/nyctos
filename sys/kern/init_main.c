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

    exec_user();
}
