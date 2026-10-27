/* sys/types.h : the size_t and ssize_t types.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Defines size_t and ssize_t from the compiler's __SIZE_TYPE__ and
 * __PTRDIFF_TYPE__ macros. Included by string.h and unistd.h.
 *
 */

#ifndef _SYS_TYPES_H_
#define _SYS_TYPES_H_

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ssize_t;

#endif /* _SYS_TYPES_H_ */
