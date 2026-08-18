/*
 * SPDX-License-Identifier: 0BSD OR Apache-2.0
 * Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
 */

/*
 * The world's simplest loader, and the kernel's equivalent of running
 * /sbin/init: /bin/motd is the first (and only) userland process, looked
 * up on the initramfs QEMU places in RAM at a fixed address via
 * '-device loader' (see the top-level Makefile and INITRAMFS_ADDR in
 * share/mk/nyc.own.mk), copied to the address it was linked for, and
 * entered at unprivileged level. There is no process table yet, so when
 * it exits (sys/kern/kern_syscall.c's sys_exit()) that is the whole
 * system's exit: exactly as fatal as pid 1 exiting on a Unix system.
 */

#include <sys/systm.h>
#include <sys/vfs.h>

void
exec_user(void)
{
    struct vnode vn;
    char *dst = (char *)USER_LOAD_ADDR;
    const char *src;
    size_t i;

    vfs_init((const void *)INITRAMFS_ADDR);

    if (vfs_lookup("bin/motd", &vn) != 0) {
        cnputs("init: /bin/motd not found on initramfs\n");
        for (;;) {
            /* Nothing left to do. */
        }
    }

    src = vn.v_data;
    for (i = 0; i < vn.v_size; i++) {
        dst[i] = src[i];
    }

    cnputs("init: exec /bin/motd\n");
    enter_user(USER_LOAD_ADDR);
}
