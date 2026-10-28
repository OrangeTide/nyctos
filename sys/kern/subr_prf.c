/* subr_prf.c : console output and the formatted print routines.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Formats numbers and strings to the console through cnprintf, cnvprintf
 * and cnputs, expanding newlines to CR LF, with a minimum field width
 * and no floating point. Output is locked per message, so messages from
 * different cores do not interleave. panic prints its message and halts
 * the machine, and detects re-entry.
 *
 */
#include <sys/backtrace.h>
#include <sys/cpu.h>
#include <sys/lock.h>
#include <sys/measure.h>
#include <sys/systm.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * One core at a time, for the length of a message rather than the length
 * of a character. Two cores printing at once produced a line neither of
 * them wrote: a panic from the second core arrived shuffled into the
 * first core's output, one fragment at a time, and could not be read.
 *
 * The unit is a message because that is the unit a reader reads. Locking
 * each character would still let two lines arrive woven together, which
 * is the defect and not a smaller version of it.
 *
 * Messages nest: a panic prints a prefix, a body and a newline, and the
 * formatter prints each piece of what it formats. So the lock is taken by
 * the outermost call and the depth in the core is what knows which one
 * that was. Interrupts are off throughout, because a handler that printed
 * while this core held the lock would wait for itself.
 *
 * Before cpu_init there is no core structure and no second core, so the
 * early boot messages take nothing and rely on being alone.
 */
#if SYS_TIER == 2
static struct spinlock console_lock = { 0, 0, "console" };
#endif

static intr_state_t
console_enter(void)
{
    intr_state_t s = md_intr_disable();
    struct cpu *c = cpu_self();

    /*
     * A message printed with interrupts on is a region of its own, and
     * a long one: a line onto a polled UART is the longest region this
     * kernel has. It is charged to the console rather than to the
     * ordinary interrupts-off figure, and both are reported. Only when this call was the
     * one that turned them off, because a message printed inside
     * somebody else's region belongs to whoever began it.
     */
    if (s != 0) {
        intr_region_console();
    }

    if (c != NULL && c->cp_cn_depth++ == 0) {
#if SYS_TIER == 2
        spin_lock(&console_lock);
#endif
    }

    return s;
}

static void
console_leave(intr_state_t saved)
{
    struct cpu *c = cpu_self();

    if (c != NULL && --c->cp_cn_depth == 0) {
#if SYS_TIER == 2
        spin_unlock(&console_lock);
#endif
    }

    md_intr_restore(saved);
}

/*
 * The panic path waits for a bounded time, then prints regardless.
 *
 * A core that stops the machine must say why, so it cannot wait for a
 * lock forever: if the core holding it is the broken one, the panic
 * would never appear at all. But the ordinary case is a core partway
 * through an ordinary message, which will be finished shortly,
 * and barging into that is what made a panic unreadable in the first
 * place. So it waits, and the bound is what it costs to be wrong: long
 * enough that no real message is still running, short enough that a
 * human watching a wedged machine does not conclude it printed nothing.
 * The longest message this system prints holds the console for about a
 * millisecond, which the interrupts-off report names, and this bound is
 * several hundred times that.
 *
 * Nothing gives it back. panic does not return.
 */
#define CONSOLE_PANIC_SPINS 20000000u

static void
console_seize(void)
{
    struct cpu *c = cpu_self();

    (void)md_intr_disable();

    if (c == NULL || c->cp_cn_depth > 0) {
        return;
    }

    c->cp_cn_depth = 1;
#if SYS_TIER == 2
    {
        unsigned int i;

        for (i = 0; i < CONSOLE_PANIC_SPINS; i++) {
            if (spin_trylock(&console_lock)) {
                return;
            }
            md_cpu_relax();
        }
    }
#endif
}

/* Characters onto the port, with the newline expansion. Caller holds it. */
static void
cn_raw(const char *buf, size_t len)
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
cnwrite(const char *buf, size_t len)
{
    intr_state_t s = console_enter();

    cn_raw(buf, len);
    console_leave(s);
}

void
cnputs(const char *s)
{
    intr_state_t st = console_enter();

    while (*s != '\0') {
        cn_raw(s, 1);
        s++;
    }

    console_leave(st);
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
            cn_raw(" ", 1);
        }
    }

    cn_raw(s, len);

    if (left) {
        while (pad-- > 0) {
            cn_raw(" ", 1);
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
    const char *np;
    size_t len;
    char c;
    int longs;
    int width;
    bool left;
    bool neg;
    intr_state_t cn = console_enter();

    while (*fmt != '\0') {
        if (*fmt != '%') {
            cn_raw(fmt, 1);
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

            /*
             * The sign is part of the number for padding purposes, so it
             * goes into the buffer rather than being written ahead of it.
             * Emitting it first puts the padding between the sign and the
             * digits, which is where -10 in a width of five becomes
             * "-  10" instead of "  -10".
             */
            len = fmtnum(&numbuf[1], sizeof(numbuf) - 1, uv, 10);
            np = &numbuf[1];
            if (neg) {
                numbuf[0] = '-';
                np = &numbuf[0];
                len++;
            }
            emit(np, len, width, left);
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
            cn_raw("%", 1);
            break;
        default:
            cn_raw("%", 1);
            if (*fmt != '\0') {
                cn_raw(fmt, 1);
            }
            break;
        }

        if (*fmt != '\0') {
            fmt++;
        }
    }

    console_leave(cn);
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
 * Whether this core is already panicking. Per core, because a second
 * core with its own reason to stop has something to say and should say
 * it; and with a fallback for the boot path, where there is no core
 * structure to ask yet.
 */
static bool
panic_reentered(void)
{
    static bool early;
    struct cpu *c = cpu_self();

    if (c == NULL) {
        if (early) {
            return true;
        }
        early = true;
        return false;
    }

    if (c->cp_panicking) {
        return true;
    }
    c->cp_panicking = true;

    return false;
}

/*
 * The kernel has decided it cannot continue: print, then stop the machine.
 *
 * A panic is for a broken kernel invariant, and for the death of a process
 * the system cannot run without. An ordinary process failing is not a
 * panic; it terminates that process and the system carries on.
 *
 * Stopping means powering the machine off rather than spinning. A hang has
 * to be noticed by a human watching a console, whereas an exit is
 * observable by whatever started the machine.
 */
void
panic(const char *fmt, ...)
{
    va_list ap;

    /*
     * Whatever else is being printed, this gets printed. The rest of
     * this message nests inside the hold taken here, so the three
     * calls below arrive as one piece.
     */
    console_seize();

    /*
     * And if this core was already in here, the fault that brought it
     * back is in the message itself. Say so in a form that cannot
     * fault, and stop.
     */
    if (panic_reentered()) {
        cnputs("\npanic while panicking\n");
        machine_halt(1);
    }

    cnputs("\npanic: ");
    va_start(ap, fmt);
    cnvprintf(fmt, ap);
    va_end(ap);
    cnputs("\n");

    /*
     * The call chain that reached here, as a record of return addresses. A
     * host tool reads these against the kernel ELF and names each frame.
     */
    {
        uintptr_t frames[16];
        unsigned int n = kbacktrace(frames, 16), k;

        cnputs("backtrace:");
        for (k = 0; k < n; k++) {
            cnprintf(" 0x%lx", (unsigned long)frames[k]);
        }
        cnputs("\n");
    }

    machine_halt(1);
}
