/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * System call numbers. Shared verbatim by the kernel and by libc's system
 * call stubs; this header is the ABI between them. Numbers follow the
 * classic Unix syscall table (V7's exit/read/write/open/close); 2 (fork)
 * is deliberately left unassigned until this kernel has processes.
 */

#ifndef _SYS_SYSCALL_H_
#define _SYS_SYSCALL_H_

#define SYS_exit  1
#define SYS_read  3
#define SYS_write 4
#define SYS_open  5
#define SYS_close 6

#endif /* _SYS_SYSCALL_H_ */
