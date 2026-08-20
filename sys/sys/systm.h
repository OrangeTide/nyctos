/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * Kernel-internal prototypes. Nothing here is visible to userland.
 */

#ifndef _SYS_SYSTM_H_
#define _SYS_SYSTM_H_

#include <sys/op.h>
#include <sys/startup.h>

#include <stdbool.h>
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

/* sys/kern/kern_op.c */
sys_err_t sys_op_exec(struct sys_op *op);

/* sys/kern/kern_thread.c */
void thread_exited(void) __attribute__((noreturn));

/* sys/kern/subr_mem.c */
void *memset(void *dst, int c, size_t len);
void *memcpy(void *dst, const void *src, size_t len);

/* sys/kern/kern_op.c, where SYS_DEBUG_CONSOLE is compiled in */
void debug_console_set(bool live);
bool debug_console_is_live(void);

/* sys/kern/subr_bootarch.c */
struct boot_member;
const struct boot_member *boot_lookup(const char *name);
uintptr_t boot_load(const struct boot_member *m);
void boot_report(void);

/* sys/kern/kern_syscall.c */
long syscall_handler(struct sys_op *op, unsigned long count);
void trap_fatal(unsigned long cause, unsigned long pc) __attribute__((noreturn));

/* sys/arch/<arch>/machdep.S */
void enter_user(unsigned long entry, const sys_startup_t *startup)
  __attribute__((noreturn));

/* sys/arch/<arch>/halt.c */
void machine_halt(int status) __attribute__((noreturn));
void machine_reset(void) __attribute__((noreturn));

#endif /* _SYS_SYSTM_H_ */
