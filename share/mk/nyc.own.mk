# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# Toolchain, target and directory layout shared by every component of the
# tree (kernel, libc, csu and userland programs). Included first by every
# Makefile; it defines variables only, never rules.

.DEFAULT_GOAL := all

ARCH ?= aarch64

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
else
CC      := clang
AR      := llvm-ar
OBJCOPY := llvm-objcopy
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
KERN_CFLAGS     := $(MACHINE_CFLAGS)
USER_CFLAGS     := $(MACHINE_CFLAGS)
# Must match share/ld/user-aarch64.ld.
USER_LOAD_ADDR  := 0x40200000
# Well clear of the kernel image (linked at 0x40080000) and below
# USER_LOAD_ADDR; QEMU's generic -device loader places the initramfs image
# here, and sys/kern/kern_exec.c reads it from there directly.
INITRAMFS_ADDR  := 0x40100000
QEMU            := qemu-system-aarch64
QEMU_FLAGS      := -M virt -cpu cortex-a53 -nographic
endif

ifeq ($(ARCH),riscv64)
TARGET_TRIPLE   := riscv64-none-elf
MACHINE_CFLAGS  := --target=$(TARGET_TRIPLE) -mcmodel=medany \
                   -DARCH_NAME=\"riscv64\"
KERN_CFLAGS     := $(MACHINE_CFLAGS) -march=rv64gc -mabi=lp64d
USER_CFLAGS     := $(MACHINE_CFLAGS) -march=rv64imac -mabi=lp64
# Must match share/ld/user-riscv64.ld.
USER_LOAD_ADDR  := 0x80200000
# Well clear of the kernel image (linked at 0x80000000) and below
# USER_LOAD_ADDR; QEMU's generic -device loader places the initramfs image
# here, and sys/kern/kern_exec.c reads it from there directly.
INITRAMFS_ADDR  := 0x80100000
QEMU            := qemu-system-riscv64
QEMU_FLAGS      := -M virt -bios none -nographic
endif

# Every build product for this architecture lives under its own directory,
# keyed by target triplet, so architectures never collide. The tree below
# OBJTOP mirrors the source tree.
OBJTOP := $(TOPDIR)/_out/$(TARGET_TRIPLE)

# Staging area for things one component builds and another consumes,
# playing the role of /usr/lib on an installed system.
LIBDIR := $(OBJTOP)/lib
LIBC   := $(LIBDIR)/libc.a
CRT1   := $(LIBDIR)/crt1.o

USER_LDSCRIPT := $(TOPDIR)/share/ld/user-$(ARCH).ld
KERN_LDSCRIPT := $(TOPDIR)/sys/arch/$(ARCH)/conf/kern.ld

MOTD_ELF  := $(OBJTOP)/bin/motd/motd
MOTD_BIN  := $(OBJTOP)/bin/motd/motd.bin
KERNEL_ELF := $(OBJTOP)/sys/kernel.elf

MKFSIMG      := $(OBJTOP)/tools/mkfsimg/mkfsimg
INITRAMFS_IMG := $(OBJTOP)/sys/initramfs.img
