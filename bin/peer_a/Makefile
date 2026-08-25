# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# PUBLIC DOMAIN (CC0-1.0)

include $(TOPDIR)/share/mk/nyc.own.mk

PROG := peer_a
SRCS := peer_a.c

# See share/mk/nyc.prog.mk.
PROG_LOAD_SLOT := 19

CFLAGS   += $(USER_CFLAGS)
CPPFLAGS := -I$(TOPDIR)/include -I$(TOPDIR)/sys

include $(TOPDIR)/share/mk/nyc.prog.mk
