/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Enough formatting for a utility to be worth writing.
 *
 * The kernel has a formatter of its own in subr_prf.c and this is a second
 * one, which is duplication with a reason: that one writes a byte at a
 * time to a device it owns, and this one has to gather bytes into a
 * message, because every write here is a rendezvous with another process.
 * Sharing the code would mean a formatter parameterised by how it emits,
 * which is more machinery than either side saves.
 *
 * The conversions are the ones utilities use: %s, %d, %u, %x, %c and %%.
 * A width is honoured only as a minimum with zero or space padding, which
 * is what a column of numbers needs. Anything else is printed as itself,
 * so a mistake in a format string is visible rather than silent.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BUF_BYTES   128

struct out {
    char     buf[BUF_BYTES];
    unsigned len;
    int      written;
    int      failed;
};

static void
flush(struct out *o)
{
    if (o->len == 0 || o->failed) {
        return;
    }
    if (write(STDOUT_FILENO, o->buf, o->len) < 0) {
        o->failed = 1;
        return;
    }
    o->written += (int)o->len;
    o->len = 0;
}

static void
emit(struct out *o, char c)
{
    if (o->len == BUF_BYTES) {
        flush(o);
    }
    if (o->failed) {
        return;
    }
    o->buf[o->len++] = c;
}

static void
emit_str(struct out *o, const char *s, unsigned width, char pad)
{
    unsigned len = (unsigned)strlen(s);

    while (len < width) {
        emit(o, pad);
        width--;
    }
    while (*s != '\0') {
        emit(o, *s++);
    }
}

/*
 * Unsigned in any base up to sixteen, written backwards into a local and
 * reversed on the way out, which is the whole of what a formatter needs to
 * do about numbers.
 */
static void
emit_num(struct out *o, unsigned long v, unsigned base, int negative,
  unsigned width, char pad)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[24];
    unsigned n = 0;

    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v != 0 && n < sizeof(tmp));

    if (negative) {
        /*
         * The sign goes before the padding when the padding is zeroes and
         * after it when it is spaces, which is the one place a formatter
         * that pads numbers has to know the difference.
         */
        if (pad == '0') {
            emit(o, '-');
        } else {
            tmp[n++] = '-';
        }
    }

    while (n < width) {
        emit(o, pad);
        width--;
    }
    while (n > 0) {
        emit(o, tmp[--n]);
    }
}

int
vprintf(const char *fmt, va_list ap)
{
    struct out o;
    unsigned width;
    char pad;

    memset(&o, 0, sizeof(o));

    while (*fmt != '\0') {
        if (*fmt != '%') {
            emit(&o, *fmt++);
            continue;
        }
        fmt++;

        pad = ' ';
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (unsigned)(*fmt++ - '0');
        }

        switch (*fmt++) {
        case 's':
            emit_str(&o, va_arg(ap, const char *), width, pad);
            break;
        case 'd': {
            int v = va_arg(ap, int);

            if (v < 0) {
                emit_num(&o, (unsigned long)(-(long)v), 10, 1, width, pad);
            } else {
                emit_num(&o, (unsigned long)v, 10, 0, width, pad);
            }
            break;
        }
        case 'u':
            emit_num(&o, va_arg(ap, unsigned int), 10, 0, width, pad);
            break;
        case 'x':
            emit_num(&o, va_arg(ap, unsigned int), 16, 0, width, pad);
            break;
        case 'c':
            emit(&o, (char)va_arg(ap, int));
            break;
        case '%':
            emit(&o, '%');
            break;
        default:
            /*
             * Not a conversion this knows. The argument list is now out of
             * step with the format string and nothing can recover it, so
             * what is printed says so rather than guessing.
             */
            emit_str(&o, "%?", 0, ' ');
            break;
        }
    }

    flush(&o);

    return o.failed ? -1 : o.written;
}

int
printf(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vprintf(fmt, ap);
    va_end(ap);

    return n;
}
