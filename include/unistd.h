/* unistd.h : console read, write, and _exit.
 *
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 *
 * Declares read(), write(), and _exit(), and defines STDIN_FILENO,
 * STDOUT_FILENO, and STDERR_FILENO. All three descriptor numbers refer to
 * the console connection the process received at spawn. Diagnostic output
 * goes through debug.h instead. Included by utilities, the filesystem
 * programs, and the C library.
 *
 */

#ifndef _UNISTD_H_
#define _UNISTD_H_

#include <sys/types.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/*
 * Input and output over the console connection the process was given
 * at spawn. All three descriptor numbers name it,
 * because there is one console and nothing else to open; a program
 * given no console reaches none, which applies to the
 * most ordinary thing a program does.
 *
 * These are not the debug console. That one has its own
 * spelling in <debug.h> and its own build switch, because a program's
 * real output and its diagnostics are different categories.
 */
ssize_t read(int fd, void *buf, size_t nbytes);
ssize_t write(int fd, const void *buf, size_t nbytes);
void _exit(int status) __attribute__((noreturn));

#endif /* _UNISTD_H_ */
