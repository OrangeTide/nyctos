# SPDX-License-Identifier: 0BSD OR Apache-2.0
#
# Copyright (C) 2026 Jon Mayo <jon@rm-f.net>
#
# The C runtime startup object, staged in LIBDIR next to libc.a.

include $(TOPDIR)/share/mk/nyc.own.mk

SRCS := $(ARCH)/crt1.S

CFLAGS   += $(USER_CFLAGS)
CPPFLAGS :=

include $(TOPDIR)/share/mk/nyc.obj.mk

all: $(CRT1)

$(CRT1): $(OBJS)
	@mkdir -p $(@D)
	cp $(OBJS) $@
