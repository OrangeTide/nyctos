/* stdlib.h : the exit() declaration.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares exit(), which does not return. Included by the C library exit
 * implementation and by the boot archive tool.
 *
 */

#ifndef _STDLIB_H_
#define _STDLIB_H_

void exit(int status) __attribute__((noreturn));

#endif /* _STDLIB_H_ */
