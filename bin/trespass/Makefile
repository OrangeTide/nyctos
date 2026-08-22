# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# PUBLIC DOMAIN (CC0-1.0)

include $(TOPDIR)/share/mk/nyc.own.mk

PROG := trespass
SRCS := trespass.c

# Live at the same time as the fs server, so it needs a slot of its own.
# See share/mk/nyc.prog.mk.
PROG_LOAD_SLOT := 12

CFLAGS   += $(USER_CFLAGS)
CPPFLAGS := -I$(TOPDIR)/include -I$(TOPDIR)/sys

include $(TOPDIR)/share/mk/nyc.prog.mk
