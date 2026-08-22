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

/* Where a member's bytes are, for a caller that copies them elsewhere. */
const uint8_t *boot_payload(const struct boot_member *m);

/* Hold back the memory the archive's programs are linked to run in. */
void boot_reserve(void);

/* The span of addresses loaded programs occupy; see subr_bootarch.c. */
void boot_user_window(uintptr_t *base, size_t *len);

/* Where the first member is linked to run. */
uintptr_t boot_first_load(void);
void boot_report(void);

/* sys/kern/kern_syscall.c */
long syscall_handler(struct sys_op *op, unsigned long count);
void trap_fatal(unsigned long cause, unsigned long pc, unsigned long addr)
  __attribute__((noreturn));

/*
 * A synchronous exception taken while a program was running. It ends
 * that thread, per section 16.2, and does not return; the kernel is not
 * involved beyond deciding what kind of fault it was and saying so.
 */
void trap_user(unsigned long cause, unsigned long pc, unsigned long addr)
  __attribute__((noreturn));

/*
 * Which kind of section 16.1 an exception is, and whether the address
 * the port reported means anything. Every exception a port can raise
 * maps to a kind; see sys/arch/<arch>/fault_md.c.
 */
uint32_t md_fault_kind(unsigned long cause);
bool md_fault_has_addr(unsigned long cause);

/*
 * The memory the machine has, which a port states rather than
 * discovers. Section 27 blesses that for a pinned target: discovering
 * a constant is not an improvement on stating one, and the stage that
 * reads a device tree changes this function and nothing above it.
 *
 * The kernel image, the boot archive inside it and the boot stack are
 * the region held back, which is everything from the load address to
 * the end of the linker script's layout.
 */
/* The privilege level this kernel runs at, for the boot log. */
const char *md_mode(void);

/* The address space the hardware is translating through. */
unsigned long md_current_space(void);

/*
 * The kernel image, split at the boundary between what it executes and
 * what it writes. See sys/arch/<arch>/mem_md.c.
 */
void md_kernel_image(uintptr_t *text, uintptr_t *rx_end, uintptr_t *end);

void md_memory_map(uintptr_t *base, size_t *len, uintptr_t *hold,
  size_t *hold_len);

/* sys/arch/<arch>/machdep.S */
void enter_user(unsigned long entry, void *arg, unsigned long sp)
  __attribute__((noreturn));

/* sys/arch/<arch>/halt.c */
void machine_halt(int status) __attribute__((noreturn));
void machine_reset(void) __attribute__((noreturn));

#endif /* _SYS_SYSTM_H_ */
