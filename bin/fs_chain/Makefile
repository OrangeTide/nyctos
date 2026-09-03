# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>

include $(TOPDIR)/share/mk/nyc.own.mk

PROG := fs_chain
SRCS := fs_chain.c

# Its own slot, because there is no MMU at Tier 0 and every program that
# can be live at once runs at the address it was linked for.
PROG_LOAD_SLOT := 27

CFLAGS   += $(USER_CFLAGS)
CPPFLAGS := -I$(TOPDIR)/include -I$(TOPDIR)/sys

include $(TOPDIR)/share/mk/nyc.prog.mk
