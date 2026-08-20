# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>

include $(TOPDIR)/share/mk/nyc.own.mk

PROG := fs_batch
SRCS := fs_batch.c

# Both programs are live at once and there is no MMU, so they need
# different slots. See share/mk/nyc.prog.mk.
PROG_LOAD_SLOT := 2

# Built with its diagnostics on, to exercise the macro layer of
# include/debug.h. Without this the DBG_ calls leave no trace in the image.
CFLAGS   += $(USER_CFLAGS) -DDEBUG_CONSOLE=1
CPPFLAGS := -I$(TOPDIR)/include -I$(TOPDIR)/sys

include $(TOPDIR)/share/mk/nyc.prog.mk
