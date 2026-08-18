/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Console output. The newline expansion that a tty line discipline would
 * normally do lives here, so neither the kernel nor userland has to know
 * that the console wants CR LF.
 *
 * The formatter is deliberately small: enough to report numbers during
 * boot and to say something useful in a panic, and no more. It has no
 * field widths, no padding, and no floating point.
 */

#include <sys/systm.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

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

static void
printnum(unsigned long long v, unsigned int base)
{
    static const char digits[] = "0123456789abcdef";
    char buf[24];
    int i = 0;

    do {
        buf[i++] = digits[v % base];
        v /= base;
    } while (v != 0);

    while (i > 0)
        cnwrite(&buf[--i], 1);
}

/*
 * Recognised conversions: %s %c %% and the integer forms %d %u %x, each
 * accepting the l, ll and z length modifiers. %p prints a 0x-prefixed
 * hexadecimal pointer. An unrecognised conversion is emitted verbatim so
 * that a bad format string is visible rather than silently swallowed.
 */
void
cnvprintf(const char *fmt, va_list ap)
{
    unsigned long long uv;
    long long sv;
    const char *s;
    char c;
    int longs;

    while (*fmt != '\0') {
        if (*fmt != '%') {
            cnwrite(fmt, 1);
            fmt++;
            continue;
        }

        fmt++;
        longs = 0;
        while (*fmt == 'l') {
            longs++;
            fmt++;
        }
        if (*fmt == 'z') {
            longs = 2;
            fmt++;
        }

        switch (*fmt) {
        case 's':
            s = va_arg(ap, const char *);
            cnputs(s != NULL ? s : "(null)");
            break;
        case 'c':
            c = (char)va_arg(ap, int);
            cnwrite(&c, 1);
            break;
        case 'd':
            if (longs >= 2)
                sv = va_arg(ap, long long);
            else if (longs == 1)
                sv = va_arg(ap, long);
            else
                sv = va_arg(ap, int);
            if (sv < 0) {
                cnputs("-");
                uv = (unsigned long long)(-sv);
            } else {
                uv = (unsigned long long)sv;
            }
            printnum(uv, 10);
            break;
        case 'u':
        case 'x':
            if (longs >= 2)
                uv = va_arg(ap, unsigned long long);
            else if (longs == 1)
                uv = va_arg(ap, unsigned long);
            else
                uv = va_arg(ap, unsigned int);
            printnum(uv, *fmt == 'x' ? 16 : 10);
            break;
        case 'p':
            cnputs("0x");
            printnum((unsigned long long)(uintptr_t)va_arg(ap, void *), 16);
            break;
        case '%':
            cnputs("%");
            break;
        default:
            cnputs("%");
            if (*fmt != '\0')
                cnwrite(fmt, 1);
            break;
        }

        if (*fmt != '\0')
            fmt++;
    }
}

void
cnprintf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    cnvprintf(fmt, ap);
    va_end(ap);
}

/*
 * Stop. There is no scheduler to hand control to and nothing to unwind, so
 * a panic prints and spins. Halting rather than resetting is deliberate
 * while bringing the system up: the message stays on the console.
 */
void
panic(const char *fmt, ...)
{
    va_list ap;

    cnputs("\npanic: ");
    va_start(ap, fmt);
    cnvprintf(fmt, ap);
    va_end(ap);
    cnputs("\n");

    for (;;) {
        /* nothing */
    }
}
