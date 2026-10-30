# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>

include $(TOPDIR)/share/mk/nyc.own.mk

PROG := fs_make
SRCS := fs_make.c

# Live alongside the servers at Tier 0, where there is no MMU, so it needs
# a slot of its own. See share/mk/nyc.prog.mk.
PROG_LOAD_SLOT := 43

CFLAGS   += $(USER_CFLAGS)
CPPFLAGS := -I$(TOPDIR)/include -I$(TOPDIR)/sys

include $(TOPDIR)/share/mk/nyc.prog.mk
