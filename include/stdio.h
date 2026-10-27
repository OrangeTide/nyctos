/* stdio.h : console input and output for utilities.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares puts(), printf(), vprintf(), and readline(). readline() reads
 * one line of input into a caller buffer. There is no FILE type and no
 * buffered stream. Included by utilities, the boot archive tool, and the
 * C library stdio implementation.
 *
 */

#ifndef _STDIO_H_
#define _STDIO_H_

#include <stdarg.h>

int puts(const char *s);
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vprintf(const char *fmt, va_list ap);

/*
 * One line of input, with the newline removed and the result terminated.
 * Returns the length, or -1. A line longer than the buffer arrives in
 * pieces, as it does from read().
 */
int readline(char *buf, int size);

#endif /* _STDIO_H_ */
