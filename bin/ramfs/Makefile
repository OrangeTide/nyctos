include $(TOPDIR)/share/mk/nyc.own.mk

PROG := ramfs
SRCS := ramfs.c

# Live at the same time as every other program, so it needs its own slot.
# See share/mk/nyc.prog.mk.
PROG_LOAD_SLOT := 39

CFLAGS   += $(USER_CFLAGS)
CPPFLAGS := -I$(TOPDIR)/include -I$(TOPDIR)/sys

include $(TOPDIR)/share/mk/nyc.prog.mk
