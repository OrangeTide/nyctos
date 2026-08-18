/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Kernel-internal prototypes. Nothing here is visible to userland.
 */

#ifndef _SYS_SYSTM_H_
#define _SYS_SYSTM_H_

#include <stddef.h>

/* Console: implemented per-architecture in sys/arch/<arch>/uart.c. */
void uart_putc(char c);

/* sys/kern/subr_prf.c */
void cnputs(const char *s);
void cnwrite(const char *buf, size_t len);
void cnprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void cnvprintf(const char *fmt, __builtin_va_list ap);
void panic(const char *fmt, ...) __attribute__((noreturn))
  __attribute__((format(printf, 1, 2)));

/* sys/kern/subr_mem.c */
void *memset(void *dst, int c, size_t len);
void *memcpy(void *dst, const void *src, size_t len);

/* sys/kern/kern_exec.c */
void exec_user(void) __attribute__((noreturn));

/* sys/kern/kern_syscall.c */
long syscall_handler(long num, long a0, long a1, long a2);
void trap_fatal(unsigned long cause, unsigned long pc) __attribute__((noreturn));

/* sys/arch/<arch>/machdep.S */
void enter_user(unsigned long entry) __attribute__((noreturn));

#endif /* _SYS_SYSTM_H_ */
