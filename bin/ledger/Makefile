# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# PUBLIC DOMAIN (CC0-1.0)

include $(TOPDIR)/share/mk/nyc.own.mk

PROG := ledger
SRCS := ledger.c

# See share/mk/nyc.prog.mk.
PROG_LOAD_SLOT := 34

CFLAGS   += $(USER_CFLAGS)
CPPFLAGS := -I$(TOPDIR)/include -I$(TOPDIR)/sys

include $(TOPDIR)/share/mk/nyc.prog.mk
