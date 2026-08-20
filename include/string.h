/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#ifndef _STRING_H_
#define _STRING_H_

#include <sys/types.h>

void *memcpy(void *dst, const void *src, size_t len);
void *memset(void *b, int c, size_t len);
size_t strlen(const char *s);
int strncmp(const char *a, const char *b, size_t n);

#endif /* _STRING_H_ */
