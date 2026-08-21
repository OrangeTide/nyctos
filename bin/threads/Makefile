# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>

include $(TOPDIR)/share/mk/nyc.own.mk

PROG := threads
SRCS := threads.c

# Live at the same time as its parent, so its own slot. See
# share/mk/nyc.prog.mk.
PROG_LOAD_SLOT := 6

CFLAGS   += $(USER_CFLAGS) -DDEBUG_CONSOLE=1
CPPFLAGS := -I$(TOPDIR)/include -I$(TOPDIR)/sys

include $(TOPDIR)/share/mk/nyc.prog.mk
