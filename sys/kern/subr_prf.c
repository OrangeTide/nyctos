/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Console output. The newline expansion that a tty line discipline would
 * normally do lives here, so neither the kernel nor userland has to know
 * that the console wants CR LF.
 */

#include <sys/systm.h>

void
cnwrite(const char *buf, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            uart_putc('\r');
        }
        uart_putc(buf[i]);
    }
}

void
cnputs(const char *s)
{
    while (*s != '\0') {
        cnwrite(s, 1);
        s++;
    }
}
