# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# Toolchain, target and directory layout shared by every component of the
# tree (kernel, libc, csu and userland programs). Included first by every
# Makefile; it defines variables only, never rules.

.DEFAULT_GOAL := all

ARCH ?= aarch64

# Which protection tier this image is built for. Section 6: Tier 0 is
# flat memory that trusts the contract, Tier 2 is an MMU that enforces
# it. The same source builds both, which is the claim the whole design
# rests on, so both are built and both are checked.
TIER ?= 0

# The span of memory each loaded program is given, which is what its
# linker script places it at and what the kernel holds back from the
# frame allocator. Defined here, above every use, because a := that
# expands it before it exists expands to nothing and the kernel then
# holds back a slot of zero bytes.
USER_LOAD_STRIDE := 0x00100000

VALID_TIERS := 0 2
ifeq ($(filter $(TIER),$(VALID_TIERS)),)
$(error Unknown TIER '$(TIER)'; expected one of: $(VALID_TIERS))
endif

VALID_ARCHS := aarch64 riscv64
ifeq ($(filter $(ARCH),$(VALID_ARCHS)),)
$(error Unknown ARCH '$(ARCH)'; expected one of: $(VALID_ARCHS))
endif

# Homebrew's LLVM and LLD are keg-only (not linked into PATH) because they
# would shadow Xcode's clang, so locate them explicitly.
LLVM_PREFIX := $(shell brew --prefix llvm 2>/dev/null)
LLD_PREFIX  := $(shell brew --prefix lld 2>/dev/null)
ifneq ($(LLVM_PREFIX),)
CC      := $(LLVM_PREFIX)/bin/clang
AR      := $(LLVM_PREFIX)/bin/llvm-ar
OBJCOPY := $(LLVM_PREFIX)/bin/llvm-objcopy
NM      := $(LLVM_PREFIX)/bin/llvm-nm
else
CC      := clang
AR      := llvm-ar
OBJCOPY := llvm-objcopy
NM      := llvm-nm
endif
ifneq ($(LLD_PREFIX),)
LD := $(LLD_PREFIX)/bin/ld.lld
else
LD := ld.lld
endif

# mkfsimg is a host tool (see tools/mkfsimg): it builds the initramfs image
# on the machine doing the build, so it uses the host's own cc rather than
# the cross toolchain above.
HOSTCC ?= cc

CFLAGS  := -ffreestanding -fno-builtin -fno-stack-protector -nostdlib \
           -Wall -Wextra -O2 -g
LDFLAGS := -nostdlib -static

# ---------------------------------------------------------- per-arch config --
# KERN_CFLAGS and USER_CFLAGS differ deliberately: the kernel keeps the
# hard-float ABI it has always been built with, while userland is built
# soft-float so that no FPU state has to be enabled before entering an
# unprivileged program.
ifeq ($(ARCH),aarch64)
TARGET_TRIPLE   := aarch64-none-elf
MACHINE_CFLAGS  := --target=$(TARGET_TRIPLE) -mgeneral-regs-only \
                   -DARCH_NAME=\"aarch64\"
# -mstrict-align: with the MMU off, aarch64 treats all of memory as
# Device-nGnRnE, where an unaligned access faults rather than being fixed
# up in hardware. Without it the compiler may merge two adjacent 32-bit
# struct fields into one 64-bit store that is only 4-byte aligned, which
# faults at boot. Userland runs with no memory protection behind it and
# has the same constraint. RISC-V gets the flag for the same reason.
KERN_CFLAGS     := $(MACHINE_CFLAGS) -mstrict-align -DSYS_TIER=$(TIER) \
                   -DSYS_USER_SLOT_BYTES=$(USER_LOAD_STRIDE)
USER_CFLAGS     := $(MACHINE_CFLAGS) -mstrict-align \
                   -DSYS_USER_SLOT_BYTES=$(USER_LOAD_STRIDE)
USER_LOAD_ADDR  := 0x40200000
QEMU            := qemu-system-aarch64
QEMU_FLAGS      := -M virt -cpu cortex-a53 -nographic
endif

ifeq ($(ARCH),riscv64)
TARGET_TRIPLE   := riscv64-none-elf
MACHINE_CFLAGS  := --target=$(TARGET_TRIPLE) -mcmodel=medany \
                   -DARCH_NAME=\"riscv64\"
KERN_CFLAGS     := $(MACHINE_CFLAGS) -march=rv64gc -mabi=lp64d \
                   -mstrict-align -DSYS_TIER=$(TIER) \
                   -DSYS_USER_SLOT_BYTES=$(USER_LOAD_STRIDE)
USER_CFLAGS     := $(MACHINE_CFLAGS) -march=rv64imac -mabi=lp64 \
                   -mstrict-align -DSYS_USER_SLOT_BYTES=$(USER_LOAD_STRIDE)
USER_LOAD_ADDR  := 0x80200000
QEMU            := qemu-system-riscv64
QEMU_FLAGS      := -M virt -bios none -nographic
endif

# How many cores the emulator offers, which follows the tier because
# section 13.3 does: Tier 0 disables interrupts inside the kernel and
# bounds latency by where the preemption points are, on the argument
# that such parts are simple and often uniprocessor, and Tier 2 enables
# them and locks instead because multiprocessing forces locking anyway.
# The kernel is built for the same number, in sys/config.h.
ifeq ($(TIER),2)
QEMU_FLAGS      += -smp 2
endif

# Every build product for this architecture lives under its own directory,
# keyed by target triplet, so architectures never collide. The tree below
# OBJTOP mirrors the source tree.
# The tier is in the path for the same reason the architecture is: the
# two images share no object files, and a build that switched tiers
# without noticing would link half of each.
OBJTOP := $(TOPDIR)/_out/$(TARGET_TRIPLE)-tier$(TIER)

# Staging area for things one component builds and another consumes,
# playing the role of /usr/lib on an installed system.
LIBDIR := $(OBJTOP)/lib
LIBC   := $(LIBDIR)/libc.a
CRT1   := $(LIBDIR)/crt1.o


USER_LDSCRIPT := $(TOPDIR)/share/ld/user-$(ARCH).ld
KERN_LDSCRIPT := $(TOPDIR)/sys/arch/$(ARCH)/conf/kern.ld

KERNEL_ELF := $(OBJTOP)/sys/kernel.elf

# The boot archive of section 24.4, and the host tool that writes it. The
# kernel embeds the archive, so it is built after the programs that go into
# it and before the kernel that carries it.
MKBOOTARCH   := $(OBJTOP)/tools/mkbootarch/mkbootarch
BOOT_ARCHIVE := $(OBJTOP)/sys/boot.img

