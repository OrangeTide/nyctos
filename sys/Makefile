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
        kern/subr_backtrace.c \
        kern/subr_kline.c \
        kern/subr_trace.c \
        kern/subr_pool.c \
        kern/subr_prf.c

CFLAGS   += $(KERN_CFLAGS)
CPPFLAGS := -I$(CURDIR) -DBOOT_ARCHIVE_PATH=\"$(BOOT_ARCHIVE)\"

include $(TOPDIR)/share/mk/nyc.obj.mk

all: $(KERNEL_ELF)

# The kernel is linked twice so it can carry tables of its own symbols and
# source lines. The first pass embeds empty stub blobs, so the references to
# them resolve and reach; mksymtab and mkline read that ELF into the real
# blobs, ksyms.S and klines.S embed them in rodata past the text, and the
# second pass links them in. The text addresses the blobs record do not move
# between the passes because the text comes before the rodata the blobs are
# added to, so the blobs built from the first pass are right for the second.
KERNEL_PASS1   := $(KERNEL_ELF).pass1
KSYM_STUB      := $(OBJTOP)/sys/ksym.stub.blob
KSYMS_STUB_OBJ := $(OBJDIR)/kern/ksyms_stub.o
KSYMS_OBJ      := $(OBJDIR)/kern/ksyms.o
KLINE_STUB     := $(OBJTOP)/sys/kline.stub.blob
KLINES_STUB_OBJ := $(OBJDIR)/kern/klines_stub.o
KLINES_OBJ     := $(OBJDIR)/kern/klines.o

$(KSYM_STUB): $(MKSYMTAB)
	@mkdir -p $(@D)
	$(MKSYMTAB) --empty $@

$(KSYMS_STUB_OBJ): kern/ksyms.S $(KSYM_STUB) Makefile
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DKSYM_BLOB_PATH=\"$(KSYM_STUB)\" \
	    -c $< -o $@

$(KLINE_STUB): $(MKLINE)
	@mkdir -p $(@D)
	$(MKLINE) --empty $@

$(KLINES_STUB_OBJ): kern/klines.S $(KLINE_STUB) Makefile
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DKLINE_BLOB_PATH=\"$(KLINE_STUB)\" \
	    -c $< -o $@

$(KERNEL_PASS1): $(OBJS) $(KSYMS_STUB_OBJ) $(KLINES_STUB_OBJ) $(KERN_LDSCRIPT)
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) -T $(KERN_LDSCRIPT) -o $@ $(OBJS) $(KSYMS_STUB_OBJ) \
	    $(KLINES_STUB_OBJ)

$(KSYM_BLOB): $(MKSYMTAB) $(KERNEL_PASS1)
	@mkdir -p $(@D)
	$(MKSYMTAB) $(KERNEL_PASS1) $@

$(KSYMS_OBJ): kern/ksyms.S $(KSYM_BLOB) Makefile
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DKSYM_BLOB_PATH=\"$(KSYM_BLOB)\" \
	    -c $< -o $@

$(KLINE_BLOB): $(MKLINE) $(KERNEL_PASS1)
	@mkdir -p $(@D)
	$(MKLINE) $(KERNEL_PASS1) $@

$(KLINES_OBJ): kern/klines.S $(KLINE_BLOB) Makefile
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DKLINE_BLOB_PATH=\"$(KLINE_BLOB)\" \
	    -c $< -o $@

$(KERNEL_ELF): $(OBJS) $(KSYMS_OBJ) $(KLINES_OBJ) $(KERN_LDSCRIPT)
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) -T $(KERN_LDSCRIPT) -o $@ $(OBJS) $(KSYMS_OBJ) \
	    $(KLINES_OBJ)

# Every program the archive carries, as the debug-stripped ELF the kernel
# loads. Each is a prerequisite, so an archive older than an image is
# rebuilt from the current one.
BOOT_IMAGES := \
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

# The archive the kernel embeds, built from the programs under bin/. Each
# member is named and pointed at its ELF; the tool reads the load address
# and the layout out of the ELF's program headers, so the slot a program
# was linked for stays its own Makefile's decision and is not repeated here.
# The slot size is passed so the tool can refuse an image that overruns it.
$(BOOT_ARCHIVE): $(MKBOOTARCH) $(BOOT_IMAGES)
	@mkdir -p $(@D)
	$(MKBOOTARCH) $@ $(USER_LOAD_STRIDE) \
	    fs_server@$(OBJTOP)/bin/fs_server/fs_server.strip \
	    fs_client@$(OBJTOP)/bin/fs_client/fs_client.strip \
	    fs_batch@$(OBJTOP)/bin/fs_batch/fs_batch.strip \
	    spin@$(OBJTOP)/bin/spin/spin.strip \
	    status@$(OBJTOP)/bin/status/status.strip \
	    spawner@$(OBJTOP)/bin/spawner/spawner.strip \
	    threads@$(OBJTOP)/bin/threads/threads.strip \
	    console@$(OBJTOP)/bin/console/console.strip \
	    greet@$(OBJTOP)/bin/greet/greet.strip \
	    sh@$(OBJTOP)/bin/sh/sh.strip \
	    echo@$(OBJTOP)/bin/echo/echo.strip \
	    cat@$(OBJTOP)/bin/cat/cat.strip \
	    trespass@$(OBJTOP)/bin/trespass/trespass.strip \
	    touch_addr@$(OBJTOP)/bin/touch_addr/touch_addr.strip \
	    grabber@$(OBJTOP)/bin/grabber/grabber.strip \
	    granted@$(OBJTOP)/bin/granted/granted.strip \
	    paged@$(OBJTOP)/bin/paged/paged.strip \
	    namespace@$(OBJTOP)/bin/namespace/namespace.strip \
	    resolve@$(OBJTOP)/bin/resolve/resolve.strip \
	    peer_a@$(OBJTOP)/bin/peer_a/peer_a.strip \
	    peer_b@$(OBJTOP)/bin/peer_b/peer_b.strip \
	    blk_server@$(OBJTOP)/bin/blk_server/blk_server.strip \
	    peer_c@$(OBJTOP)/bin/peer_c/peer_c.strip \
	    wedged@$(OBJTOP)/bin/wedged/wedged.strip \
	    orphan@$(OBJTOP)/bin/orphan/orphan.strip \
	    echoes@$(OBJTOP)/bin/echoes/echoes.strip \
	    supervise@$(OBJTOP)/bin/supervise/supervise.strip \
	    fs_chain@$(OBJTOP)/bin/fs_chain/fs_chain.strip \
	    brief@$(OBJTOP)/bin/brief/brief.strip \
	    poke_addr@$(OBJTOP)/bin/poke_addr/poke_addr.strip \
	    leap@$(OBJTOP)/bin/leap/leap.strip \
	    patient@$(OBJTOP)/bin/patient/patient.strip \
	    tick@$(OBJTOP)/bin/tick/tick.strip \
	    clock@$(OBJTOP)/bin/clock/clock.strip \
	    ledger@$(OBJTOP)/bin/ledger/ledger.strip \
	    patron@$(OBJTOP)/bin/patron/patron.strip \
	    twostep@$(OBJTOP)/bin/twostep/twostep.strip \
	    fourways@$(OBJTOP)/bin/fourways/fourways.strip \
	    lastborn@$(OBJTOP)/bin/lastborn/lastborn.strip \
	    ramfs@$(OBJTOP)/bin/ramfs/ramfs.strip \
	    virtio_blk@$(OBJTOP)/bin/virtio_blk/virtio_blk.strip

$(OBJDIR)/kern/bootimg.o: $(BOOT_ARCHIVE)
