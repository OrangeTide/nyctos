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
        arch/$(ARCH)/fault_md.c \
        arch/$(ARCH)/halt.c \
        arch/$(ARCH)/intr.c \
        arch/$(ARCH)/machdep.S \
        arch/$(ARCH)/mem_md.c \
        arch/$(ARCH)/cpu_md.c \
        arch/$(ARCH)/pmap.c \
        arch/$(ARCH)/swtch.S \
        arch/$(ARCH)/thread_md.c \
        arch/$(ARCH)/uart.c \
        kern/bootimg.S \
        kern/init_main.c \
        kern/kern_channel.c \
        kern/kern_cohort.c \
        kern/kern_cpu.c \
        kern/kern_lock.c \
        kern/kern_measure.c \
        kern/kern_connection.c \
        kern/kern_grant.c \
        kern/kern_handle.c \
        kern/kern_intr.c \
        kern/kern_object.c \
        kern/kern_op.c \
        kern/kern_prepared.c \
        kern/kern_pager.c \
        kern/kern_batchreg.c \
        kern/kern_objref.c \
        kern/kern_proc.c \
        kern/kern_timeline.c \
        kern/kern_sched.c \
        kern/kern_syscall.c \
        kern/kern_thread.c \
        kern/kern_time.c \
        kern/subr_mem.c \
        kern/subr_page.c \
        kern/subr_vm.c \
        kern/subr_bootarch.c \
        kern/subr_trace.c \
        kern/subr_pool.c \
        kern/subr_prf.c

CFLAGS   += $(KERN_CFLAGS)
CPPFLAGS := -I$(CURDIR) -DBOOT_ARCHIVE_PATH=\"$(BOOT_ARCHIVE)\"

include $(TOPDIR)/share/mk/nyc.obj.mk

all: $(KERNEL_ELF)

$(KERNEL_ELF): $(OBJS) $(KERN_LDSCRIPT)
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) -T $(KERN_LDSCRIPT) -o $@ $(OBJS)

# Every program the archive carries, and beside each the layout its
# link recorded. Both are prerequisites: the archive bakes in the
# numbers, so an archive older than a layout carries numbers that are
# no longer true. Building M14 stage one hit that twice, and both
# times it read as a result rather than as a stale build.
BOOT_BINS := \
$(OBJTOP)/bin/fs_server/fs_server.bin \
$(OBJTOP)/bin/fs_client/fs_client.bin \
$(OBJTOP)/bin/fs_batch/fs_batch.bin \
$(OBJTOP)/bin/spin/spin.bin \
$(OBJTOP)/bin/status/status.bin \
$(OBJTOP)/bin/spawner/spawner.bin \
$(OBJTOP)/bin/threads/threads.bin \
$(OBJTOP)/bin/console/console.bin \
$(OBJTOP)/bin/greet/greet.bin \
$(OBJTOP)/bin/sh/sh.bin \
$(OBJTOP)/bin/echo/echo.bin \
$(OBJTOP)/bin/cat/cat.bin \
$(OBJTOP)/bin/trespass/trespass.bin \
$(OBJTOP)/bin/touch_addr/touch_addr.bin \
$(OBJTOP)/bin/grabber/grabber.bin \
$(OBJTOP)/bin/granted/granted.bin \
$(OBJTOP)/bin/paged/paged.bin \
$(OBJTOP)/bin/namespace/namespace.bin \
$(OBJTOP)/bin/resolve/resolve.bin \
$(OBJTOP)/bin/peer_a/peer_a.bin \
$(OBJTOP)/bin/peer_b/peer_b.bin \
$(OBJTOP)/bin/blk_server/blk_server.bin \
$(OBJTOP)/bin/peer_c/peer_c.bin \
$(OBJTOP)/bin/wedged/wedged.bin \
$(OBJTOP)/bin/orphan/orphan.bin \
$(OBJTOP)/bin/echoes/echoes.bin \
$(OBJTOP)/bin/supervise/supervise.bin \
$(OBJTOP)/bin/fs_chain/fs_chain.bin \
$(OBJTOP)/bin/brief/brief.bin

BOOT_LAYOUTS := $(BOOT_BINS:.bin=.bin.layout)

# The archive the kernel embeds, built from the programs under bin/ and
# from the load addresses their Makefiles chose. Those addresses are
# repeated here rather than exported, because a program's slot is its own
# Makefile's decision and nothing else should be able to move it.
$(BOOT_ARCHIVE): $(MKBOOTARCH) $(BOOT_BINS) $(BOOT_LAYOUTS)
	@mkdir -p $(@D)
	$(MKBOOTARCH) $@ \
	    fs_server=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 0 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/fs_server/fs_server.bin \
	    fs_client=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 1 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/fs_client/fs_client.bin \
	    fs_batch=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 2 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/fs_batch/fs_batch.bin \
	    spin=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 3 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/spin/spin.bin \
	    status=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 4 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/status/status.bin \
	    spawner=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 5 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/spawner/spawner.bin \
	    threads=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 6 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/threads/threads.bin \
	    console=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 7 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/console/console.bin \
	    greet=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 8 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/greet/greet.bin \
	    sh=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 9 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/sh/sh.bin \
	    echo=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 10 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/echo/echo.bin \
	    cat=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 11 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/cat/cat.bin \
	    trespass=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 12 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/trespass/trespass.bin \
	    touch_addr=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 13 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/touch_addr/touch_addr.bin \
	    grabber=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 14 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/grabber/grabber.bin \
	    granted=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 15 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/granted/granted.bin \
	    paged=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 16 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/paged/paged.bin \
	    namespace=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 17 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/namespace/namespace.bin \
	    resolve=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 18 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/resolve/resolve.bin \
	    peer_a=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 19 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/peer_a/peer_a.bin \
	    peer_b=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 20 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/peer_b/peer_b.bin \
	    blk_server=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 21 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/blk_server/blk_server.bin \
	    peer_c=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 22 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/peer_c/peer_c.bin \
	    wedged=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 23 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/wedged/wedged.bin \
	    orphan=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 24 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/orphan/orphan.bin \
	    echoes=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 25 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/echoes/echoes.bin \
	    supervise=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 26 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/supervise/supervise.bin \
	    fs_chain=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 27 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/fs_chain/fs_chain.bin \
	    brief=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 28 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/brief/brief.bin

$(OBJDIR)/kern/bootimg.o: $(BOOT_ARCHIVE)
