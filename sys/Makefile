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
        kern/bootimg.S \
        kern/init_main.c \
        kern/kern_channel.c \
        kern/kern_connection.c \
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
        kern/subr_bootarch.c \
        kern/subr_pool.c \
        kern/subr_prf.c

CFLAGS   += $(KERN_CFLAGS)
CPPFLAGS := -I$(CURDIR) -DBOOT_ARCHIVE_PATH=\"$(BOOT_ARCHIVE)\"

include $(TOPDIR)/share/mk/nyc.obj.mk

all: $(KERNEL_ELF)

$(KERNEL_ELF): $(OBJS) $(KERN_LDSCRIPT)
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) -T $(KERN_LDSCRIPT) -o $@ $(OBJS)

# The archive the kernel embeds, built from the programs under bin/ and
# from the load addresses their Makefiles chose. Those addresses are
# repeated here rather than exported, because a program's slot is its own
# Makefile's decision and nothing else should be able to move it.
$(BOOT_ARCHIVE): $(MKBOOTARCH) \
    $(OBJTOP)/bin/fs_server/fs_server.bin \
    $(OBJTOP)/bin/fs_client/fs_client.bin
	@mkdir -p $(@D)
	$(MKBOOTARCH) $@ \
	    fs_server=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 0 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/fs_server/fs_server.bin \
	    fs_client=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 1 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/fs_client/fs_client.bin

$(OBJDIR)/kern/bootimg.o: $(BOOT_ARCHIVE)
