# PUBLIC DOMAIN (CC0-1.0)

include $(TOPDIR)/share/mk/nyc.own.mk

PROG := patient
SRCS := patient.c

# See share/mk/nyc.prog.mk.
PROG_LOAD_SLOT := 31

CFLAGS   += $(USER_CFLAGS)
CPPFLAGS := -I$(TOPDIR)/include -I$(TOPDIR)/sys

include $(TOPDIR)/share/mk/nyc.prog.mk
