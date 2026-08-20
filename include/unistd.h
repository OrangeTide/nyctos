/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

#ifndef _UNISTD_H_
#define _UNISTD_H_

#include <sys/types.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/*
 * Standard output only, and only where the debug console of section 22.4
 * is compiled in. There are no file descriptors and no open: a program
 * reaches a file through a connection handle it was given, per section
 * 25.2, and nothing here mediates that.
 */
ssize_t write(int fd, const void *buf, size_t nbytes);
void _exit(int status) __attribute__((noreturn));

#endif /* _UNISTD_H_ */
