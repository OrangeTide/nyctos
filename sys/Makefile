# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# The kernel. It is the whole boot image: no userland is built into it and
# none is loaded alongside it, because nothing runs in user mode yet. The
# stage that spawns the first process brings that back.

include $(TOPDIR)/share/mk/nyc.own.mk

SRCS := arch/$(ARCH)/boot.S \
        arch/$(ARCH)/exception.S \
        arch/$(ARCH)/halt.c \
        arch/$(ARCH)/intr.c \
        arch/$(ARCH)/machdep.S \
        arch/$(ARCH)/swtch.S \
        arch/$(ARCH)/thread_md.c \
        arch/$(ARCH)/uart.c \
        kern/init_main.c \
        kern/kern_channel.c \
        kern/kern_handle.c \
        kern/kern_intr.c \
        kern/kern_object.c \
        kern/kern_op.c \
        kern/kern_objref.c \
        kern/kern_proc.c \
        kern/kern_timeline.c \
        kern/kern_sched.c \
        kern/kern_syscall.c \
        kern/kern_thread.c \
        kern/kern_time.c \
        kern/subr_mem.c \
        kern/subr_pool.c \
        kern/subr_prf.c

CFLAGS   += $(KERN_CFLAGS)
CPPFLAGS := -I$(CURDIR)

include $(TOPDIR)/share/mk/nyc.obj.mk

all: $(KERNEL_ELF)

$(KERNEL_ELF): $(OBJS) $(KERN_LDSCRIPT)
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) -T $(KERN_LDSCRIPT) -o $@ $(OBJS)
