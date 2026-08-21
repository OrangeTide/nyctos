/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * What a utility needs to talk to a person, and nothing else.
 *
 * There is no FILE, no fopen and no buffering, because there is nothing to
 * open: a program reaches the console through a handle it was given at
 * spawn, per section 20.4, and reaches a file through a connection, per
 * section 25.2. printf and readline are the two shapes utilities actually
 * use, and both go through the console of section 19.
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
