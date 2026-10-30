/* stdlib.h : exit() and the heap allocator.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares exit(), which does not return, and malloc() and free() over
 * the process's growable heap. Included by the C library and by the boot
 * archive tool.
 *
 */

#ifndef _STDLIB_H_
#define _STDLIB_H_

#include <stddef.h>

void exit(int status) __attribute__((noreturn));

/*
 * Return a block of at least n bytes, aligned for any type, or NULL when
 * n is zero or the heap cannot grow to hold it. free() returns a block
 * from malloc() to be handed out again; freeing NULL does nothing.
 */
void *malloc(size_t n);
void free(void *p);

#endif /* _STDLIB_H_ */
