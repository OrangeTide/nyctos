# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>

include $(TOPDIR)/share/mk/nyc.own.mk

LIB := c

SRCS := arch/$(ARCH)/sys/syscall.S \
        sys/write.c \
        sys/open.c \
        sys/read.c \
        sys/close.c \
        sys/_exit.c \
        stdio/puts.c \
        stdlib/exit.c \
        string/memcpy.c \
        string/memset.c \
        string/strlen.c

CFLAGS   += $(USER_CFLAGS)
CPPFLAGS := -I$(TOPDIR)/include -I$(TOPDIR)/sys -I$(CURDIR)/include

include $(TOPDIR)/share/mk/nyc.lib.mk
