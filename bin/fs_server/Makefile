# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>

include $(TOPDIR)/share/mk/nyc.own.mk

PROG := fs_server
SRCS := fs_server.c

# Both programs are live at once and there is no MMU, so they need
# different slots. See share/mk/nyc.prog.mk.
PROG_LOAD_SLOT := 0

CFLAGS   += $(USER_CFLAGS)
CPPFLAGS := -I$(TOPDIR)/include -I$(TOPDIR)/sys

include $(TOPDIR)/share/mk/nyc.prog.mk
