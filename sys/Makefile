# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# The kernel. QEMU loads it alongside a separately built initramfs image
# (from tools/mkfsimg, out of every userland program the ramdisk
# carries); the top-level Makefile places that image in RAM at
# INITRAMFS_ADDR via '-device loader' rather than linking it into the
# kernel. bin/ and tools/mkfsimg must both have been built first, and
# the top-level Makefile guarantees the ordering.

include $(TOPDIR)/share/mk/nyc.own.mk

SRCS := arch/$(ARCH)/boot.S \
        arch/$(ARCH)/exception.S \
        arch/$(ARCH)/halt.c \
        arch/$(ARCH)/machdep.S \
        arch/$(ARCH)/uart.c \
        kern/init_main.c \
        kern/kern_exec.c \
        kern/kern_file.c \
        kern/kern_object.c \
        kern/kern_syscall.c \
        kern/ramfs.c \
        kern/subr_mem.c \
        kern/subr_pool.c \
        kern/subr_prf.c \
        kern/vfs.c

CFLAGS   += $(KERN_CFLAGS) -DUSER_LOAD_ADDR=$(USER_LOAD_ADDR)UL \
            -DINITRAMFS_ADDR=$(INITRAMFS_ADDR)UL
CPPFLAGS := -I$(CURDIR)

include $(TOPDIR)/share/mk/nyc.obj.mk

all: $(KERNEL_ELF) $(INITRAMFS_IMG)

$(INITRAMFS_IMG): $(MKFSIMG) $(MOTD_BIN) $(TOPDIR)/etc/motd
	@mkdir -p $(@D)
	$(MKFSIMG) -o $@ bin/motd=$(MOTD_BIN) etc/motd=$(TOPDIR)/etc/motd

$(KERNEL_ELF): $(OBJS) $(KERN_LDSCRIPT)
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) -T $(KERN_LDSCRIPT) -o $@ $(OBJS)

