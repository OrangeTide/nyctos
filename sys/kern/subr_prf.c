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
 * boot and to say something useful in a panic, and no more. It has a
 * minimum field width and no floating point.
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

static size_t
fmtnum(char *buf, size_t buflen, unsigned long long v, unsigned int base)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[24];
    size_t n = 0;
    size_t i = 0;

    do {
        tmp[i++] = digits[v % base];
        v /= base;
    } while (v != 0 && i < sizeof(tmp));

    while (i > 0 && n < buflen) {
        buf[n++] = tmp[--i];
    }

    return n;
}

static size_t
slen(const char *s)
{
    size_t n = 0;

    while (s[n] != '\0') {
        n++;
    }

    return n;
}

/*
 * Emit with padding to a minimum width. Width exists because the compiler
 * validates a format string against real printf, so a conversion this
 * formatter does not implement is accepted at compile time and goes wrong
 * only at run time, and does so by consuming the wrong argument. Supporting
 * the common case is cheaper than a trap that silently misreads varargs.
 */
static void
emit(const char *s, size_t len, int width, bool left)
{
    size_t pad = 0;

    if (width > 0 && (size_t)width > len) {
        pad = (size_t)width - len;
    }

    if (!left) {
        while (pad-- > 0) {
            cnwrite(" ", 1);
        }
    }

    cnwrite(s, len);

    if (left) {
        while (pad-- > 0) {
            cnwrite(" ", 1);
        }
    }
}

/*
 * Recognised conversions: %s %c %% and the integer forms %d %u %x, each
 * accepting the l, ll and z length modifiers, and %p for a 0x-prefixed
 * pointer. Each accepts an optional '-' and a decimal minimum width.
 * An unrecognised conversion is emitted verbatim so that a bad format
 * string is visible rather than silently swallowed.
 */
void
cnvprintf(const char *fmt, va_list ap)
{
    char numbuf[24];
    unsigned long long uv;
    long long sv;
    const char *s;
    size_t len;
    char c;
    int longs;
    int width;
    bool left;
    bool neg;

    while (*fmt != '\0') {
        if (*fmt != '%') {
            cnwrite(fmt, 1);
            fmt++;
            continue;
        }

        fmt++;
        left = false;
        width = 0;
        neg = false;

        while (*fmt == '-') {
            left = true;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = (width * 10) + (*fmt - '0');
            fmt++;
        }

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
            if (s == NULL) {
                s = "(null)";
            }
            emit(s, slen(s), width, left);
            break;
        case 'c':
            c = (char)va_arg(ap, int);
            emit(&c, 1, width, left);
            break;
        case 'd':
            if (longs >= 2) {
                sv = va_arg(ap, long long);
            } else if (longs == 1) {
                sv = va_arg(ap, long);
            } else {
                sv = va_arg(ap, int);
            }
            if (sv < 0) {
                neg = true;
                uv = (unsigned long long)(-sv);
            } else {
                uv = (unsigned long long)sv;
            }
            if (neg) {
                cnwrite("-", 1);
                if (width > 0) {
                    width--;
                }
            }
            len = fmtnum(numbuf, sizeof(numbuf), uv, 10);
            emit(numbuf, len, width, left);
            break;
        case 'u':
        case 'x':
            if (longs >= 2) {
                uv = va_arg(ap, unsigned long long);
            } else if (longs == 1) {
                uv = va_arg(ap, unsigned long);
            } else {
                uv = va_arg(ap, unsigned int);
            }
            len = fmtnum(numbuf, sizeof(numbuf), uv,
              *fmt == 'x' ? 16u : 10u);
            emit(numbuf, len, width, left);
            break;
        case 'p':
            cnputs("0x");
            len = fmtnum(numbuf, sizeof(numbuf),
              (unsigned long long)(uintptr_t)va_arg(ap, void *), 16u);
            emit(numbuf, len, width, left);
            break;
        case '%':
            cnwrite("%", 1);
            break;
        default:
            cnwrite("%", 1);
            if (*fmt != '\0') {
                cnwrite(fmt, 1);
            }
            break;
        }

        if (*fmt != '\0') {
            fmt++;
        }
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
 * The kernel has decided it cannot continue: print, then stop the machine.
 *
 * A panic is for a broken kernel invariant, and for the death of a process
 * the system cannot run without. An ordinary process failing is not a
 * panic; it terminates that process and the system carries on. Section 16
 * of design.md draws the same line for faults.
 *
 * Stopping means powering the machine off rather than spinning. A hang has
 * to be noticed by a human watching a console, whereas an exit is
 * observable by whatever started the machine.
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

    machine_halt(1);
}
