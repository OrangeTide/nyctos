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

ssize_t write(int fd, const void *buf, size_t nbytes);
int open(const char *path, int flags);
ssize_t read(int fd, void *buf, size_t nbytes);
int close(int fd);
void _exit(int status) __attribute__((noreturn));

#endif /* _UNISTD_H_ */
