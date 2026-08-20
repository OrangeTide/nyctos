# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>

include $(TOPDIR)/share/mk/nyc.own.mk

PROG := spin
SRCS := spin.c

# Live at the same time as nothing else, but slots are assigned once and by
# hand, so it gets its own. See share/mk/nyc.prog.mk.
PROG_LOAD_SLOT := 3

CFLAGS   += $(USER_CFLAGS)
CPPFLAGS := -I$(TOPDIR)/include -I$(TOPDIR)/sys

include $(TOPDIR)/share/mk/nyc.prog.mk
