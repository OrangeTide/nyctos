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
$(OBJTOP)/bin/fs_server/fs_server.strip \
$(OBJTOP)/bin/fs_client/fs_client.strip \
$(OBJTOP)/bin/fs_batch/fs_batch.strip \
$(OBJTOP)/bin/spin/spin.strip \
$(OBJTOP)/bin/status/status.strip \
$(OBJTOP)/bin/spawner/spawner.strip \
$(OBJTOP)/bin/threads/threads.strip \
$(OBJTOP)/bin/console/console.strip \
$(OBJTOP)/bin/greet/greet.strip \
$(OBJTOP)/bin/sh/sh.strip \
$(OBJTOP)/bin/echo/echo.strip \
$(OBJTOP)/bin/cat/cat.strip \
$(OBJTOP)/bin/trespass/trespass.strip \
$(OBJTOP)/bin/touch_addr/touch_addr.strip \
$(OBJTOP)/bin/grabber/grabber.strip \
$(OBJTOP)/bin/granted/granted.strip \
$(OBJTOP)/bin/paged/paged.strip \
$(OBJTOP)/bin/namespace/namespace.strip \
$(OBJTOP)/bin/resolve/resolve.strip \
$(OBJTOP)/bin/peer_a/peer_a.strip \
$(OBJTOP)/bin/peer_b/peer_b.strip \
$(OBJTOP)/bin/blk_server/blk_server.strip \
$(OBJTOP)/bin/peer_c/peer_c.strip \
$(OBJTOP)/bin/wedged/wedged.strip \
$(OBJTOP)/bin/orphan/orphan.strip \
$(OBJTOP)/bin/echoes/echoes.strip \
$(OBJTOP)/bin/supervise/supervise.strip \
$(OBJTOP)/bin/fs_chain/fs_chain.strip \
$(OBJTOP)/bin/brief/brief.strip \
$(OBJTOP)/bin/poke_addr/poke_addr.strip \
$(OBJTOP)/bin/leap/leap.strip \
    $(OBJTOP)/bin/patient/patient.strip \
    $(OBJTOP)/bin/tick/tick.strip \
    $(OBJTOP)/bin/clock/clock.strip \
    $(OBJTOP)/bin/ledger/ledger.strip \
    $(OBJTOP)/bin/patron/patron.strip \
    $(OBJTOP)/bin/twostep/twostep.strip \
    $(OBJTOP)/bin/fourways/fourways.strip \
    $(OBJTOP)/bin/lastborn/lastborn.strip \
    $(OBJTOP)/bin/ramfs/ramfs.strip \
    $(OBJTOP)/bin/virtio_blk/virtio_blk.strip

BOOT_LAYOUTS := $(BOOT_BINS:.strip=.strip.layout)

# The archive the kernel embeds, built from the programs under bin/ and
# from the load addresses their Makefiles chose. Those addresses are
# repeated here rather than exported, because a program's slot is its own
# Makefile's decision and nothing else should be able to move it.
$(BOOT_ARCHIVE): $(MKBOOTARCH) $(BOOT_BINS) $(BOOT_LAYOUTS)
	@mkdir -p $(@D)
	$(MKBOOTARCH) $@ \
	    fs_server=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 0 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/fs_server/fs_server.strip \
	    fs_client=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 1 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/fs_client/fs_client.strip \
	    fs_batch=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 2 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/fs_batch/fs_batch.strip \
	    spin=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 3 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/spin/spin.strip \
	    status=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 4 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/status/status.strip \
	    spawner=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 5 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/spawner/spawner.strip \
	    threads=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 6 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/threads/threads.strip \
	    console=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 7 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/console/console.strip \
	    greet=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 8 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/greet/greet.strip \
	    sh=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 9 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/sh/sh.strip \
	    echo=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 10 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/echo/echo.strip \
	    cat=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 11 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/cat/cat.strip \
	    trespass=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 12 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/trespass/trespass.strip \
	    touch_addr=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 13 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/touch_addr/touch_addr.strip \
	    grabber=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 14 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/grabber/grabber.strip \
	    granted=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 15 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/granted/granted.strip \
	    paged=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 16 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/paged/paged.strip \
	    namespace=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 17 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/namespace/namespace.strip \
	    resolve=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 18 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/resolve/resolve.strip \
	    peer_a=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 19 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/peer_a/peer_a.strip \
	    peer_b=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 20 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/peer_b/peer_b.strip \
	    blk_server=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 21 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/blk_server/blk_server.strip \
	    peer_c=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 22 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/peer_c/peer_c.strip \
	    wedged=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 23 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/wedged/wedged.strip \
	    orphan=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 24 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/orphan/orphan.strip \
	    echoes=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 25 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/echoes/echoes.strip \
	    supervise=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 26 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/supervise/supervise.strip \
	    fs_chain=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 27 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/fs_chain/fs_chain.strip \
	    brief=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 28 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/brief/brief.strip \
	    poke_addr=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 29 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/poke_addr/poke_addr.strip \
	    leap=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 30 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/leap/leap.strip \
	    patient=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 31 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/patient/patient.strip \
	    tick=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 32 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/tick/tick.strip \
	    clock=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 33 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/clock/clock.strip \
	    ledger=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 34 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/ledger/ledger.strip \
	    patron=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 35 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/patron/patron.strip \
	    twostep=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 36 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/twostep/twostep.strip \
	    fourways=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 37 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/fourways/fourways.strip \
	    lastborn=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 38 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/lastborn/lastborn.strip \
	    ramfs=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 39 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/ramfs/ramfs.strip \
	    virtio_blk=$(shell printf '0x%x' $$(( $(USER_LOAD_ADDR) + 40 * $(USER_LOAD_STRIDE) )))@$(OBJTOP)/bin/virtio_blk/virtio_blk.strip

$(OBJDIR)/kern/bootimg.o: $(BOOT_ARCHIVE)
