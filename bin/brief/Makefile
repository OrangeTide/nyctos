# PUBLIC DOMAIN (CC0-1.0)

include $(TOPDIR)/share/mk/nyc.own.mk

PROG := brief
SRCS := brief.c

# The first slot after every other program. Nothing else runs while it
# does, so it could share one, but a slot of its own keeps the archive
# readable as a list of what exists rather than of what overlaps.
PROG_LOAD_SLOT := 28

CFLAGS   += $(USER_CFLAGS)
CPPFLAGS := -I$(TOPDIR)/include -I$(TOPDIR)/sys

include $(TOPDIR)/share/mk/nyc.prog.mk
